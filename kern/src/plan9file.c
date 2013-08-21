/*
 * Copyright 2013 Google Inc.
 * Copyright (c) 1989-2003 by Lucent Technologies, Bell Laboratories.
 */
#define DEBUG
#include <setjmp.h>
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
#include <fcall.h>
#include <ros/fs.h>

/* simple functions for common uses. Read a num/string to user mode,
 * accounting for offset.
 */
int
readnum(unsigned long off, char *buf, unsigned long n, unsigned long val, int size)
{
	char tmp[64];
	size = MIN(sizeof(tmp),size);

	/* we really need the %* format. */
	snprintf(tmp, size, "%lud", val);
	tmp[size-1] = ' ';
	if(off >= size)
		return 0;
	if(off+n > size)
		n = size-off;
	memmove(buf, tmp+off, n);
	return n;
}

long
readstr(long offset, char *buf, long n, char *str)
{
	long size;

	size = strlen(str);
	if(offset >= size)
		return 0;
	if(offset+n > size)
		n = size-offset;
	memmove(buf, str+offset, n);
	return n;
}

void
fdclose(int fd, int flag, struct errbuf *perrbuf)
{
	int i;
	struct chan *c;
	struct fgrp *f;

	f = current->fgrp;
	spin_lock(&f->lock);
	c = f->fd[fd];
	if(c == NULL){
		/* can happen for users with shared fd tables */
		spin_unlock(&f->lock);
		return;
	}
	if(flag){
		if(c == NULL || !(c->flag&flag)){
			spin_unlock(&f->lock);
			return;
		}
	}
	f->fd[fd] = NULL;
	if(fd == f->maxfd)
		for(i = fd; --i >= 0 && f->fd[i] == 0; )
			f->maxfd = i;

	spin_unlock(&f->lock);
	cclose(c, perrbuf);
}

int
openmode(int omode, struct errbuf *perrbuf)
{
	omode &= ~(OTRUNC|OCEXEC|ORCLOSE);
	if(omode > OEXEC)
	  error(Ebadarg);
	if(omode == OEXEC)
		return OREAD;
	return omode;
}

static void
unlockfgrp(struct fgrp *f)
{
  int ex;
  
  ex = f->exceed;
  f->exceed = 0;
  spin_unlock(&f->lock);
  if(ex)
    printd("warning: process exceeds %d file descriptors\n", ex);
}


int
growfd(struct fgrp *f, int fd)	/* fd is always >= 0 */
{
    struct chan **newfd, **oldfd;
    
    if(fd < f->nfd)
	return 0;
    if(fd >= f->nfd+DELTAFD)
		return -1;	/* out of range */
    /*
     * Unbounded allocation is unwise
     */
    if(f->nfd >= 5000){
    Exhausted:
	printd("no free file descriptors\n");
	return -1;
    }
    newfd = kmalloc((f->nfd+DELTAFD)*sizeof(struct chan*), KMALLOC_WAIT);
    if(newfd == 0)
	goto Exhausted;
    oldfd = f->fd;
    memmove(newfd, oldfd, f->nfd*sizeof(struct chan*));
    f->fd = newfd;
    kfree(oldfd);
    f->nfd += DELTAFD;
    if(fd > f->maxfd){
	if(fd/100 > f->maxfd/100)
	    f->exceed = (fd/100)*100;
	f->maxfd = fd;
    }
    return 1;
}

/*
 *  this assumes that the fgrp is locked
 */
int
findfreefd(struct fgrp *f, int start)
{
	int fd;

	for(fd=start; fd<f->nfd; fd++)
		if(f->fd[fd] == 0)
			break;
	if(fd >= f->nfd && growfd(f, fd) < 0)
		return -1;
	return fd;
}

int
newfd(struct chan *c)
{
	int fd;
	struct fgrp *f;

	f = current->fgrp;
	spin_lock(&f->lock);
	fd = findfreefd(f, 0);
	if(fd < 0){
		unlockfgrp(f);
		return -1;
	}
	if(fd > f->maxfd)
		f->maxfd = fd;
	f->fd[fd] = c;
	unlockfgrp(f);
	return fd;
}

