
/* proc on plan 9 has lots of capabilities, some of which we might
 * want for akaros:
 * debug control
 * event tracing
 * process control (no need for signal system call, etc.)
 * textual status
 * rather than excise code that won't work, I'm bracketing it with
 * #if 0 until we know we don't want it
 */
#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>


enum
{
	Qdir,
	Qtrace,
	Qtracepids,
	Qargs,
	Qctl,
	Qfd,
	Qfpregs,
	Qkregs,
	Qmem,
	Qnote,
	Qnoteid,
	Qnotepg,
	Qns,
	Qproc,
	Qregs,
	Qsegment,
	Qstatus,
	Qtext,
	Qwait,
	Qprofile,
	Qsyscall,
	Qcore,
};

enum
{
	CMclose,
	CMclosefiles,
	CMfixedpri,
	CMhang,
	CMkill,
	CMnohang,
	CMnoswap,
	CMpri,
	CMprivate,
	CMprofile,
	CMstart,
	CMstartstop,
	CMstartsyscall,
	CMstop,
	CMwaitstop,
	CMwired,
	CMtrace,
	/* real time */
	CMperiod,
	CMdeadline,
	CMcost,
	CMsporadic,
	CMdeadlinenotes,
	CMadmit,
	CMextra,
	CMexpel,
	CMevent,
	CMcore,
};

enum{
	Nevents = 0x4000,
	Emask = Nevents - 1,
	Ntracedpids = 1024,
};

/* + 6 * 12 for extra NIX counters. */
#define STATSIZE	(2*KNAMELEN+12+9*12 +6*12)

/*
 * Status, fd, and ns are left fully readable (0444) because of their use in debugging,
 * particularly on shared servers.
 * Arguably, ns and fd shouldn't be readable; if you'd prefer, change them to 0000
 */
struct dirtab procdir[] =
{
	{"args",		{Qargs},	0,			0660},
	{"ctl",		{Qctl},		0,			0000},
	{"fd",		{Qfd},		0,			0444},
	{"fpregs",	{Qfpregs},	0,			0000},
	//	{"kregs",	{Qkregs},	sizeof(Ureg),		0600},
	{"mem",		{Qmem},		0,			0000},
	{"note",		{Qnote},	0,			0000},
	{"noteid",	{Qnoteid},	0,			0664},
	{"notepg",	{Qnotepg},	0,			0000},
	{"ns",		{Qns},		0,			0444},
	{"proc",		{Qproc},	0,			0400},
	//	{"regs",		{Qregs},	sizeof(Ureg),		0000},
	{"segment",	{Qsegment},	0,			0444},
	{"status",	{Qstatus},	STATSIZE,		0444},
	{"text",		{Qtext},	0,			0000},
	{"wait",		{Qwait},	0,			0400},
	{"profile",	{Qprofile},	0,			0400},
	{"syscall",	{Qsyscall},	0,			0400},	
	{"core",		{Qcore},	0,			0444},
};

static
struct cmdtab proccmd[] = {
	{CMclose,		"close",		2},
	{CMclosefiles,		"closefiles",		1},
	{CMfixedpri,		"fixedpri",		2},
	{CMhang,			"hang",			1},
	{CMnohang,		"nohang",		1},
	{CMnoswap,		"noswap",		1},
	{CMkill,			"kill",			1},
	{CMpri,			"pri",			2},
	{CMprivate,		"private",		1},
	{CMprofile,		"profile",		1},
	{CMstart,		"start",		1},
	{CMstartstop,		"startstop",		1},
	{CMstartsyscall,		"startsyscall",		1},
	{CMstop,			"stop",			1},
	{CMwaitstop,		"waitstop",		1},
	{CMwired,		"wired",		2},
	{CMtrace,		"trace",		0},
	{CMperiod,		"period",		2},
	{CMdeadline,		"deadline",		2},
	{CMcost,			"cost",			2},
	{CMsporadic,		"sporadic",		1},
	{CMdeadlinenotes,	"deadlinenotes",	1},
	{CMadmit,		"admit",		1},
	{CMextra,		"extra",		1},
	{CMexpel,		"expel",		1},
	{CMevent,		"event",		1},
	{CMcore,			"core",			2},
};

/* Segment type from portdat.h */
static char *sname[]={ "Text", "Data", "Bss", "Stack", "Shared", "Phys", };

/*
 * struct qids are, in path:
 *	 4 bits of file type (qids above)
 *	23 bits of process slot number + 1
 *	     in vers,
 *	32 bits of pid, for consistency checking
 * If notepg, c->pgrpid.path is pgrp slot, .vers is noteid.
 */
#define	QSHIFT	5	/* location in qid of proc slot # */
#define	SLOTBITS 23	/* number of bits in the slot */
#define	QIDMASK	((1<<QSHIFT)-1)
#define	SLOTMASK	(((1<<SLOTBITS)-1) << QSHIFT)

#define QID(q)		((((uint32_t)(q).path)&QIDMASK)>>0)
#define SLOT(q)		(((((uint32_t)(q).path)&SLOTMASK)>>QSHIFT)-1)
#define PID(q)		((q).vers)
#define NOTEID(q)	((q).vers)

static void	procctlreq(struct proc*, char*, int);
static int	procctlmemio(struct proc*, uintptr_t, int, void*, int);
//static struct chan*	proctext(struct chan*, struct proc*);
//static Segment* txt2data(struct proc*, Segment*);
//static int	procstopped(void*);
static void	mntscan(struct mntwalk*, struct proc*);

//static Traceevent *tevents;
static char *tpids, *tpidsc, *tpidse;
static spinlock_t tlock;
static int topens;
static int tproduced, tconsumed;
//static void notrace(struct proc*, int, int64_t);

//void (*proctrace)(struct proc*, int, int64_t) = notrace;

