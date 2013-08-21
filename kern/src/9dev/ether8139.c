/*
 * Realtek 8139 (but not the 8129).
 * Error recovery for the various over/under -flow conditions
 * may need work.
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

#include <etherif.h>

enum {					/* registers */
	Idr0		= 0x0000,	/* MAC address */
	Mar0		= 0x0008,	/* Multicast address */
	Tsd0		= 0x0010,	/* Transmit Status Descriptor0 */
	Tsad0		= 0x0020,	/* Transmit Start Address Descriptor0 */
	Rbstart		= 0x0030,	/* Receive Buffer Start Address */
	Erbcr		= 0x0034,	/* Early Receive Byte Count */
	Ersr		= 0x0036,	/* Early Receive Status */
	Cr		= 0x0037,	/* Command Register */
	Capr		= 0x0038,	/* Current Address of Packet Read */
	Cbr		= 0x003A,	/* Current Buffer Address */
	Imr		= 0x003C,	/* Interrupt Mask */
	Isr		= 0x003E,	/* Interrupt Status */
	Tcr		= 0x0040,	/* Transmit Configuration */
	Rcr		= 0x0044,	/* Receive Configuration */
	Tctr		= 0x0048,	/* Timer Count */
	Mpc		= 0x004C,	/* Missed Packet Counter */
	Cr9346		= 0x0050,	/* 9346 Command Register */
	Config0		= 0x0051,	/* Configuration Register 0 */
	Config1		= 0x0052,	/* Configuration Register 1 */
	TimerInt	= 0x0054,	/* Timer Interrupt */
	Msr		= 0x0058,	/* Media Status */
	Config3		= 0x0059,	/* Configuration Register 3 */
	Config4		= 0x005A,	/* Configuration Register 4 */
	Mulint		= 0x005C,	/* Multiple Interrupt Select */
	RerID		= 0x005E,	/* PCI Revision ID */
	Tsad		= 0x0060,	/* Transmit Status of all Descriptors */

	Bmcr		= 0x0062,	/* Basic Mode Control */
	Bmsr		= 0x0064,	/* Basic Mode Status */
	Anar		= 0x0066,	/* Auto-Negotiation Advertisment */
	Anlpar		= 0x0068,	/* Auto-Negotiation Link Partner */
	Aner		= 0x006A,	/* Auto-Negotiation Expansion */
	Dis		= 0x006C,	/* Disconnect Counter */
	Fcsc		= 0x006E,	/* False Carrier Sense Counter */
	Nwaytr		= 0x0070,	/* N-way Test */
	Rec		= 0x0072,	/* RX_ER Counter */
	Cscr		= 0x0074,	/* CS Configuration */
	Phy1parm	= 0x0078,	/* PHY Parameter 1 */
	Twparm		= 0x007C,	/* Twister Parameter */
	Phy2parm	= 0x0080,	/* PHY Parameter 2 */
};

enum {					/* Cr */
	Bufe		= 0x01,		/* Rx Buffer Empty */
	Te		= 0x04,		/* Transmitter Enable */
	Re		= 0x08,		/* Receiver Enable */
	Rst		= 0x10,		/* Software Reset */
};

enum {					/* Imr/Isr */
	Rok		= 0x0001,	/* Receive OK */
	Rer		= 0x0002,	/* Receive Error */
	Tok		= 0x0004,	/* Transmit OK */
	Ter		= 0x0008,	/* Transmit Error */
	Rxovw		= 0x0010,	/* Receive Buffer Overflow */
	PunLc		= 0x0020,	/* Packet Underrun or Link struct change */
	Fovw		= 0x0040,	/* Receive FIFO Overflow */
	Clc		= 0x2000,	/* Cable Length struct change */
	Timerbit	= 0x4000,	/* Timer */
	Serr		= 0x8000,	/* System Error */
};