static int
newfd2(int fd[2], struct chan *c[2])
{
	struct fgrp *f;

	f = current->fgrp;
	spin_lock(&f->lock);
	fd[0] = findfreefd(f, 0);
	if(fd[0] < 0){
		unlockfgrp(f);
		return -1;
	}
	fd[1] = findfreefd(f, fd[0]+1);
	if(fd[1] < 0){
		unlockfgrp(f);
		return -1;
	}
	if(fd[1] > f->maxfd)
		f->maxfd = fd[1];
	f->fd[fd[0]] = c[0];
	f->fd[fd[1]] = c[1];
	unlockfgrp(f);

	return 0;
}

struct chan*
fdtochan(int fd, int mode, int chkmnt, int iref, struct errbuf *perrbuf)
{
	struct chan *c;
	struct fgrp *f;

	c = NULL;
	f = current->fgrp;

	spin_lock(&f->lock);
	if(fd<0 || f->nfd<=fd || (c = f->fd[fd])==0) {
		spin_unlock(&f->lock);
		error(Ebadfd);
	}
	if(iref)
		kref_get(&c->ref, 1);
	spin_unlock(&f->lock);

	if(chkmnt && (c->flag&CMSG)) {
		if(iref)
		  cclose(c, perrbuf);
		error(Ebadusefd);
	}

	if(mode<0 || c->mode==ORDWR)
		return c;

	if((mode&OTRUNC) && c->mode==OREAD) {
		if(iref)
		    cclose(c, perrbuf);
		error(Ebadusefd);
	}

	if((mode&~OTRUNC) != c->mode) {
		if(iref)
		    cclose(c, perrbuf);
		error(Ebadusefd);
	}

	return c;
}

static long
unionread(struct chan *c, void *va, long n, struct errbuf *perrbuf)
{
    ERRSTACK(2);
	int i;
	long nr;
	struct mhead *mh;
	struct mount *mount;

	spin_lock(&c->umqlock);
	mh = c->umh;
	rlock(&mh->lock);
	mount = mh->mount;
	/* bring mount in sync with c->uri and c->umc */
	for(i = 0; mount != NULL && i < c->uri; i++)
		mount = mount->next;

	nr = 0;
	while(mount != NULL){
		/* Error causes component of union to be skipped */
		if(mount->to && !waserror()){
			if(c->umc == NULL){
			  c->umc = cclone(mount->to, perrbuf);
			    c->umc = c->umc->dev->open(c->umc, OREAD, perrbuf);
			}

			nr = c->umc->dev->read(c->umc, va, n, c->umc->offset, perrbuf);
			c->umc->offset += nr;
		}
		if(nr > 0)
			break;

		/* Advance to next element */
		c->uri++;
		if(c->umc){
		    cclose(c->umc, perrbuf);
			c->umc = NULL;
		}
		mount = mount->next;
	}
	runlock(&mh->lock);
	spin_unlock(&c->umqlock);
	return nr;
}

static void
unionrewind(struct chan *c, struct errbuf *perrbuf)
{
	spin_lock(&c->umqlock);
	c->uri = 0;
	if(c->umc){
	    cclose(c->umc, perrbuf);
		c->umc = NULL;
	}
	spin_unlock(&c->umqlock);
}

static char*
pathlast(struct path *p)
{
	char *s;

	if(p == NULL)
		return NULL;
	if(p->len == 0)
		return NULL;
	s = strrchr(p->s, '/');
	if(s)
		return s+1;
	return p->s;
}