#if 0
static void
profclock(Ureg *ur, Timer *)
{
	Tos *tos;

	if(up == NULL || current->state != Running)
		return;

	/* user profiling clock */
	if(userureg(ur)){
		tos = (Tos*)(USTKTOP-sizeof(Tos));
		tos->clock += TK2MS(1);
		segclock(userpc(ur));
	}
}
#endif
static int
procgen(struct chan *c, char *name, struct dirtab *tab, int unused, int s, struct dir *dp, struct errbuf *perrbuf)
{
	struct qid qid;
	struct proc *p;
	char *ename;

	int pid;
	uint32_t path, perm, len;

	if(s == DEVDOTDOT){
		mkqid(&qid, Qdir, 0, QTDIR, perrbuf);
		devdir(c, qid, "#p", 0, eve, 0555, dp);
		return 1;
	}

	if(c->qid.path == Qdir){
		if(s == 0){
			strncpy(current->genbuf,  "trace", sizeof(current->genbuf));
			mkqid(&qid, Qtrace, -1, QTFILE, perrbuf);
			devdir(c, qid, current->genbuf, 0, eve, 0444, dp);
			return 1;
		}
		if(s == 1){
			strncpy(current->genbuf,  "tracepids", sizeof(current->genbuf));
			mkqid(&qid, Qtracepids, -1, QTFILE, perrbuf);
			devdir(c, qid, current->genbuf, 0, eve, 0444, dp);
			return 1;
		}
		s -= 2;
		if(name != NULL){
			/* ignore s and use name to find pid */
			pid = strtol(name, &ename, 10);
			if(pid<=0 || ename[0]!='\0')
				return -1;
			p = pid2proc(pid);
			if(! p)
				return -1;
		}
		else {
			pid = s;
			p = pid2proc(pid);
			if(! p)
				return -1;
		}

		snprintf(current->genbuf, sizeof current->genbuf, "%ud", pid);
		/*
		 * String comparison is done in devwalk so
		 * name must match its formatted pid.
		 */
		if(name != NULL && strcmp(name, current->genbuf) != 0)
			return -1;
		mkqid(&qid, (s+1)<<QSHIFT, pid, QTDIR, perrbuf);
		devdir(c, qid, current->genbuf, 0, p->user, DMDIR|0555, dp);
		kref_put(&p->p_kref);
		return 1;
	}
	if(c->qid.path == Qtrace){
		strncpy(current->genbuf,  "trace", sizeof(current->genbuf));
		mkqid(&qid, Qtrace, -1, QTFILE, perrbuf);
		devdir(c, qid, current->genbuf, 0, eve, 0444, dp);
		return 1;
	}
	if(c->qid.path == Qtracepids){
		strncpy(current->genbuf,  "tracepids", sizeof(current->genbuf));
		mkqid(&qid, Qtrace, -1, QTFILE, perrbuf);
		devdir(c, qid, current->genbuf, 0, eve, 0444, dp);
		return 1;
	}
	if(s >= ARRAY_SIZE(procdir))
		return -1;
	if(tab)
		panic("procgen");

	tab = &procdir[s];
	path = c->qid.path&~(((1<<QSHIFT)-1));	/* slot component */

	if((p = pid2proc(SLOT(c->qid))) == NULL)
		return -1;
	perm = 0444 | tab->perm;
#if 0
	if(perm == 0)
		perm = p->procmode;
	else	/* just copy read bits */
		perm |= p->procmode & 0444;
#endif

	len = tab->length;
#if 0
	switch(QID(c->qid)) {
	case Qwait:
		len = p->nwait;	/* incorrect size, but >0 means there's something to read */
		break;
	case Qprofile:
		q = p->seg[TSEG];
		if(q && q->profile) {
			len = (q->top-q->base)>>LRESPROF;
			len *= sizeof(*q->profile);
		}
		break;
	}
#endif

	mkqid(&qid, path|tab->qid.path, c->qid.vers, QTFILE, perrbuf);
	devdir(c, qid, tab->name, len, p->user, perm, dp);
	kref_put(&p->p_kref);
	return 1;
}

#if 0
static void
notrace(struct proc*, Tevent, int64_t)
{
}
static spinlock_t lock tlck;

static void
_proctrace(struct proc* p, Tevent etype, int64_t ts)
{
	Traceevent *te;
	int tp;

	ilock(&tlck);
	if (p->trace == 0 || topens == 0 ||
		tproduced - tconsumed >= Nevents){
		iunlock(&tlck);
		return;
	}
	tp = tproduced++;
	iunlock(&tlck);

	te = &tevents[tp&Emask];
	te->pid = p->pid;
	te->etype = etype;
	if (ts == 0)
		te->time = todget(NULL);
	else
		te->time = ts;
	te->core = m->machno;
}

void
proctracepid(struct proc *p)
{
	if(p->trace == 1 && proctrace != notrace){
		p->trace = 2;
		ilock(&tlck);
		tpidsc = seprint(tpidsc, tpidse, "%d %s\n", p->pid, p->text);
		iunlock(&tlck);
	}
}
	
#endif
static void
procinit(void)
{
#if 0
	if(conf.nproc >= (SLOTMASK>>QSHIFT) - 1)
		printd("warning: too many procs for devproc\n");
	addclock0link((void (*)(void))profclock, 113);	/* Relative prime to HZ */
#endif
}

static struct chan*
procattach(char *spec, struct errbuf *perrbuf)
{
	return devattach('p', spec, perrbuf);
}

static struct walkqid*
procwalk(struct chan *c, struct chan *nc, char **name, int nname, struct errbuf *perrbuf)
{
	return devwalk(c, nc, name, nname, 0, 0, procgen, perrbuf);
}

static long
procstat(struct chan *c, uint8_t *db, long n, struct errbuf *perrbuf)
{
	return devstat(c, db, n, 0, 0, procgen, perrbuf);
}

/*
 *  none can't read or write state on other
 *  processes.  This is to contain access of
 *  servers running as none should they be
 *  subverted by, for example, a stack attack.
 */
static void
nonone(struct proc *p)
{
	return;
#if 0
	if(p == up)
		return;
	if(strcmp(current->user, "none") != 0)
		return;
	if(iseve())
		return;
	error(Eperm);
#endif
}