enum {					/* Tcr */
	Clrabt		= 0x00000001,	/* Clear Abort */
	TxrrSHIFT	= 4,		/* Transmit Retry Count */
	TxrrMASK	= 0x000000F0,
	MtxdmaSHIFT	= 8,		/* Max. DMA Burst Size */
	MtxdmaMASK	= 0x00000700,
	Mtxdma2048	= 0x00000700,
	Acrc		= 0x00010000,	/* Append CRC (not) */
	LbkSHIFT	= 17,		/* Loopback Test */
	LbkMASK		= 0x00060000,
	Rtl8139ArevG	= 0x00800000,	/* RTL8139A Rev. G ID */
	IfgSHIFT	= 24,		/* Interframe Gap */
	IfgMASK		= 0x03000000,
	HwveridSHIFT	= 26,		/* Hardware Version ID */
	HwveridMASK	= 0x7C000000,
};

enum {					/* Rcr */
	Aap		= 0x00000001,	/* Accept All Packets */
	Apm		= 0x00000002,	/* Accept Physical Match */
	Am		= 0x00000004,	/* Accept Multicast */
	Ab		= 0x00000008,	/* Accept Broadcast */
	Ar		= 0x00000010,	/* Accept Runt */
	Aer		= 0x00000020,	/* Accept Error */
	Sel9356		= 0x00000040,	/* 9356 EEPROM used */
	Wrap		= 0x00000080,	/* Rx Buffer Wrap Control */
	MrxdmaSHIFT	= 8,		/* Max. DMA Burst Size */
	MrxdmaMASK	= 0x00000700,
	Mrxdmaunlimited	= 0x00000700,
	RblenSHIFT	= 11,		/* Receive Buffer Length */
	RblenMASK	= 0x00001800,
	Rblen8K		= 0x00000000,	/* 8KB+16 */
	Rblen16K	= 0x00000800,	/* 16KB+16 */
	Rblen32K	= 0x00001000,	/* 32KB+16 */
	Rblen64K	= 0x00001800,	/* 64KB+16 */
	RxfthSHIFT	= 13,		/* Receive Buffer Length */
	RxfthMASK	= 0x0000E000,
	Rxfth256	= 0x00008000,
	Rxfthnone	= 0x0000E000,
	Rer8		= 0x00010000,	/* Accept Error Packets > 8 bytes */
	MulERINT	= 0x00020000,	/* Multiple Early Interrupt Select */
	ErxthSHIFT	= 24,		/* Early Rx Threshold */
	ErxthMASK	= 0x0F000000,
	Erxthnone	= 0x00000000,
};

enum {					/* Received Packet Status */
	Rcok		= 0x0001,	/* Receive Completed OK */
	Fae		= 0x0002,	/* Frame Alignment Error */
	Crc		= 0x0004,	/* CRC Error */
	Long		= 0x0008,	/* Long Packet */
	Runt		= 0x0010,	/* Runt Packet Received */
	Ise		= 0x0020,	/* Invalid Symbol Error */
	Bar		= 0x2000,	/* Broadcast Address Received */
	Pam		= 0x4000,	/* Physical Address Matched */
	Mar		= 0x8000,	/* Multicast Address Received */
};

enum {					/* Media Status Register */
	Rxpf		= 0x01,		/* Pause Flag */
	Txpf		= 0x02,		/* Pause Flag */
	Linkb		= 0x04,		/* Inverse of Link Status */
	Speed10		= 0x08,		/* 10Mbps */
	Auxstatus	= 0x10,		/* Aux. Power Present Status */
	Rxfce		= 0x40,		/* Receive Flow Control Enable */
	Txfce		= 0x80,		/* Transmit Flow Control Enable */
};

typedef struct Td Td;
struct Td {			/* Soft Transmit Descriptor */
	int	tsd;
	int	tsad;
	uint8_t*	data;
	struct block*	bp;
};

enum {					/* Tsd0 */
	SizeSHIFT	= 0,		/* Descriptor Size */
	SizeMASK	= 0x00001FFF,
	Own		= 0x00002000,
	Tun		= 0x00004000,	/* Transmit FIFO Underrun */
	Tcok		= 0x00008000,	/* Transmit COmpleted OK */
	EtxthSHIFT	= 16,		/* Early Tx Threshold */
	EtxthMASK	= 0x001F0000,
	NccSHIFT	= 24,		/* Number of Collisions Count */
	NccMASK		= 0x0F000000,
	Cdh		= 0x10000000,	/* CD Heartbeat */
	Owc		= 0x20000000,	/* Out of Window Collision */
	Tabt		= 0x40000000,	/* Transmit Abort */
	Crs		= 0x80000000,	/* Carrier Sense Lost */
};