static unsigned long
dirfixed(uint8_t *p, unsigned char *e, struct dir *d, struct errbuf *perrbuf)
{
	int len;
	struct dev *dev;

	len = GBIT16(p)+BIT16SZ;
	if(p + len > e)
		return 0;

	p += BIT16SZ;	/* ignore size */
	dev = devtabget(GBIT16(p), 1, perrbuf);			//XDYNX
	if(dev != NULL){
		d->type = dev->dc;
		//devtabdecr(dev);
	}
	else
		d->type = -1;
	p += BIT16SZ;
	d->dev = GBIT32(p);
	p += BIT32SZ;
	d->qid.type = GBIT8(p);
	p += BIT8SZ;
	d->qid.vers = GBIT32(p);
	p += BIT32SZ;
	d->qid.path = GBIT64(p);
	p += BIT64SZ;
	d->mode = GBIT32(p);
	p += BIT32SZ;
	d->atime = GBIT32(p);
	p += BIT32SZ;
	d->mtime = GBIT32(p);
	p += BIT32SZ;
	d->length = GBIT64(p);

	return len;
}

static char*
dirname(uint8_t *p, unsigned long *n)
{
	p += BIT16SZ+BIT16SZ+BIT32SZ+BIT8SZ+BIT32SZ+BIT64SZ
		+ BIT32SZ+BIT32SZ+BIT32SZ+BIT64SZ;
	*n = GBIT16(p);

	return (char*)p+BIT16SZ;
}

static unsigned long
dirsetname(char *name, unsigned long len, uint8_t *p, unsigned long n, unsigned long maxn)
{
	char *oname;
	unsigned long nn, olen;

	if(n == BIT16SZ)
		return BIT16SZ;

	oname = dirname(p, &olen);

	nn = n+len-olen;
	PBIT16(p, nn-BIT16SZ);
	if(nn > maxn)
		return BIT16SZ;

	if(len != olen)
		memmove(oname+len, oname+olen, p+n-(uint8_t*)(oname+olen));
	PBIT16((uint8_t*)(oname-2), len);
	memmove(oname, name, len);

	return nn;
}

/*
 * struct mountfix might have caused the fixed results of the directory read
 * to overflow the buffer.  Catch the overflow in c->dirrock.
 */
static void
mountrock(struct chan *c, uint8_t *p, unsigned char **pe)
{
	uint8_t *e, *r;
	int len, n;

	e = *pe;

	/* find last directory entry */
	for(;;){
		len = BIT16SZ+GBIT16(p);
		if(p+len >= e)
			break;
		p += len;
	}

	/* save it away */
	spin_lock(&c->rockqlock);
	if(c->nrock+len > c->mrock){
		n = ROUNDUP(c->nrock+len, 1024);
		r = kmalloc(n, 0);
		memmove(r, c->dirrock, c->nrock);
		kfree(c->dirrock);
		c->dirrock = r;
		c->mrock = n;
	}
	memmove(c->dirrock+c->nrock, p, len);
	c->nrock += len;
	spin_unlock(&c->rockqlock);

	/* drop it */
	*pe = p;
}

/*
 * Satisfy a directory read with the results saved in c->dirrock.
 */
static int
mountrockread(struct chan *c, uint8_t *op, long n, long *nn)
{
	long dirlen;
	uint8_t *rp, *erp, *ep, *p;

	/* common case */
	if(c->nrock == 0)
		return 0;

	/* copy out what we can */
	spin_lock(&c->rockqlock);
	rp = c->dirrock;
	erp = rp+c->nrock;
	p = op;
	ep = p+n;
	while(rp+BIT16SZ <= erp){
		dirlen = BIT16SZ+GBIT16(rp);
		if(p+dirlen > ep)
			break;
		memmove(p, rp, dirlen);
		p += dirlen;
		rp += dirlen;
	}

	if(p == op){
		spin_unlock(&c->rockqlock);
		return 0;
	}

	/* shift the rest */
	if(rp != erp)
		memmove(c->dirrock, rp, erp-rp);
	c->nrock = erp - rp;

	*nn = p - op;
	spin_unlock(&c->rockqlock);
	return 1;
}

static void
mountrewind(struct chan *c)
{
	c->nrock = 0;
}

/*
 * Rewrite the results of a directory read to reflect current
 * name space bindings and mounts.  Specifically, replace
 * directory entries for bind and mount points with the results
 * of statting what is mounted there.  Except leave the old names.
 */