static struct chan*
procopen(struct chan *c, int omode, struct errbuf *perrbuf)
{
	ERRSTACK(2);
	struct proc *p;
	struct pgrp *pg;
	struct chan *tc;
	int pid;

	if(c->qid.type & QTDIR)
		return devopen(c, omode, 0, 0, procgen, perrbuf);

	if(QID(c->qid) == Qtrace){
		error("not yet");
#if 0
		if (omode != OREAD)
			error(Eperm);
		lock(&tlock);
		if (waserror()){
			unlock(&tlock);
			nexterror();
		}
		if (topens > 0)
			error("already open");
		topens++;
		if (tevents == NULL){
			tevents = (Traceevent*)kmalloc(sizeof(Traceevent) * Nevents, 0);
			if(tevents == NULL)
				error(Enomem);
			tpids = kmalloc(Ntracedpids * 20, 0);
			if(tpids == NULL){
				kfree(tpids);
				tpids = NULL;
				error(Enomem);
			}
			tpidsc = tpids;
			tpidse = tpids + Ntracedpids * 20;
			*tpidsc = 0;
			tproduced = tconsumed = 0;
		}
		proctrace = _proctrace;
		unlock(&tlock);

		c->mode = openmode(omode, perrbuf);
		c->flag |= COPEN;
		c->offset = 0;
		return c;
#endif
	}
	if(QID(c->qid) == Qtracepids){
		error("not yet");
#if 0
		if (omode != OREAD)
			error(Eperm);
		c->mode = openmode(omode, perrbuf);
		c->flag |= COPEN;
		c->offset = 0;
		return c;
#endif
	}
#if 0
	if((p = pid2proc(SLOT(c->qid))) == NULL)
		error(Eprocdied);
	qlock(&p->debug);
	if(waserror()){
		qunlock(&p->debug);
		kref_put(&p->p_kref);
		nexterror();
	}
	pid = PID(c->qid);
	if(p->pid != pid)
		error(Eprocdied);

	omode = openmode(omode, perrbuf);

	switch(QID(c->qid)){
	case Qtext:
		if(omode != OREAD)
			error(Eperm);
		tc = proctext(c, p);
		tc->offset = 0;
		qunlock(&p->debug);
		kref_put(&p->p_kref);
		cclose(c);
		return tc;

	case Qproc:
	case Qsegment:
	case Qprofile:
	case Qfd:
		if(omode != OREAD)
			error(Eperm);
		break;

	case Qnote:
		if(p->privatemem)
			error(Eperm);
		break;

	case Qmem:
	case Qctl:
		if(p->privatemem)
			error(Eperm);
		nonone(p);
		break;

	case Qargs:
	case Qnoteid:
	case Qstatus:
	case Qwait:
	case Qregs:
	case Qfpregs:
	case Qkregs:
	case Qsyscall:
	case Qcore:
		nonone(p);
		break;

	case Qns:
		if(omode != OREAD)
			error(Eperm);
		c->aux = kmalloc(sizeof(Mntwalk), 0);
		break;

	case Qnotepg:
		nonone(p);
		pg = p->pgrp;
		if(pg == NULL)
			error(Eprocdied);
		if(omode!=OWRITE || pg->pgrpid == 1)
			error(Eperm);
		c->pgrpid.path = pg->pgrpid+1;
		c->pgrpid.vers = p->noteid;
		break;


	default:
#endif
#if 0
		qunlock(&p->debug);
		kref_put(&p->p_kref);
		pprint("procopen %#llux\n", c->qid.path);
#endif
		error(Egreg);
#if 0
	}


	/* Affix pid to qid */
	if(p->state != Dead)
		c->qid.vers = p->pid;
	/* make sure the process slot didn't get reallocated while we were playing */
	coherence();
	if(p->pid != pid)
		error(Eprocdied);

#endif
	tc = devopen(c, omode, 0, 0, procgen, perrbuf);
#if 0
	qunlock(&p->debug);
	kref_put(&p->p_kref);

#endif
	return tc;
}

static long
procwstat(struct chan *c, uint8_t *db, long n, struct errbuf *perrbuf)
{
	ERRSTACK(2);
	error("not yet");
#if 0
	struct proc *p;
	struct dir *d;

	if(c->qid.type & QTDIR)
		error(Eperm);

	if(QID(c->qid) == Qtrace)
		return devwstat(c, db, n, perrbuf);

	if((p = pid2proc(SLOT(c->qid))) == NULL)
		error(Eprocdied);
	nonone(p);
	d = NULL;
	qlock(&p->debug);
	if(waserror()){
		qunlock(&p->debug);
		kref_put(&p->p_kref);
		kfree(d);
		nexterror();
	}

	if(p->pid != PID(c->qid))
		error(Eprocdied);

	if(strcmp(current->user, p->user) != 0 && strcmp(current->user, eve) != 0)
		error(Eperm);

	d = kmalloc(sizeof(struct dir) + n, 0);
	n = convM2D(db, n, &d[0], (char*)&d[1]);
	if(n == 0)
		error(Eshortstat);
	if(!emptystr(d->uid) && strcmp(d->uid, p->user) != 0){
		if(strcmp(current->user, eve) != 0)
			error(Eperm);
		else
			kstrdup(&p->user, d->uid);
	}
	if(d->mode != ~0UL)
		p->procmode = d->mode&0777;

	qunlock(&p->debug);
	kref_put(&p->p_kref);
	kfree(d);

	return n;
#endif
}


#if 0
static long
procoffset(long offset, char *va, int *np)
{
	if(offset > 0) {
		offset -= *np;
		if(offset < 0) {
			memmove(va, va+*np+offset, -offset);
			*np = -offset;
		}
		else
			*np = 0;
	}
	return offset;
}

static int
procqidwidth(struct chan *c)
{
	char buf[32];

	return sprint(buf, "%lud", c->qid.vers);
}

int
procfdprint(struct chan *c, int fd, int w, char *s, int ns)
{
	int n;

	if(w == 0)
		w = procqidwidth(c);
	n = snprint(s, ns, "%3d %.2s %C %4ud (%.16llux %*lud %.2ux) %5ld %8lld %s\n",
		fd,
		&"r w rw"[(c->mode&3)<<1],
		c->dev->dc, c->devno,
		c->qid.path, w, c->qid.vers, c->qid.type,
		c->iounit, c->offset, c->path->s);
	return n;
}