enum {
	Rblen		= Rblen64K,	/* Receive Buffer Length */
	Ntd		= 4,		/* Number of Transmit Descriptors */
};

#define 	Tdbsz		ROUNDUP(sizeof(struct etherpkt), 4)

typedef struct ctlr {
	int	port;
	//Pcidev*	pcidev;
	struct ctlr*	next;
	int	active;
	int	id;

	qlock_t alock;			/* attach */
	spinlock_t ilock;			/* init */
	void*	alloc;			/* base of per-struct ctlr allocated data */

	int	pcie;			/* flag: pci-express device? */

	uint64_t	mchash;			/* multicast hash */

	int	rcr;			/* receive configuration register */
	uint8_t*	rbstart;		/* receive buffer */
	int	rblen;			/* receive buffer length */
	int	ierrs;			/* receive errors */

	spinlock_t tlock;			/* transmit */
	Td	td[Ntd];
	int	ntd;			/* descriptors active */
	int	tdh;			/* host index into td */
	int	tdi;			/* interface index into td */
	int	etxth;			/* early transmit threshold */
	int	taligned;		/* packet required no alignment */
	int	tunaligned;		/* packet required alignment */

	int	dis;			/* disconnect counter */
	int	fcsc;			/* false carrier sense counter */
	int	rec;			/* RX_ER counter */
	unsigned int	mcast;
} ctlr;

static struct ctlr* ctlrhead;
static struct ctlr* ctlrtail;

#define csr8r(c, r)	(inb((c)->port+(r)))
#define csr16r(c, r)	(ins((c)->port+(r)))
#define csr32r(c, r)	(inl((c)->port+(r)))
#define csr8w(c, r, b)	(outb((c)->port+(r), (int)(b)))
#define csr16w(c, r, w)	(outs((c)->port+(r), (uint16_t)(w)))
#define csr32w(c, r, l)	(outl((c)->port+(r), (uint32_t)(l)))

static struct ether *kickdev;
static void kickme(void);

static void
rtl8139promiscuous(void* arg, int on)
{
	struct ether *edev;
	struct ctlr * ctlr;

	edev = arg;
	ctlr = edev->ctlr;
	ilock(&ctlr->ilock);

	if(on)
		ctlr->rcr |= Aap;
	else
		ctlr->rcr &= ~Aap;
	csr32w(ctlr, Rcr, ctlr->rcr);
	iunlock(&ctlr->ilock);
}

enum {
	/* everyone else uses 0x04c11db7, but they both produce the same crc */
	struct etherpolybe = 0x04c11db6,
	Bytemask = (1<<8) - 1,
};

static uint32_t
ethercrcbe(uint8_t *addr, long len)
{
	int i, j;
	uint32_t c, crc, carry;

	crc = ~0UL;
	for (i = 0; i < len; i++) {
		c = addr[i];
		for (j = 0; j < 8; j++) {
			carry = ((crc & (1UL << 31))? 1: 0) ^ (c & 1);
			crc <<= 1;
			c >>= 1;
			if (carry)
				crc = (crc ^ struct etherpolybe) | carry;
		}
	}
	return crc;
}

static uint32_t
swabl(uint32_t l)
{
	return l>>24 | (l>>8) & (Bytemask<<8) |
		(l<<8) & (Bytemask<<16) | l<<24;
}