static long
mountfix(struct chan *c, uint8_t *op, long n, long maxn, struct errbuf *perrbuf)
{
    ERRSTACK(2);
	char *name;
	int nbuf;
	struct chan *nc;
	struct mhead *mh;
	struct mount *mount;
	unsigned long dirlen, nname, r, rest;
	long l;
	uint8_t *buf, *e, *p;
	struct dir d;

	p = op;
	buf = NULL;
	nbuf = 0;
	for(e=&p[n]; p+BIT16SZ<e; p+=dirlen){
	    dirlen = dirfixed(p, e, &d, perrbuf);
		if(dirlen == 0)
			break;
		nc = NULL;
		mh = NULL;
		if(findmount(&nc, &mh, d.type, d.dev, d.qid, perrbuf)){
			/*
			 * If it's a union directory and the original is
			 * in the union, don't rewrite anything.
			 */
			for(mount=mh->mount; mount; mount=mount->next)
			    if(eqchanddq(mount->to, d.type, d.dev, d.qid, 1, perrbuf))
					goto Norewrite;

			name = dirname(p, &nname);
			/*
			 * Do the stat but fix the name.  If it fails,
			 * leave old entry.
			 * BUG: If it fails because there isn't room for
			 * the entry, what can we do?  Nothing, really.
			 * Might as well skip it.
			 */
			if(buf == NULL){
				buf = kmalloc(4096, 0);
				nbuf = 4096;
			}
			if(waserror())
				goto Norewrite;
			l = nc->dev->stat(nc, buf, nbuf, perrbuf);
			r = dirsetname(name, nname, buf, l, nbuf);
			if(r == BIT16SZ)
				error("dirsetname");

			/*
			 * Shift data in buffer to accomodate new entry,
			 * possibly overflowing into rock.
			 */
			rest = e - (p+dirlen);
			if(r > dirlen){
				while(p+r+rest > op+maxn){
					mountrock(c, p, &e);
					if(e == p){
						dirlen = 0;
						goto Norewrite;
					}
					rest = e - (p+dirlen);
				}
			}
			if(r != dirlen){
				memmove(p+r, p+dirlen, rest);
				dirlen = r;
				e = p+dirlen+rest;
			}

			/*
			 * Rewrite directory entry.
			 */
			memmove(p, buf, r);

		    Norewrite:
			cclose(nc, perrbuf);
			putmhead(mh, perrbuf);
		}
	}
	if(buf)
		kfree(buf);

	if(p != e)
		error("oops in mountfix");

	return e-op;
}