static int
procfds(struct proc *p, char *va, int count, long offset)
{
	ERRSTACK(2);
	struct fgrp *f;
	struct chan *c;
	char buf[256];
	int n, i, w, ww;
	char *a;

	/* print to buf to avoid holding fgrp lock while writing to user space */
	if(count > sizeof buf)
		count = sizeof buf;
	a = buf;

	qlock(&p->debug);
	f = p->fgrp;
	if(f == NULL){
		qunlock(&p->debug);
		return 0;
	}
	lock(f);
	if(waserror()){
		unlock(f);
		qunlock(&p->debug);
		nexterror();
	}

	n = readstr(0, a, count, p->dot->path->s);
	n += snprint(a+n, count-n, "\n");
	offset = procoffset(offset, a, &n);
	/* compute width of qid.path */
	w = 0;
	for(i = 0; i <= f->maxfd; i++) {
		c = f->fd[i];
		if(c == NULL)
			continue;
		ww = procqidwidth(c);
		if(ww > w)
			w = ww;
	}
	for(i = 0; i <= f->maxfd; i++) {
		c = f->fd[i];
		if(c == NULL)
			continue;
		n += procfdprint(c, i, w, a+n, count-n);
		offset = procoffset(offset, a, &n);
	}
	unlock(f);
	qunlock(&p->debug);

	/* copy result to user space, now that locks are released */
	memmove(va, buf, n);

	return n;
}
#endif
static void
procclose(struct chan * c, struct errbuf *perrbuf)
{
	if(QID(c->qid) == Qtrace){
		spin_lock(&tlock);
		if(topens > 0)
			topens--;
		/* ??
		if(topens == 0)
			proctrace = notrace;
		*/
		spin_unlock(&tlock);
	}
	if(QID(c->qid) == Qns && c->aux != 0)
		kfree(c->aux);
}

#if 0
static void
int2flag(int flag, char *s)
{
	if(flag == 0){
		*s = '\0';
		return;
	}
	*s++ = '-';
	if(flag & MAFTER)
		*s++ = 'a';
	if(flag & MBEFORE)
		*s++ = 'b';
	if(flag & MCREATE)
		*s++ = 'c';
	if(flag & MCACHE)
		*s++ = 'C';
	*s = '\0';
}

static char*
argcpy(char *s, char *p)
{
	char *t, *tp, *te;
	int n;

	n = p - s;
	if(n > 128)
		n = 128;
	if(n <= 0){
		t = kmalloc(1, 0);
		*t = 0;
		return t;
	}
	t = kmalloc(n, 0);
	tp = t;
	te = t+n;

	while(tp + 1 < te){
		for(p--; p>s && p[-1] != 0; p--)
			;
		tp = seprint(tp, te, "%q ", p);
		if(p == s)
			break;
	}
	if(*tp == ' ')
		*tp = 0;
	return t;
}

static int
procargs(struct proc *p, char *buf, int nbuf)
{
	char *s;

	if(p->setargs == 0){
		s = argcpy(p->args, p->args+p->nargs);
		kfree(p->args);
		p->nargs = strlen(s);
		p->args = s;
		p->setargs = 1;
	}
	return snprint(buf, nbuf, "%s", p->args);
}