static void
rtl8139multicast(void* ether, uint8_t *eaddr, int add)
{
	struct ether *edev;
	struct ctlr *ctlr;

	if (!add)
		return;	/* ok to keep receiving on old mcast addrs */

	edev = ether;
	ctlr = edev->ctlr;
	ilock(&ctlr->ilock);

	ctlr->mchash |= 1ULL << (ethercrcbe(eaddr, Eaddrlen) >> 26);

	ctlr->rcr |= Am;
	csr32w(ctlr, Rcr, ctlr->rcr);

	/* pci-e variants reverse the order of the hash byte registers */
	if (0 && ctlr->pcie) {
		csr32w(ctlr, Mar0,   swabl(ctlr->mchash>>32));
		csr32w(ctlr, Mar0+4, swabl(ctlr->mchash));
	} else {
		csr32w(ctlr, Mar0,   ctlr->mchash);
		csr32w(ctlr, Mar0+4, ctlr->mchash>>32);
	}

	iunlock(&ctlr->ilock);
}

static long
rtl8139ifstat(struct ether* edev, void* a, long n, uint32_t offset)
{
	int l;
	char *p;
	struct ctlr *ctlr;

	ctlr = edev->ctlr;
	p = kmalloc(READSTR, 0);
	if(p == NULL)
		error(Enomem);
	l = snprint(p, READSTR, "rcr %#8.8ux\n", ctlr->rcr);
	l += snprint(p+l, READSTR-l, "multicast %ud\n", ctlr->mcast);
	l += snprint(p+l, READSTR-l, "ierrs %d\n", ctlr->ierrs);
	l += snprint(p+l, READSTR-l, "etxth %d\n", ctlr->etxth);
	l += snprint(p+l, READSTR-l, "taligned %d\n", ctlr->taligned);
	l += snprint(p+l, READSTR-l, "tunaligned %d\n", ctlr->tunaligned);
	ctlr->dis += csr16r(ctlr, Dis);
	l += snprint(p+l, READSTR-l, "dis %d\n", ctlr->dis);
	ctlr->fcsc += csr16r(ctlr, Fcsc);
	l += snprint(p+l, READSTR-l, "fcscnt %d\n", ctlr->fcsc);
	ctlr->rec += csr16r(ctlr, Rec);
	l += snprint(p+l, READSTR-l, "rec %d\n", ctlr->rec);

	l += snprint(p+l, READSTR-l, "Tcr %#8.8lux\n", csr32r(ctlr, Tcr));
	l += snprint(p+l, READSTR-l, "Config0 %#2.2ux\n", csr8r(ctlr, Config0));
	l += snprint(p+l, READSTR-l, "Config1 %#2.2ux\n", csr8r(ctlr, Config1));
	l += snprint(p+l, READSTR-l, "Msr %#2.2ux\n", csr8r(ctlr, Msr));
	l += snprint(p+l, READSTR-l, "Config3 %#2.2ux\n", csr8r(ctlr, Config3));
	l += snprint(p+l, READSTR-l, "Config4 %#2.2ux\n", csr8r(ctlr, Config4));

	l += snprint(p+l, READSTR-l, "Bmcr %#4.4ux\n", csr16r(ctlr, Bmcr));
	l += snprint(p+l, READSTR-l, "Bmsr %#4.4ux\n", csr16r(ctlr, Bmsr));
	l += snprint(p+l, READSTR-l, "Anar %#4.4ux\n", csr16r(ctlr, Anar));
	l += snprint(p+l, READSTR-l, "Anlpar %#4.4ux\n", csr16r(ctlr, Anlpar));
	l += snprint(p+l, READSTR-l, "Aner %#4.4ux\n", csr16r(ctlr, Aner));
	l += snprint(p+l, READSTR-l, "Nwaytr %#4.4ux\n", csr16r(ctlr, Nwaytr));
	snprint(p+l, READSTR-l, "Cscr %#4.4ux\n", csr16r(ctlr, Cscr));
	n = readstr(offset, a, n, p);
	kfree(p);

	return n;
}

static int
rtl8139reset(struct ctlr* ctlr)
{
	int timeo;

	/* stop interrupts */
	csr16w(ctlr, Imr, 0);
	csr16w(ctlr, Isr, ~0);
	csr32w(ctlr, TimerInt, 0);

	/*
	 * Soft reset the controller.
	 */
	csr8w(ctlr, Cr, Rst);
	for(timeo = 0; timeo < 1000; timeo++){
		if(!(csr8r(ctlr, Cr) & Rst))
			return 0;
		delay(1);
	}

	return -1;
}