long
sysread(int fd, void *p, size_t n, off_t off)
{
    PERRBUF;
    
    ERRSTACK(2);
    long nn, nnn;
    struct chan *c;
    int ispread = 1;
    printd("sysread %d %p %d %lld\n", fd, p, n, off);

    if(waserror()){
	set_errno(EBADF);
	return -1;
    }

    c = fdtochan(fd, OREAD, 1, 1, perrbuf);
    
    if(waserror()){
	cclose(c, perrbuf);
	return -1;
    }
    
    /*
     * The offset is passed through on directories, normally.
     * Sysseek complains, but pread is used by servers like exportfs,
     * that shouldn't need to worry about this issue.
     *
     * Notice that c->devoffset is the offset that c's dev is seeing.
     * The number of bytes read on this fd (c->offset) may be different
     * due to rewritings in mountfix.
     */
    if(off == ~0LL){	/* use and maintain channel's offset */
	off = c->offset;
	ispread = 0;
    }
    
    if(c->qid.type & QTDIR){
	unsigned char *ents;
	/*
	 * struct directory read:
	 * rewind to the beginning of the file if necessary;
	 * try to fill the buffer via mountrockread;
	 * clear ispread to always maintain the struct chan offset.
	 */
	/* this is a bit of a hack until we resolve akaros direntry format. */
	ents = kmalloc(8192, KMALLOC_WAIT);
	if (! ents)
		error(Enomem);

	if(off == 0LL){
	    if(!ispread){
		c->offset = 0;
		c->devoffset = 0;
	    }
	    mountrewind(c);
	    unionrewind(c, perrbuf);
	}
	printd("sysread: dir: ispread %d @ %lld\n", ispread, off);
	/* tell it we have less than we have to make sure it will
	 * fit in the large akaros dirents.
	 */
	if(!mountrockread(c, p, n, &nn)){
	    if(c->umh)
		nn = unionread(c, ents, 2048, perrbuf);
	    else{
		if(off != c->offset)
		    error(Edirseek);
		nn = c->dev->read(c, ents, 2048, c->devoffset, perrbuf);
	    }
	}
	nnn = mountfix(c, ents, nn, n, perrbuf);
printd("sysread: dir: nn %d nnn %d\n", nn, nnn);
	/* now convert to akaros kdents. This whole thing needs fixin' */
	int total, amt = 0, iter = 0;
	for(total = 0; total < nnn; total += amt){
		printd("Converted nnn %d... total %d amt %d\n", nnn, total, amt);
		amt = convM2kdirent(ents, nnn-total, (struct kdirent *) p);
printd("amt after call %d\n", amt);
		ents += amt;
		if (iter++ > 4)
			break;
	} 
printd("sysread: dir: nn %d nnn %d\n", nn, nnn);
	
	ispread = 0;
    }
    else
	nnn = nn = c->dev->read(c, p, n, off, perrbuf);
    
    if(!ispread){
	spin_lock(&c->lock);
	c->devoffset += nn;
	c->offset += nnn;
	spin_unlock(&c->lock);
    }
    
    cclose(c, perrbuf);
    
    return nnn;
}

long
syswrite(int fd, void *p, size_t n, off_t off)
{
    PERRBUF;
    ERRSTACK(3);

    int ispwrite = 1;
    long r = n;
    struct chan *c;

	printd("syswrite %d %p %d\n", fd, p, n);
    n = 0;
    if (waserror()) {
	set_errno(EBADF);
	return -1;
    }
    
    c = fdtochan(fd, OWRITE, 1, 1, perrbuf);
    printd("syswrite chan %p\n", c);
    if(waserror()) {

	if(!ispwrite){
	    spin_lock(&c->lock);
	    c->offset -= n;
	    spin_unlock(&c->lock);
	}
	cclose(c, perrbuf);
	nexterror();
    }
    
    if(c->qid.type & QTDIR)
	error(Eisdir);
    
    n = r;
    

    if(off == ~0LL){	/* use and maintain channel's offset */
	spin_lock(&c->lock);
	off = c->offset;
	c->offset += n;
	spin_unlock(&c->lock);
    }

    printd("call dev write\n");
    r = c->dev->write(c, p, n, off, perrbuf);

    if(!ispwrite && r < n){
	spin_lock(&c->lock);
	c->offset -= n - r;
	spin_unlock(&c->lock);
    }
    
    cclose(c, perrbuf);
printd("syswrite retturns %d\n", r);
    return r;

}

int
syscreate(char *name, int omode)
{
    PERRBUF;
    ERRSTACK(2);
    struct chan *c = NULL;
    int fd;
    /* if it exists, it is truncated. 
     * if it does not exists, it's created.
     * so we don't need these flags.
     * TODO; convert all flags to akaros flags.
     */
    omode &= ~(O_CREAT|O_TRUNC);
      
	if (waserror()){
	    if(c)
		cclose(c, perrbuf);
	    printd("syscreate fail 1 mode  %x\n", omode);
	    return -1;
	}

	openmode(omode, perrbuf);	/* error check only */

	c = namec(name, Acreate, omode, 0, perrbuf);
	fd = newfd(c);
	if(fd < 0)
	    error(Enofd);
	return fd;
}