static int
eventsavailable(void *)
{
	return tproduced > tconsumed;
}
#endif
static long
procread(struct chan *c, void *va, long n, int64_t off, struct errbuf *perrbuf)
{
#if 0
	struct proc *p;
	Mach *ac, *wired;
	long l, r;
	Waitq *wq;
	Ureg kur;
	uint8_t *rptr;
	Confmem *cm;
	Mntwalk *mw;
	Segment *sg, *s;
	int i, j, navail, pid, rsize;
	char flag[10], *sps, *srv, statbuf[NSEG*64];
	uintptr_t offset, u;
	int tesz;
#endif
	if(c->qid.type & QTDIR)
		return devdirread(c, va, n, 0, 0, procgen, perrbuf);

#if 0
	offset = off;

	if(QID(c->qid) == Qtrace){
		if(!eventsavailable(NULL))
			return 0;

		rptr = va;
		tesz = BIT32SZ + BIT32SZ + BIT64SZ + BIT32SZ;
		navail = tproduced - tconsumed;
		if(navail > n / tesz)
			navail = n / tesz;
		while(navail > 0) {
			PBIT32(rptr, tevents[tconsumed & Emask].pid);
			rptr += BIT32SZ;
			PBIT32(rptr, tevents[tconsumed & Emask].etype);
			rptr += BIT32SZ;
			PBIT64(rptr, tevents[tconsumed & Emask].time);
			rptr += BIT64SZ;
			PBIT32(rptr, tevents[tconsumed & Emask].core);
			rptr += BIT32SZ;
			tconsumed++;
			navail--;
		}
		return rptr - (uint8_t*)va;
	}

	if(QID(c->qid) == Qtracepids)
		if(tpids == NULL)
			return 0;
		else
			return readstr(off, va, n, tpids);

	if((p = pid2proc(SLOT(c->qid))) == NULL)
		error(Eprocdied);
	if(p->pid != PID(c->qid)){
		kref_put(&p->p_kref);
		error(Eprocdied);
	}

	switch(QID(c->qid)){
	default:
		kref_put(&p->p_kref);
		break;
	case Qargs:
		qlock(&p->debug);
		j = procargs(p, current->genbuf, sizeof current->genbuf);
		qunlock(&p->debug);
		kref_put(&p->p_kref);
		if(offset >= j)
			return 0;
		if(offset+n > j)
			n = j-offset;
		memmove(va, &current->genbuf[offset], n);
		return n;

	case Qsyscall:
		if(p->syscalltrace == NULL)
			return 0;
		return readstr(offset, va, n, p->syscalltrace);

	case Qcore:
		i = 0;
		ac = p->ac;
		wired = p->wired;
		if(ac != NULL)
			i = ac->machno;
		else if(wired != NULL)
			i = wired->machno;
		snprint(statbuf, sizeof statbuf, "%d\n", i);
		return readstr(offset, va, n, statbuf);

	case Qmem:
		if(offset < KZERO
		|| (offset >= USTKTOP-USTKSIZE && offset < USTKTOP)){
			r = procctlmemio(p, offset, n, va, 1);
			kref_put(&p->p_kref);
			return r;
		}

		if(!iseve()){
			kref_put(&p->p_kref);
			error(Eperm);
		}

		/* validate kernel addresses */
		if(offset < PTR2UINT(end)) {
			if(offset+n > PTR2UINT(end))
				n = PTR2UINT(end) - offset;
			memmove(va, UINT2PTR(offset), n);
			kref_put(&p->p_kref);
			return n;
		}
		for(i=0; i<nelem(conf.mem); i++){
			cm = &conf.mem[i];
			/* klimit-1 because klimit might be zero! */
			if(cm->kbase <= offset && offset <= cm->klimit-1){
				if(offset+n >= cm->klimit-1)
					n = cm->klimit - offset;
				memmove(va, UINT2PTR(offset), n);
				kref_put(&p->p_kref);
				return n;
			}
		}
		kref_put(&p->p_kref);
		error(Ebadarg);

	case Qprofile:
		s = p->seg[TSEG];
		if(s == 0 || s->profile == 0)
			error("profile is off");
		i = (s->top-s->base)>>LRESPROF;
		i *= sizeof(*s->profile);
		if(offset >= i){
			kref_put(&p->p_kref);
			return 0;
		}
		if(offset+n > i)
			n = i - offset;
		memmove(va, ((char*)s->profile)+offset, n);
		kref_put(&p->p_kref);
		return n;

	case Qnote:
		qlock(&p->debug);
		if(waserror()){
			qunlock(&p->debug);
			kref_put(&p->p_kref);
			nexterror();
		}
		if(p->pid != PID(c->qid))
			error(Eprocdied);
		if(n < 1)	/* must accept at least the '\0' */
			error(Etoosmall);
		if(p->nnote == 0)
			n = 0;
		else {
			i = strlen(p->note[0].msg) + 1;
			if(i > n)
				i = n;
			rptr = va;
			memmove(rptr, p->note[0].msg, i);
			rptr[i-1] = '\0';
			p->nnote--;
			memmove(p->note, p->note+1, p->nnote*sizeof(Note));
			n = i;
		}
		if(p->nnote == 0)
			p->notepending = 0;
		qunlock(&p->debug);
		kref_put(&p->p_kref);
		return n;

	case Qproc:
		if(offset >= sizeof(struct proc)){
			kref_put(&p->p_kref);
			return 0;
		}
		if(offset+n > sizeof(struct proc))
			n = sizeof(struct proc) - offset;
		memmove(va, ((char*)p)+offset, n);
		kref_put(&p->p_kref);
		return n;

	case Qregs:
		rptr = (uint8_t*)p->dbgreg;
		rsize = sizeof(Ureg);
	regread:
		if(rptr == 0){
			kref_put(&p->p_kref);
			error(Enoreg);
		}
		if(offset >= rsize){
			kref_put(&p->p_kref);
			return 0;
		}
		if(offset+n > rsize)
			n = rsize - offset;
		memmove(va, rptr+offset, n);
		kref_put(&p->p_kref);
		return n;

	case Qkregs:
		memset(&kur, 0, sizeof(Ureg));
		setkernur(&kur, p);
		rptr = (uint8_t*)&kur;
		rsize = sizeof(Ureg);
		goto regread;

	case Qfpregs:
		r = fpudevprocio(p, va, n, offset, 0);
		kref_put(&p->p_kref);
		return r;

	case Qstatus:
		if(offset >= STATSIZE){
			kref_put(&p->p_kref);
			return 0;
		}
		if(offset+n > STATSIZE)
			n = STATSIZE - offset;

		sps = p->psstate;
		if(sps == 0)
			sps = statename[p->state];
		memset(statbuf, ' ', sizeof statbuf);
		j = 2*KNAMELEN + 12;
		snprint(statbuf, j+1, "%-*.*s%-*.*s%-12.11s",
			KNAMELEN, KNAMELEN-1, p->text,
			KNAMELEN, KNAMELEN-1, p->user,
			sps);

		for(i = 0; i < 6; i++) {
			l = p->time[i];
			if(i == TReal)
				l = sys->ticks - l;
			l = TK2MS(l);
			readnum(0, statbuf+j+NUMSIZE*i, NUMSIZE, l, NUMSIZE);
		}
		/* ignore stack, which is mostly non-existent */
		u = 0;
		for(i=1; i<NSEG; i++){
			s = p->seg[i];
			if(s)
				u += s->top - s->base;
		}
		readnum(0, statbuf+j+NUMSIZE*6, NUMSIZE, u>>10u, NUMSIZE);	/* wrong size */
		readnum(0, statbuf+j+NUMSIZE*7, NUMSIZE, p->basepri, NUMSIZE);
		readnum(0, statbuf+j+NUMSIZE*8, NUMSIZE, p->priority, NUMSIZE);

		/*
		 * NIX: added # of traps, syscalls, and iccs
		 */
		readnum(0, statbuf+j+NUMSIZE*9, NUMSIZE, p->ntrap, NUMSIZE);
		readnum(0, statbuf+j+NUMSIZE*10, NUMSIZE, p->nintr, NUMSIZE);
		readnum(0, statbuf+j+NUMSIZE*11, NUMSIZE, p->nsyscall, NUMSIZE);
		readnum(0, statbuf+j+NUMSIZE*12, NUMSIZE, p->nicc, NUMSIZE);
		readnum(0, statbuf+j+NUMSIZE*13, NUMSIZE, p->nactrap, NUMSIZE);
		readnum(0, statbuf+j+NUMSIZE*14, NUMSIZE, p->nacsyscall, NUMSIZE);
		memmove(va, statbuf+offset, n);
		kref_put(&p->p_kref);
		return n;

	case Qsegment:
		j = 0;
		for(i = 0; i < NSEG; i++) {
			sg = p->seg[i];
			if(sg == 0)
				continue;
			j += sprint(statbuf+j, "%-6s %c%c %p %p %4d\n",
				sname[sg->type&SG_TYPE],
				sg->type&SG_RONLY ? 'R' : ' ',
				sg->profile ? 'P' : ' ',
				sg->base, sg->top, sg->ref);
		}
		kref_put(&p->p_kref);
		if(offset >= j)
			return 0;
		if(offset+n > j)
			n = j-offset;
		if(n == 0 && offset == 0)
			exhausted("segments");
		memmove(va, &statbuf[offset], n);
		return n;

	case Qwait:
		if(!canqlock(&p->qwaitr)){
			kref_put(&p->p_kref);
			error(Einuse);
		}

		if(waserror()) {
			qunlock(&p->qwaitr);
			kref_put(&p->p_kref);
			nexterror();
		}

		lock(&p->exl);
		if(up == p && p->nchild == 0 && p->waitq == 0) {
			unlock(&p->exl);
			error(Enochild);
		}
		pid = p->pid;
		while(p->waitq == 0) {
			unlock(&p->exl);
			sleep(&p->waitr, haswaitq, p);
			if(p->pid != pid)
				error(Eprocdied);
			lock(&p->exl);
		}
		wq = p->waitq;
		p->waitq = wq->next;
		p->nwait--;
		unlock(&p->exl);

		qunlock(&p->qwaitr);
		kref_put(&p->p_kref);
		n = snprint(va, n, "%d %lud %lud %lud %q",
			wq->w.pid,
			wq->w.time[TUser], wq->w.time[TSys], wq->w.time[TReal],
			wq->w.msg);
		kfree(wq);
		return n;

	case Qns:
		qlock(&p->debug);
		if(waserror()){
			qunlock(&p->debug);
			kref_put(&p->p_kref);
			nexterror();
		}
		if(p->pgrp == NULL || p->pid != PID(c->qid))
			error(Eprocdied);
		mw = c->aux;
		if(mw->cddone){
			qunlock(&p->debug);
			kref_put(&p->p_kref);
			return 0;
		}
		mntscan(mw, p);
		if(mw->mh == 0){
			mw->cddone = 1;
			i = snprint(va, n, "cd %s\n", p->dot->path->s);
			qunlock(&p->debug);
			kref_put(&p->p_kref);
			return i;
		}
		int2flag(mw->cm->mflag, flag);
		if(strcmp(mw->cm->to->path->s, "#M") == 0){
			srv = srvname(mw->cm->to->mchan);
			i = snprint(va, n, "mount %s %s %s %s\n", flag,
				srv==NULL? mw->cm->to->mchan->path->s : srv,
				mw->mh->from->path->s, mw->cm->spec? mw->cm->spec : "");
			kfree(srv);
		}else
			i = snprint(va, n, "bind %s %s %s\n", flag,
				mw->cm->to->path->s, mw->mh->from->path->s);
		qunlock(&p->debug);
		kref_put(&p->p_kref);
		return i;

	case Qnoteid:
		r = readnum(offset, va, n, p->noteid, NUMSIZE);
		kref_put(&p->p_kref);
		return r;
	case Qfd:
		r = procfds(p, va, n, offset);
		kref_put(&p->p_kref);
		return r;
	}
#endif
	error(Egreg);
	return 0;			/* not reached */
}