static void
rtl8139halt(struct ctlr* ctlr)
{
	int i;

	csr8w(ctlr, Cr, 0);
	csr16w(ctlr, Imr, 0);
	csr16w(ctlr, Isr, ~0);
	csr32w(ctlr, TimerInt, 0);

	for(i = 0; i < Ntd; i++){
		if(ctlr->td[i].bp == NULL)
			continue;
		freeb(ctlr->td[i].bp);
		ctlr->td[i].bp = NULL;
	}
}

static void
rtl8139shutdown(struct ether *edev)
{
	struct ctlr *ctlr;

	ctlr = edev->ctlr;
	ilock(&ctlr->ilock);
	rtl8139halt(ctlr);
	rtl8139reset(ctlr);
	iunlock(&ctlr->ilock);
}

static void
rtl8139init(struct ether* edev)
{
	int i;
	uint32_t r;
	struct ctlr *ctlr;
	uint8_t *alloc;

	ctlr = edev->ctlr;
	ilock(&ctlr->ilock);

	rtl8139halt(ctlr);

	/*
	 * MAC Address.
	 */
	r = (edev->ea[3]<<24)|(edev->ea[2]<<16)|(edev->ea[1]<<8)|edev->ea[0];
	csr32w(ctlr, Idr0, r);
	r = (edev->ea[5]<<8)|edev->ea[4];
	csr32w(ctlr, Idr0+4, r);

	/*
	 * Receiver
	 */
	alloc = (uint8_t*)ROUNDUP((uintptr_t)ctlr->alloc, 32);
	ctlr->rbstart = alloc;
	alloc += ctlr->rblen+16;
	memset(ctlr->rbstart, 0, ctlr->rblen+16);
	csr32w(ctlr, Rbstart, PCIWADDR(ctlr->rbstart));
	ctlr->rcr = Rxfth256|Rblen|Mrxdmaunlimited|Ab|Am|Apm;

	/*
	 * Transmitter.
	 */
	for(i = 0; i < Ntd; i++){
		ctlr->td[i].tsd = Tsd0+i*4;
		ctlr->td[i].tsad = Tsad0+i*4;
		ctlr->td[i].data = alloc;
		alloc += Tdbsz;
		ctlr->td[i].bp = NULL;
	}
	ctlr->ntd = ctlr->tdh = ctlr->tdi = 0;
	ctlr->etxth = 128/32;

	/*
	 * Enable receiver/transmitter.
	 * Need to enable before writing the Rcr or it won't take.
	 */
	csr8w(ctlr, Cr, Te|Re);
	csr32w(ctlr, Tcr, Mtxdma2048);
	csr32w(ctlr, Rcr, ctlr->rcr);
	csr32w(ctlr, Mar0,   0);
	csr32w(ctlr, Mar0+4, 0);
	ctlr->mchash = 0;

	/*
	 * Interrupts.
	 */
	csr32w(ctlr, TimerInt, 0);
	csr16w(ctlr, Imr, Serr|Timerbit|Fovw|PunLc|Rxovw|Ter|Tok|Rer|Rok);
	csr32w(ctlr, Mpc, 0);

	iunlock(&ctlr->ilock);
}

static void
rtl8139attach(struct ether* edev)
{
	struct ctlr *ctlr;

	if(edev == NULL) {
		printd("rtl8139attach: NULL edev\n");
		return;
	}
	ctlr = edev->ctlr;
	if(ctlr == NULL) {
		printd("rtl8139attach: NULL ctlr for struct ether %#p\n", edev);
		return;
	}
	qlock(&ctlr->alock);
	if(ctlr->alloc == NULL){
		ctlr->rblen = 1<<((Rblen>>RblenSHIFT)+13);
		ctlr->alloc = kmalloc(ctlr->rblen + 16 + Ntd * Tdbsz + 32, 0);
		if(ctlr->alloc == NULL) {
			qunlock(&ctlr->alock);
			error(Enomem);
		}
		rtl8139init(edev);
		kickdev = edev;
		addclock0link(kickme, 7);
	}
	qunlock(&ctlr->alock);
}