int
sysopen(char *name, int omode)
{
    PERRBUF;
    ERRSTACK(2);
    struct chan *c = NULL;
    int fd;
    int mustdir = 0;
    printd("sysopen %s mode %o\n", name, omode);
    if (omode & O_NONBLOCK) /* what to do? */
	omode &= ~O_NONBLOCK;
    if (omode & O_CLOEXEC) /* FIX ME */
	omode &= ~O_CLOEXEC;
    if (omode & O_DIRECTORY){
	omode &= ~O_DIRECTORY;
	mustdir = 1;
    }
	if (omode & (O_CREAT|O_TRUNC))
	    return syscreate(name, omode);

	if (waserror()){
	    if(c)
		cclose(c, perrbuf);
	    return -1;
	}
	openmode(omode, perrbuf);	/* error check only */

	c = namec(name, Aopen, omode, 0, perrbuf);
	fd = newfd(c);
	if(fd < 0)
	    error(Enofd);
	printd("sysopen %s returns %d %x\n", name, fd, fd);
	return fd;
}

int sysclose(int fd)
{
	ERRSTACKBASE(1);

	if (waserror()){
		return -1;
	}

	fdtochan(fd, -1, 0, 0, perrbuf);
	fdclose(fd, 0, perrbuf);
	return 0;
}

int
sysstat(char *name, uint8_t *statbuf, int len)
{
    PERRBUF;
    ERRSTACK(2);
    int r;
    struct chan *c = NULL;
    char *aname;
    int fd;
    uint8_t data[sizeof(struct dir)];
    
    if (waserror()){
	if(c)
	    cclose(c, perrbuf);
	return -1;
    }

    c = namec(name, Aaccess, 0, 0, perrbuf);

    r = c->dev->stat(c, data, sizeof(data), perrbuf);

    aname = pathlast(c->path);
    if(aname)
	r = dirsetname(aname, strlen(aname), data, r, sizeof(data));
    
    cclose(c, perrbuf);
    /* now convert for akaros. */
    convM2kstat(data, sizeof(data), (struct kstat *)statbuf);
    
    return 0;
}

int
sysfstat(int fd, uint8_t *statbuf, int len)
{
    PERRBUF;
    ERRSTACK(2);
    int r;
    struct chan *c = NULL;
    uint8_t data[sizeof(struct dir)];
    
    if (waserror()){
	return -1;
    }

    c = fdtochan(fd, -1, 0, 1, perrbuf);

    if(waserror()) {
      cclose(c, perrbuf);
	nexterror();
    }
    r = c->dev->stat(c, data, sizeof(data), perrbuf);
    
    cclose(c, perrbuf);
    /* now convert for akaros. */
    convM2kstat(data, sizeof(data), (struct kstat *)statbuf);
 printd("sysfstat fd %d ok\n", fd);   
    return 0;
}

int
sysdup(int ofd, int nfd)
{
    ERRSTACKBASE(2);
	struct chan *nc, *oc;
	struct fgrp *f;

	/*
	 * int dup(int oldfd, int newfd);
	 * if newfd is < 0, pick anything.
	 *
	 * Close after dup'ing, so date > #d/1 works
	 */
	if (waserror()){
	    set_errno(EBADF);
	    return -1;
	}

	oc = fdtochan(ofd, -1, 0, 1, perrbuf);

	if(nfd != -1){
		f = current->fgrp;
		spin_lock(&f->lock);
		if(nfd < 0 || growfd(f, nfd) < 0) {
			unlockfgrp(f);
			cclose(oc, perrbuf);
			error(Ebadfd);
		}
		if(nfd > f->maxfd)
			f->maxfd = nfd;

		nc = f->fd[nfd];
		f->fd[nfd] = oc;
		unlockfgrp(f);
		if(nc != NULL)
		    cclose(nc, perrbuf);
	}else{
		if(waserror()) {
		    cclose(oc, perrbuf);
		    nexterror();
		}
		nfd = newfd(oc);
		if(nfd < 0)
		    error(Enofd);
	}

	return nfd;
}


int
plan9setup(struct proc *up)
{
    PERRBUF;
    ERRSTACK(2);
    if (waserror()){
      printd("plan9setup failed\n");
      return -1;
    }

    up->fgrp = dupfgrp(NULL, perrbuf);
    up->pgrp = newpgrp();
    return 0;
}