#if 0
static void
mntscan(Mntwalk *mw, struct proc *p)
{
	Pgrp *pg;
	struct mount *t;
	struct mhead *f;
	int best, i, last, nxt;

	pg = p->pgrp;
	rlock(&pg->ns);

	nxt = 0;
	best = (int)(~0U>>1);		/* largest 2's complement int */

	last = 0;
	if(mw->mh)
		last = mw->cm->mountid;

	for(i = 0; i < MNTHASH; i++) {
		for(f = pg->mnthash[i]; f; f = f->hash) {
			for(t = f->mount; t; t = t->next) {
				if(mw->mh == 0 ||
				  (t->mountid > last && t->mountid < best)) {
					mw->cm = t;
					mw->mh = f;
					best = mw->cm->mountid;
					nxt = 1;
				}
			}
		}
	}
	if(nxt == 0)
		mw->mh = 0;

	runlock(&pg->ns);
}
#endif

static long
procwrite(struct chan *c, void *va, long n, int64_t off, struct errbuf *perrbuf)
{
	ERRSTACK(2);
	error("not yet");
	return 0;
#if 0
	struct proc *p, *t;
	int i, id, l;
	char *args, buf[ERRMAX];
	uintptr_t offset;

	if(c->qid.type & QTDIR)
		error(Eisdir);

	/* Use the remembered noteid in the channel rather
	 * than the process pgrpid
	 */
	if(QID(c->qid) == Qnotepg) {
		pgrpnote(NOTEID(c->pgrpid), va, n, NUser);
		return n;
	}

	if((p = pid2proc(SLOT(c->qid))) == NULL)
		error(Eprocdied);

	qlock(&p->debug);
	if(waserror()){
		qunlock(&p->debug);
		kref_put(&p->p_kref);
		nexterror();
	}
	if(p->pid != PID(c->qid))
		error(Eprocdied);

	offset = off;

	switch(QID(c->qid)){
	case Qargs:
		if(n == 0)
			error(Eshort);
		if(n >= sizeof buf - strlen(p->text) - 1)
			error(Etoobig);
		l = snprint(buf, sizeof buf, "%s [%s]", p->text, (char*)va);
		args = kmalloc(l + 1, 0);
		if(args == NULL)
			error(Enomem);
		memmove(args, buf, l);
		args[l] = 0;
		kfree(p->args);
		p->nargs = l;
		p->args = args;
		p->setargs = 1;
		break;

	case Qmem:
		if(p->state != Stopped)
			error(Ebadctl);

		n = procctlmemio(p, offset, n, va, 0);
		break;

	case Qregs:
		if(offset >= sizeof(Ureg))
			n = 0;
		else if(offset+n > sizeof(Ureg))
			n = sizeof(Ureg) - offset;
		if(p->dbgreg == 0)
			error(Enoreg);
		setregisters(p->dbgreg, (char*)(p->dbgreg)+offset, va, n);
		break;

	case Qfpregs:
		n = fpudevprocio(p, va, n, offset, 1);
		break;

	case Qctl:
		procctlreq(p, va, n);
		break;

	case Qnote:
		if(p->kp)
			error(Eperm);
		if(n >= ERRMAX-1)
			error(Etoobig);
		memmove(buf, va, n);
		buf[n] = 0;
		if(!postnote(p, 0, buf, NUser))
			error("note not posted");
		break;
	case Qnoteid:
		id = atoi(va);
		if(id == p->pid) {
			p->noteid = id;
			break;
		}
		for(i = 0; (t = pid2proc(i)) != NULL; i++){
			if(t->state == Dead || t->noteid != id){
				kref_put(&p->p_kref);
				continue;
			}
			if(strcmp(p->user, t->user) != 0){
				kref_put(&p->p_kref);
				error(Eperm);
			}
			kref_put(&p->p_kref);
			p->noteid = id;
			break;
		}
		if(p->noteid != id)
			error(Ebadarg);
		break;
	default:
		qunlock(&p->debug);
		kref_put(&p->p_kref);
		pprint("unknown qid %#llux in procwrite\n", c->qid.path);
		error(Egreg);
	}
	qunlock(&p->debug);
	kref_put(&p->p_kref);
	return n;
#endif
}