static void
rtl8139txstart(struct ether* edev)
{
	Td *td;
	int size;
	struct block *bp;
	struct ctlr *ctlr;

	ctlr = edev->ctlr;
	while(ctlr->ntd < Ntd){
		bp = qget(edev->oq);
		if(bp == NULL)
			break;
		size = BLEN(bp);

		td = &ctlr->td[ctlr->tdh];
		if(((uintptr_t)bp->rp) & 0x03){
			memmove(td->data, bp->rp, size);
			freeb(bp);
			csr32w(ctlr, td->tsad, PCIWADDR(td->data));
			ctlr->tunaligned++;
		}
		else{
			td->bp = bp;
			csr32w(ctlr, td->tsad, PCIWADDR(bp->rp));
			ctlr->taligned++;
		}
		csr32w(ctlr, td->tsd, (ctlr->etxth<<EtxthSHIFT)|size);

		ctlr->ntd++;
		ctlr->tdh = NEXT(ctlr->tdh, Ntd);
	}
}

static void
rtl8139transmit(struct ether* edev)
{
	struct ctlr *ctlr;

	ctlr = edev->ctlr;
	ilock(&ctlr->tlock);
	rtl8139txstart(edev);
	iunlock(&ctlr->tlock);
}

static void
rtl8139receive(struct ether* edev)
{
	struct block *bp;
	struct ctlr *ctlr;
	uint16_t capr;
	uint8_t cr, *p;
	int l, length, status;

	ctlr = edev->ctlr;

	/*
	 * Capr is where the host is reading from,
	 * Cbr is where the NIC is currently writing.
	 */
	if(ctlr->rblen == 0)
		return;		/* not attached yet (shouldn't happen) */
	capr = (csr16r(ctlr, Capr)+16) % ctlr->rblen;
	while(!(csr8r(ctlr, Cr) & Bufe)){
		p = ctlr->rbstart+capr;

		/*
		 * Apparently the packet length may be 0xFFF0 if
		 * the NIC is still copying the packet into memory.
		 */
		length = (*(p+3)<<8)|*(p+2);
		if(length == 0xFFF0)
			break;
		status = (*(p+1)<<8)|*p;

		if(!(status & Rcok)){
			if(status & (Ise|Fae))
				edev->frames++;
			if(status & Crc)
				edev->crcs++;
			if(status & (Runt|Long))
				edev->buffs++;

			/*
			 * Reset the receiver.
			 * Also may have to restore the multicast list
			 * here too if it ever gets used.
			 */
			cr = csr8r(ctlr, Cr);
			csr8w(ctlr, Cr, cr & ~Re);
			csr32w(ctlr, Rbstart, PCIWADDR(ctlr->rbstart));
			csr8w(ctlr, Cr, cr);
			csr32w(ctlr, Rcr, ctlr->rcr);

			continue;
		}

		/*
		 * Receive Completed OK.
		 * Very simplistic; there are ways this could be done
		 * without copying, but the juice probably isn't worth
		 * the squeeze.
		 * The packet length includes a 4 byte CRC on the end.
		 */
		capr = (capr+4) % ctlr->rblen;
		p = ctlr->rbstart+capr;
		capr = (capr+length) % ctlr->rblen;
		if(status & Mar)
			ctlr->mcast++;

		if((bp = iallocb(length)) != NULL){
			if(p+length >= ctlr->rbstart+ctlr->rblen){
				l = ctlr->rbstart+ctlr->rblen - p;
				memmove(bp->wp, p, l);
				bp->wp += l;
				length -= l;
				p = ctlr->rbstart;
			}
			if(length > 0){
				memmove(bp->wp, p, length);
				bp->wp += length;
			}
			bp->wp -= 4;
			etheriq(edev, bp, 1);
		}

		capr = ROUNDUP(capr, 4);
		csr16w(ctlr, Capr, capr-16);
	}
}

static void
rtl8139interrupt(Ureg*, void* arg)
{
	Td *td;
	struct ctlr *ctlr;
	struct ether *edev;
	int isr, msr, tsd;

	edev = arg;
	ctlr = edev->ctlr;
	if(ctlr == NULL) {	/* not attached yet? (shouldn't happen) */
		printd("rtl8139interrupt: interrupt for unattached struct ether %#p\n",
			edev);
		return;
	}

	while((isr = csr16r(ctlr, Isr)) != 0){
		csr16w(ctlr, Isr, isr);
		if(ctlr->alloc == NULL) {
			printd("rtl8139interrupt: interrupt for unattached struct ctlr "
				"%#p port %#p\n", ctlr, (void *)ctlr->port);
			return;	/* not attached yet (shouldn't happen) */
		}
		if(isr & (Fovw|PunLc|Rxovw|Rer|Rok)){
			rtl8139receive(edev);
			if(!(isr & Rok))
				ctlr->ierrs++;
			isr &= ~(Fovw|Rxovw|Rer|Rok);
		}

		if(isr & (Ter|Tok)){
			ilock(&ctlr->tlock);
			while(ctlr->ntd){
				td = &ctlr->td[ctlr->tdi];
				tsd = csr32r(ctlr, td->tsd);
				if(!(tsd & (Tabt|Tun|Tcok)))
					break;

				if(!(tsd & Tcok)){
					if(tsd & Tun){
						if(ctlr->etxth < ETHERMAXTU/32)
							ctlr->etxth++;
					}
					edev->oerrs++;
				}

				if(td->bp != NULL){
					freeb(td->bp);
					td->bp = NULL;
				}

				ctlr->ntd--;
				ctlr->tdi = NEXT(ctlr->tdi, Ntd);
			}
			rtl8139txstart(edev);
			iunlock(&ctlr->tlock);
			isr &= ~(Ter|Tok);
		}

		if(isr & PunLc){
			/*
			 * Maybe the link changed - do we care very much?
			 */
			msr = csr8r(ctlr, Msr);
			if(!(msr & Linkb)){
				if(!(msr & Speed10) && edev->mbps != 100){
					edev->mbps = 100;
					qsetlimit(edev->oq, 256*1024);
				}
				else if((msr & Speed10) && edev->mbps != 10){
					edev->mbps = 10;
					qsetlimit(edev->oq, 65*1024);
				}
			}
			isr &= ~(Clc|PunLc);
		}

		/*
		 * Only Serr|Timerbit should be left by now.
		 * Should anything be done to tidy up? TimerInt isn't
		 * used so that can be cleared. A PCI bus error is indicated
		 * by Serr, that's pretty serious; is there anyhing to do
		 * other than try to reinitialise the chip?
		 */
		if((isr & (Serr|Timerbit)) != 0){
			iprint("rtl8139interrupt: imr %#4.4ux isr %#4.4ux\n",
				csr16r(ctlr, Imr), isr);
			if(isr & Timerbit)
				csr32w(ctlr, TimerInt, 0);
			if(isr & Serr)
				rtl8139init(edev);
		}
	}
}

static void
kickme(void)
{
	if(kickdev)
		rtl8139interrupt(NULL, kickdev);
}

static struct ctlr*
rtl8139match(struct ether* edev, int id)
{
	Pcidev *p;
	struct ctlr *ctlr;
	int i, port;

	/*
	 * Any adapter matches if no edev->port is supplied,
	 * otherwise the ports must match.
	 */
	for(ctlr = ctlrhead; ctlr != NULL; ctlr = ctlr->next){
		if(ctlr->active)
			continue;
		p = ctlr->pcidev;
		if(((p->did<<16)|p->vid) != id)
			continue;
		port = p->mem[0].bar & ~0x01;
		if(edev->port != 0 && edev->port != port)
			continue;

		if(ioalloc(port, p->mem[0].size, 0, "rtl8139") < 0){
			printd("rtl8139: port %#ux in use\n", port);
			continue;
		}

		if(pcigetpms(p) > 0){
			pcisetpms(p, 0);
	
			for(i = 0; i < 6; i++)
				pcicfgw32(p, PciBAR0+i*4, p->mem[i].bar);
			pcicfgw8(p, PciINTL, p->intl);
			pcicfgw8(p, PciLTR, p->ltr);
			pcicfgw8(p, PciCLS, p->cls);
			pcicfgw16(p, PciPCR, p->pcr);
		}

		ctlr->port = port;
		if(rtl8139reset(ctlr)) {
			iofree(port);
			continue;
		}
		pcisetbme(p);

		ctlr->active = 1;
		return ctlr;
	}
	return NULL;
}