struct dev procdevtab = {
	'p',
	"proc",

	devreset,
	procinit,
	devshutdown,
	procattach,
	procwalk,
	procstat,
	procopen,
	devcreate,
	procclose,
	procread,
	devbread,
	procwrite,
	devbwrite,
	devremove,
	procwstat,
};

#if 0
static struct chan*
proctext(struct chan *c, struct proc *p)
{
	ERRSTACK(2);
	struct chan *tc;
	Image *i;
	Segment *s;

	s = p->seg[TSEG];
	if(s == 0)
		error(Enonexist);
	if(p->state==Dead)
		error(Eprocdied);

	lock(s);
	i = s->image;
	if(i == 0) {
		unlock(s);
		error(Eprocdied);
	}
	unlock(s);

	lock(i);
	if(waserror()) {
		unlock(i);
		nexterror();
	}

	tc = i->c;
	if(tc == 0)
		error(Eprocdied);

	if(kref_get(&tc->ref, 1) == 1 || (tc->flag&COPEN) == 0 || tc->mode!=OREAD) {
		cclose(tc);
		error(Eprocdied);
	}

	if(p->pid != PID(c->qid)){
		cclose(tc);
		error(Eprocdied);
	}

	unlock(i);

	return tc;
}

void
procstopwait(struct proc *p, int ctl)
{
	ERRSTACK(2);
	int pid;

	if(p->pdbg)
		error(Einuse);
	if(procstopped(p) || p->state == Broken)
		return;

	if(ctl != 0)
		p->procctl = ctl;
	p->pdbg = up;
	pid = p->pid;
	qunlock(&p->debug);
	current->psstate = "Stopwait";
	if(waserror()) {
		p->pdbg = 0;
		qlock(&p->debug);
		nexterror();
	}
	sleep(&current->sleep, procstopped, p);
	qlock(&p->debug);
	if(p->pid != pid)
		error(Eprocdied);
}

static void
procctlcloseone(struct proc *p, struct fgrp *f, int fd)
{
	struct chan *c;

	c = f->fd[fd];
	if(c == NULL)
		return;
	f->fd[fd] = NULL;
	unlock(f);
	qunlock(&p->debug);
	cclose(c);
	qlock(&p->debug);
	lock(f);
}

void
procctlclosefiles(struct proc *p, int all, int fd)
{
	int i;
	struct fgrp *f;

	f = p->fgrp;
	if(f == NULL)
		error(Eprocdied);

	lock(f);
	f->ref++;
	if(all)
		for(i = 0; i < f->maxfd; i++)
			procctlcloseone(p, f, i);
	else
		procctlcloseone(p, f, fd);
	unlock(f);
	closefgrp(f);
}

static char *
parsetime(int64_t *rt, char *s)
{
	uint64_t ticks;
	uint32_t l;
	char *e, *p;
	static int p10[] = {100000000, 10000000, 1000000, 100000, 10000, 1000, 100, 10, 1};

	if (s == NULL)
		return("missing value");
	ticks=strtoul(s, &e, 10);
	if (*e == '.'){
		p = e+1;
		l = strtoul(p, &e, 10);
		if(e-p > nelem(p10))
			return "too many digits after decimal point";
		if(e-p == 0)
			return "ill-formed number";
		l *= p10[e-p-1];
	}else
		l = 0;
	if (*e == '\0' || strcmp(e, "s") == 0){
		ticks = 1000000000 * ticks + l;
	}else if (strcmp(e, "ms") == 0){
		ticks = 1000000 * ticks + l/1000;
	}else if (strcmp(e, "µs") == 0 || strcmp(e, "us") == 0){
		ticks = 1000 * ticks + l/1000000;
	}else if (strcmp(e, "ns") != 0)
		return "unrecognized unit";
	*rt = ticks;
	return NULL;
}

static void
procctlreq(struct proc *p, char *va, int n)
{
	ERRSTACK(2);
	Segment *s;
	int npc, pri, core;
	struct cmdbuf *cb;
	struct cmdtab *ct;
	int64_t time;
	char *e;

	if(p->kp)	/* no ctl requests to kprocs */
		error(Eperm);

	cb = parsecmd(va, n);
	if(waserror()){
		kfree(cb);
		nexterror();
	}

	ct = lookupcmd(cb, proccmd, nelem(proccmd));

	switch(ct->index){
	case CMclose:
		procctlclosefiles(p, 0, atoi(cb->f[1]));
		break;
	case CMclosefiles:
		procctlclosefiles(p, 1, 0);
		break;
	case CMhang:
		p->hang = 1;
		break;
	case CMkill:
		switch(p->state) {
		case Broken:
			unbreak(p);
			break;
		case Stopped:
		case Semdown:
			p->procctl = struct proc_exitme;
			postnote(p, 0, "sys: killed", NExit);
			ready(p);
			break;
		default:
			p->procctl = struct proc_exitme;
			postnote(p, 0, "sys: killed", NExit);
		}
		break;
	case CMnohang:
		p->hang = 0;
		break;
	case CMnoswap:
		p->noswap = 1;
		break;
	case CMpri:
		pri = atoi(cb->f[1]);
		if(pri > PriNormal && !iseve())
			error(Eperm);
		procpriority(p, pri, 0);
		break;
	case CMfixedpri:
		pri = atoi(cb->f[1]);
		if(pri > PriNormal && !iseve())
			error(Eperm);
		procpriority(p, pri, 1);
		break;
	case CMprivate:
		p->privatemem = 1;
		break;
	case CMprofile:
		s = p->seg[TSEG];
		if(s == 0 || (s->type&SG_TYPE) != SG_TEXT)
			error(Ebadctl);
		if(s->profile != 0)
			kfree(s->profile);
		npc = (s->top-s->base)>>LRESPROF;
		s->profile = kmalloc(npc * sizeof(*s->profile), 0);
		if(s->profile == 0)
			error(Enomem);
		break;
	case CMstart:
		if(p->state != Stopped)
			error(Ebadctl);
		ready(p);
		break;
	case CMstartstop:
		if(p->state != Stopped)
			error(Ebadctl);
		p->procctl = struct proc_traceme;
		ready(p);
		procstopwait(p, struct proc_traceme);
		break;
	case CMstartsyscall:
		if(p->state != Stopped)
			error(Ebadctl);
		p->procctl = struct proc_tracesyscall;
		ready(p);
		procstopwait(p, struct proc_tracesyscall);
		break;
	case CMstop:
		procstopwait(p, struct proc_stopme);
		break;
	case CMwaitstop:
		procstopwait(p, 0);
		break;
	case CMwired:
		core = atoi(cb->f[1]);
		procwired(p, core);
		sched();
		break;
	case CMtrace:
		switch(cb->nf){
		case 1:
			p->trace ^= 1;
			break;
		case 2:
			p->trace = (atoi(cb->f[1]) != 0);
			break;
		default:
			error("args");
		}
		break;
	/* real time */
	case CMperiod:
		if(p->edf == NULL)
			edfinit(p);
		if(e=parsetime(&time, cb->f[1]))	/* time in ns */
			error(e);
		edfstop(p);
		p->edf->T = time/1000;			/* Edf times are in µs */
		break;
	case CMdeadline:
		if(p->edf == NULL)
			edfinit(p);
		if(e=parsetime(&time, cb->f[1]))
			error(e);
		edfstop(p);
		p->edf->D = time/1000;
		break;
	case CMcost:
		if(p->edf == NULL)
			edfinit(p);
		if(e=parsetime(&time, cb->f[1]))
			error(e);
		edfstop(p);
		p->edf->C = time/1000;
		break;
	case CMsporadic:
		if(p->edf == NULL)
			edfinit(p);
		p->edf->flags |= Sporadic;
		break;
	case CMdeadlinenotes:
		if(p->edf == NULL)
			edfinit(p);
		p->edf->flags |= Sendnotes;
		break;
	case CMadmit:
		if(p->edf == 0)
			error("edf params");
		if(e = edfadmit(p))
			error(e);
		break;
	case CMextra:
		if(p->edf == NULL)
			edfinit(p);
		p->edf->flags |= Extratime;
		break;
	case CMexpel:
		if(p->edf)
			edfstop(p);
		break;
	case CMevent:
		if(current->trace)
			proctrace(up, SUser, 0);
		break;
	case CMcore:
		core = atoi(cb->f[1]);
		if(core >= MACHMAX)
			error("wrong core number");
		else if(core == 0){
			if(p->ac == NULL)
				error("not running in an ac");
			p->procctl = struct proc_totc;
			if(p != up && p->state == Exotic){
				/* see the comment in postnote */
				intrac(p);
			}
		}else{
			if(p->ac != NULL)
				error("running in an ac");
			if(core < 0)
				p->ac = getac(p, -1);
			else
				p->ac = getac(p, core);
			p->procctl = struct proc_toac;
			p->prepagemem = 1;
		}
		break;
	}
	kfree(cb);
}

static int
procstopped(void *a)
{
	struct proc *p = a;
	return p->state == Stopped;
}

static int
procctlmemio(struct proc *p, uintptr_t offset, int n, void *va, int read)
{
	KMap *k;
	Pte *pte;
	Page *pg;
	Segment *s;
	uintptr_t soff, l;	/* hmmmm */
	uint8_t *b;
	uintmem pgsz;

	for(;;) {
		s = seg(p, offset, 1);
		if(s == 0)
			error(Ebadarg);

		if(offset+n >= s->top)
			n = s->top-offset;

		if(!read && (s->type&SG_TYPE) == SG_TEXT)
			s = txt2data(p, s);

		s->steal++;
		soff = offset-s->base;
		if(waserror()) {
			s->steal--;
			nexterror();
		}
		if(fixfault(s, offset, read, 0, s->color) == 0)
			break;
		s->steal--;
	}
	pte = s->map[soff/PTEMAPMEM];
	if(pte == 0)
		panic("procctlmemio");
	pgsz = m->pgsz[s->pgszi];
	pg = pte->pages[(soff&(PTEMAPMEM-1))/pgsz];
	if(pagedout(pg))
		panic("procctlmemio1");

	l = pgsz - (offset&(pgsz-1));
	if(n > l)
		n = l;

	k = kmap(pg);
	if(waserror()) {
		s->steal--;
		kunmap(k);
		nexterror();
	}
	b = (uint8_t*)VA(k);
	b += offset&(pgsz-1);
	if(read == 1)
		memmove(va, b, n);	/* This can fault */
	else
		memmove(b, va, n);
	kunmap(k);

	/* Ensure the process sees text page changes */
	if(s->flushme)
		memset(pg->cachectl, PG_TXTFLUSH, sizeof(pg->cachectl));

	s->steal--;

	if(read == 0)
		p->newtlb = 1;

	return n;
}

static Segment*
txt2data(struct proc *p, Segment *s)
{
	int i;
	Segment *ps;

	ps = newseg(SG_DATA, s->base, s->size);
	ps->image = s->image;
	kref_get(&ps->image->ref, 1);
	ps->fstart = s->fstart;
	ps->flen = s->flen;
	ps->flushme = 1;

	qlock(&p->seglock);
	for(i = 0; i < NSEG; i++)
		if(p->seg[i] == s)
			break;
	if(i == NSEG)
		panic("segment gone");

	qunlock(&s->lk);
	putseg(s);
	qlock(&ps->lk);
	p->seg[i] = ps;
	qunlock(&p->seglock);

	return ps;
}

Segment*
data2txt(Segment *s)
{
	Segment *ps;

	ps = newseg(SG_TEXT, s->base, s->size);
	ps->image = s->image;
	kref_get(&ps->image->ref, 1);
	ps->fstart = s->fstart;
	ps->flen = s->flen;
	ps->flushme = 1;

	return ps;
}
#endif