static struct {
	char*	name;
	int	id;
} rtl8139pci[] = {
	{ "rtl8139",	(0x8139<<16)|0x10EC, },	/* generic */
	{ "smc1211",	(0x1211<<16)|0x1113, },	/* SMC EZ-Card */
	{ "dfe-538tx",	(0x1300<<16)|0x1186, }, /* D-Link DFE-538TX */
	{ "dfe-560txd",	(0x1340<<16)|0x1186, }, /* D-Link DFE-560TXD */
	{ NULL },
};

static int
rtl8139pnp(struct ether* edev)
{
	int i, id;
	Pcidev *p;
	struct ctlr *ctlr;
	uint8_t ea[Eaddrlen];

	/*
	 * Make a list of all ethernet controllers
	 * if not already done.
	 */
	if(ctlrhead == NULL){
		p = NULL;
		while(p = pcimatch(p, 0, 0)){
			if(p->ccrb != 0x02 || p->ccru != 0)
				continue;
			ctlr = kmalloc(sizeof(struct ctlr), 0);
			if(ctlr == NULL)
				error(Enomem);
			ctlr->pcidev = p;
			ctlr->id = (p->did<<16)|p->vid;

			if(ctlrhead != NULL)
				ctlrtail->next = ctlr;
			else
				ctlrhead = ctlr;
			ctlrtail = ctlr;
		}
	}

	/*
	 * Is it an RTL8139 under a different name?
	 * Normally a search is made through all the found controllers
	 * for one which matches any of the known vid+did pairs.
	 * If a vid+did pair is specified a search is made for that
	 * specific controller only.
	 */
	id = 0;
	for(i = 0; i < edev->nopt; i++){
		if(cistrncmp(edev->opt[i], "id=", 3) == 0)
			id = strtol(&edev->opt[i][3], NULL, 0);
	}

	ctlr = NULL;
	if(id != 0)
		ctlr = rtl8139match(edev, id);
	else for(i = 0; rtl8139pci[i].name; i++){
		if((ctlr = rtl8139match(edev, rtl8139pci[i].id)) != NULL)
			break;
	}
	if(ctlr == NULL)
		return -1;

	edev->ctlr = ctlr;
	edev->port = ctlr->port;
	edev->irq = ctlr->pcidev->intl;
	edev->tbdf = ctlr->pcidev->tbdf;

	/*
	 * Check if the adapter's station address is to be overridden.
	 * If not, read it from the device and set in edev->ea.
	 */
	memset(ea, 0, Eaddrlen);
	if(memcmp(ea, edev->ea, Eaddrlen) == 0){
		i = csr32r(ctlr, Idr0);
		edev->ea[0] = i;
		edev->ea[1] = i>>8;
		edev->ea[2] = i>>16;
		edev->ea[3] = i>>24;
		i = csr32r(ctlr, Idr0+4);
		edev->ea[4] = i;
		edev->ea[5] = i>>8;
	}

	edev->arg = edev;
	edev->attach = rtl8139attach;
	edev->transmit = rtl8139transmit;
	edev->interrupt = rtl8139interrupt;
	edev->ifstat = rtl8139ifstat;

	edev->promiscuous = rtl8139promiscuous;
	edev->multicast = rtl8139multicast;
	edev->shutdown = rtl8139shutdown;

	/*
	 * This should be much more dynamic but will do for now.
	 */
	if((csr8r(ctlr, Msr) & (Speed10|Linkb)) == 0)
		edev->mbps = 100;

	return 0;
}

void
ether8139link(void)
{
	addethercard("rtl8139", rtl8139pnp);
}
