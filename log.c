/*
 * nubfs, part 4: The Log
 *
 *	based on "A Flash File System for Inferno", C H Forsyth, 2002
 */

#include	"dat.h"
#include	"fns.h"

typedef struct LogBlk LogBlk;
typedef struct LogBlkHdr LogBlkHdr;
typedef struct LogBlkQ LogBlkQ;
typedef struct LogBuf LogBuf;
typedef struct LogSeg LogSeg;

enum{
	/* log block size */
//	Logbshift=	15,
	Logbshift=	12,	/* smaller for testing */
	Blksize=		1<<Logbshift,

	/* block sequence numbers are partitioned into sweep sets */
	Logswshift=	2,
	Nsweep=		1<<Logswshift,
	Swmask=		Nsweep-1,
};

/*
 * unpacked header, without data, always in memory
 */
struct LogBlk
{
	u64int	seq;
	u64int	base;
	u32int	used;	/* next available offset in bytes */
	u32int	limit;		/* size in bytes less auxiliary data area */
	uchar	tag;		/* Tlog0, ... Tlog3: type and sequence set */

	LogBlk*	next;
};

/*
 * log block, header and data
 */
struct LogBuf
{
	LogBlk*	blk;		/* block to which this data belongs */
	u64int	seq;
	u32int	used;
	u32int	limit;
	u32int	size;
	uchar	tag;
	uchar	buf[Blksize];
};

/*
 * header storage format: at front and back of block:
 * if they don't match, the block wasn't completely written.
 */
struct LogBlkHdr
{
	uchar	tag;
	uchar	seq[8];
	uchar	used[3];
	uchar	csum[4];
};
#define	LOGBLKHDRLEN	(1+8+3+4)

/* GBIT8, PBIT8, 16, 32, 64 in fcall.h */
#define	GBIT24(p)	((p)[0]|((p)[1]<<8)|((p)[2]<<16))
#define	PBIT24(p,v)	(p)[0]=(v);(p)[1]=(v)>>8;(p)[2]=(v)>>16;

struct LogBlkQ
{
	LogBlk*	head;
	LogBlk*	tail;
	int	len;
};

struct LogSeg
{
	u32int	bsize;
	uint	bshift;
	uchar	tag;		/* tag for blocks in this segment */
	u64int	gen;		/* next sequence number in this segment */
	LogBuf	page;	/* current output (if any) */
	LogBlkQ	blocks;
};

#define	swsucc(n)	(((n)+1)&Swmask)
#define	swpred(n)	(((n)-1)&Swmask)
#define	swset(n) ((n)&Swmask)
#define	mktag(n)	(Tlog0+((n)&Swmask))

#define	nearlyfull(lg)	((lg)->empty.len < 3)

struct LogFile
{
	int	fd;
	int	(*copy)(LogEntry*);
	u64int	length;
	u32int	bsize;
	uint	bshift;
	uint	nblocks;
	LogSeg	swept;
	LogSeg	active;
	LogBlkQ	empty;
	LogBlk	block[];
};

static void	bucketsort(LogBlkQ*, uint);
static void	readblk(LogFile*, uint, LogBlk*);
static void	readblkhdr(LogFile*, uint, int, LogBlk*);
static void	scanlogfile(LogFile*);
static LogBuf* segspace(LogFile*, LogSeg*, uint, int);
static void tack(LogBlkQ*, LogBlk*);
static void initseg(LogFile*, LogSeg*);
static void readlogpage(LogFile*, LogBlk*, LogBuf*);
static void allocpage(LogFile*, LogSeg*, int);
static void flushpage(LogFile*, LogBuf*);
static void cleanpage(LogFile*, LogBuf*);
static void writepage(LogFile*, LogBuf*);
static void sweeplog(LogFile*);
static void segappend(LogFile*, LogSeg*, LogEntry*, int);
static void printlog(LogEntry*, uint);

LogFile*
logopen(int fd, u64int length)
{
	LogFile *lg;
	uint nb;

	fmtinstall('L', fmtL);
	nb = length>>Logbshift;
	if(nb == 0)
		return nil;
	lg = emallocz(sizeof(*lg)+(nb*sizeof(lg->block[0])), 1);
	lg->fd = fd;
	lg->length = length;
	lg->nblocks = nb;
	lg->bsize = Blksize;
	lg->bshift = Logbshift;
	initseg(lg, &lg->swept);
	initseg(lg, &lg->active);
	scanlogfile(lg);
	return lg;
}

void
logsetcopy(LogFile *lg, int (*copy)(LogEntry*))
{
	lg->copy = copy;
}

static void
initseg(LogFile *lg, LogSeg *s)
{
	s->bsize = lg->bsize;
	s->bshift = lg->bshift;
	s->gen = 0;
	s->tag = Tnone;
	s->page.blk = nil;
	s->blocks.head = nil;
}

static void
setseg(LogSeg *s, int tag, LogBlkQ *q)
{
	s->tag = tag;
	s->blocks = *q;
	s->gen = q->len;
}

/*
 * read in order, to find sweep/active partition, and collecting empties
 */
static void
scanlogfile(LogFile *lg)
{
	int i, sweep, sweep0, sweep1;
	LogBlk *b;
	LogBlkQ sweeps[Nsweep];

	memset(sweeps, 0, sizeof(sweeps));
	sweep0 = sweep1 = -1;
	for(i = 0; i < lg->nblocks; i++){
		b = &lg->block[i];
		readblk(lg, i, b);
		if(debug['r'])
			fprint(2, "read block %d seq %lld tag %.2ux\n", i, b->seq, b->tag);
		switch(b->tag){
		case Tnone:
			/* append to empty list in order, for better locality */
			tack(&lg->empty, b);
			break;
		case Tlog0:
		case Tlog1:
		case Tlog2:
		case Tlog3:
			sweep = swset(b->tag);
			if(debug['l'])
				fprint(2, "Tlog%d block %d base %llud seq %llud used %ud\n", sweep, i, b->base, b->seq, b->used);
			if(sweep0 < 0)
				sweep0 = sweep;
			else if(sweep != sweep0){
				if(sweep1 < 0)
					sweep1 = sweep;
				else if(sweep != sweep1)
					error("too many log sweeps: %d %d %d", sweep0, sweep1, sweep);
			}
			tack(&sweeps[sweep], b);
			break;
		case Tboot:
			/* ignored */
			break;
		default:
			error("unknown tag: %#.2ux", b->tag);
		}
	}
	for(i=0; i<nelem(sweeps); i++)
		bucketsort(&sweeps[i], lg->nblocks);
	/* partition into swept (sweep1), then active (sweep0) */
	if(sweep0 < 0){
		/* both swept and active are empty; start active log */
		lg->active.tag = mktag(0);
		return;
	}
	if(sweep1 < 0){
		/* swept is empty; active has all blocks*/
		setseg(&lg->active, mktag(sweep0), &sweeps[sweep0]);
	}else{
		if(sweep0 == swsucc(sweep1)){
			/* swept blocks are successors in the cycle */
			int t = sweep0; sweep0 = sweep1; sweep1 = t;
		}
		if(sweep1 != swsucc(sweep0))
			error("log mis-swept: sweep0=%d sweep1=%d", sweep0, sweep1);
		setseg(&lg->swept, mktag(sweep1), &sweeps[sweep1]);
		setseg(&lg->active, mktag(sweep0), &sweeps[sweep0]);
	}
	print("%d log %dK blocks free\n", lg->empty.len, Blksize/1024);
	if(debug['p']){
		if(lg->swept.blocks.head != nil){
			print("swept:\n");
			logreplay(lg, 0, printlog);
		}
		if(lg->active.blocks.head != nil){
			print("active:\n");
			logreplay(lg, 1, printlog);
		}
		if(debug['x'])
			exits("debug");
	}
}

/*
 * reorder blocks by logical block number (sequence number),
 * checking for gaps, duplicates and errors
 */
static void
bucketsort(LogBlkQ *q, uint maxn)
{
	LogBlk **blks, *b;
	int i;
	uint minlog, maxlog, seq;
	uint n;

	if(q->head == nil)
		return;
	minlog = ~0;
	maxlog = 0;
	for(b = q->head; b != nil; b = b->next){
		seq = b->seq;
		if(seq < minlog)
			minlog = seq;
		if(seq > maxlog)
			maxlog = seq;
	}
	n = maxlog-minlog+1;
	if(n > maxn)
		error("sequence number span out of range %ud %ud (%ud > %ud)",
			minlog, maxlog, n, maxn);
	if(minlog != 0)
		error("block sequence does not start at 0");
	blks = emallocz(n*sizeof(*blks), 1);
	for(b = q->head; b != nil; b = b->next){
		seq = b->seq;
		//fprint(2, "seq %ud minseq %ud\n", seq, minlog);
		if(blks[seq-minlog] != nil)
			error("duplicate log seq %ud, set %d", seq, swset(b->tag));
		blks[seq-minlog] = b;
	}
	q->head = nil;
	q->len = 0;
	for(i=0; i<n; i++){
		if(blks[i] == nil)
			error("missing log seq %ud", minlog+i);
		tack(q, blks[i]);
	}
	free(blks);
}

static void
tack(LogBlkQ *q, LogBlk *b)
{
	b->next = nil;
	if(q->head == nil){
		q->len = 0;
		q->head = b;
	}else
		q->tail->next = b;
	q->tail = b;
	q->len++;
}

static LogBlk*
take(LogBlkQ *q)
{
	LogBlk *b;

	b = q->head;
	if(b != nil){
		q->len--;
		q->head = b->next;
	}
	return b;
}

/*
 * the values at head and tail of the block
 * must agree. if they don't, assume the write
 * was incomplete and the log is corrupt
 */
static void
readblk(LogFile *lg, uint bno, LogBlk *b)
{
	LogBlk h0, h1;

	memset(&h0, 0, sizeof(h0));
	readblkhdr(lg, bno, 0, &h0);
	readblkhdr(lg, bno, 1, &h1);
	if(h0.seq != h1.seq ||
	   h0.tag != h1.tag ||
	   h0.used != h1.used)
		error("log block %ud head/tail inconsistent", bno);
	*b = h0;
	b->base = (u64int)bno<<lg->bshift;
	b->limit = lg->bsize - LOGBLKHDRLEN;
	if(b->tag != 0){
		if(b->used < LOGBLKHDRLEN || b->used > b->limit)
			error("log block %ud used count %ud invalid", bno, b->used);
	}else{
		b->tag = Tnone;
		b->used = LOGBLKHDRLEN;
	}
	/* tag checked later */
}

static void
readblkhdr(LogFile *lg, uint bno, int top, LogBlk *b)
{
	LogBlkHdr h;
	u64int o;

	o = (u64int)bno << lg->bshift;
	if(top)
		o += lg->bsize - LOGBLKHDRLEN;
	//fprint(2, "pread %d %lld %ud\n", lg->fd, o, bno);
	if(pread(lg->fd, &h, LOGBLKHDRLEN, o) != LOGBLKHDRLEN)
		error("error reading log block %ud's tag", bno);
	b->tag = h.tag;
	b->seq = GBIT64(h.seq);
	b->used = GBIT24(h.used);
}

static void
readlogpage(LogFile *lg, LogBlk *b, LogBuf *page)
{
	if(pread(lg->fd, page->buf, lg->bsize, b->base) != lg->bsize)
		error("logreadblock: block base %lld: read error: %r", b->base);
	page->blk = b;
	page->seq = b->seq;
	page->used = b->used;
	page->tag = b->tag;
	page->limit = lg->bsize - LOGBLKHDRLEN;
	page->size = lg->bsize;
	//fprint(2, "read %llud used %ud\n", b->base, b->used);
	/* TO DO: csum */
}

static void
segappend(LogFile *lg, LogSeg *seg, LogEntry *l, int scavenging)
{
	uint i;
	int n;
	LogBuf *p;

	n = 0;
	for(i = 0; i < 2; i++){
		p = segspace(lg, seg, n, scavenging);
		n = logpack(p->buf+p->used, p->limit - p->used, l);
		if(n > 0){
			if(debug['l'])
				fprint(2, "log: %d bytes @ %d\n", n, p->used);
			p->used += n;
			return;
		}
		n = -n;
	}
	error("log buffer overflow");	/* it will never fit */
}

void
logappend(LogFile *lg, LogEntry *l)
{
	segappend(lg, &lg->active, l, 0);
}

void
logreplay(LogFile *lg, int which, void (*f)(LogEntry*, uint))
{
	LogEntry l;
	LogBlk *b;
	LogSeg *seg;
	LogBuf *page;
	uchar *p, *ep;
	uint n;

	seg = which? &lg->active: &lg->swept;
	page = &seg->page;
	for(b = seg->blocks.head; b != nil; b = b->next){
		readlogpage(lg, b, page);
		if(debug['p'] || debug['l'])
			print("block %lld %ud\n", b->base, b->used);
		p = page->buf + LOGBLKHDRLEN;
		ep = page->buf + page->used;
		for(; p != ep; p += n){
			n = logunpack(p, ep-p, &l);
			//print("replay @ %lud: ", p-page->buf);
			if(n == 0)
				error("log unpack: block %#llux: inconsistent length", b->seq);
			f(&l, p-page->buf);
		}
	}
}

static void
printlog(LogEntry *e, uint offset)
{
	print(" %ud: %L\n", offset, e);

}

/*
 * complete any partial sweep
 */
void
logcomplete(LogFile *lg)
{
	if(lg->swept.blocks.head != nil)
		sweeplog(lg);
}

void
logsweep(LogFile *lg)
{
	debug['l']++;
	debug['S']++;
	sweeplog(lg);
	debug['l']--;
	debug['S']--;
}

static void
sweeplog(LogFile *lg)
{
	uint n, s;
	LogBlk *b;
	LogBuf *page0, *page1;
	uchar *p, *ep;
	LogEntry l;
	u64int cmdseq;
	static int sweeps;

	page0 = &lg->active.page;	/* note: lg->active.page might be in use */
	flushpage(lg, page0);	/* push last chunk to storage */
	page1 = &lg->swept.page;
	if(lg->swept.blocks.head == nil){	/* new sweep */
		s = swset(lg->active.tag);
		lg->swept.tag = mktag(swsucc(s));
		if(debug['S'])
			fprint(2, "fresh sweep tag %#ux\n", lg->swept.tag);
		page1->blk = nil;
	}else{
		/* continue at tail page of existing sweep */
		if(debug['S'])
			fprint(2, "continue sweep tag %#ux head %#p tail %#p\n", lg->swept.tag,
				lg->swept.blocks.head, lg->swept.blocks.tail);
		readlogpage(lg, lg->swept.blocks.tail, page1);
	}
	cmdseq = 0;
	while((b = take(&lg->active.blocks)) != nil){
		readlogpage(lg, b, page0);
		p = page0->buf + LOGBLKHDRLEN;
		ep = page0->buf + page0->used;
		for(; p != ep; p += n){
			n = logunpack(p, ep-p, &l);
			if(debug['S'])
				print("@ %llud.%lud: ", b->seq, p-page0->buf);
			if(n == 0)
				error("copylog: log unpack: block %llud: error", b->seq);
			if(l.seq <= cmdseq){
				if(debug['S'])
					print("%L [seen]\n", &l);
				continue;
			}
			cmdseq = l.seq;
			switch(lg->copy(&l)){
			case 0:	/* discard */
				break;
			case 1:	/* keep, as-is */
				page1 = segspace(lg, &lg->swept, n, 1);
				if(page1 == nil)
					error("copylog: copy space exhausted");
				memmove(page1->buf+page1->used, p, n);
				page1->used += n;
				break;
			case 2:	/* keep, must repack */
				segappend(lg, &lg->swept, &l, 1);
				break;
			default:
				error("internal: bad return from log copy");
			}
		}
		flushpage(lg, page1);
		cleanpage(lg, page0);
		tack(&lg->empty, b);
	}
	/* active log is now empty: make swept log active*/
	lg->active = lg->swept;
	initseg(lg, &lg->swept);
	if(debug['q'] && ++sweeps >= debug['q'])
		exits("swept");
}

static LogBuf*
segspace(LogFile *lg, LogSeg *seg, uint nbytes, int scavenging)
{
	LogBuf *p;

	if(nbytes >= sizeof(p->buf))
		error("log entry would never fit page");
	p = &seg->page;
	if(p->blk == nil)
		allocpage(lg, seg, scavenging);
	if(p->used+nbytes > p->limit){
		flushpage(lg, p);
		allocpage(lg, seg, scavenging);
	}
	return p;
}

void
logflush(LogFile *lg)
{
	flushpage(lg, &lg->active.page);
}

static void
allocpage(LogFile *lg, LogSeg *seg, int scavenging)
{
	LogBuf *p;
	LogBlk *nb;

	p = &seg->page;
	if(!scavenging && nearlyfull(lg)){
		sweeplog(lg);
		if(nearlyfull(lg))// && !removing(p))
			raise("file system log full");
	}
	nb = take(&lg->empty);
	if(nb == nil)
		raise("log irrevocably full");
	p->blk = nb;
	p->tag = seg->tag;
	p->seq = seg->gen++;
	p->used = LOGBLKHDRLEN;
	p->size = lg->bsize;
	p->limit = p->size - LOGBLKHDRLEN;
	tack(&seg->blocks, nb);
	assert(seg->blocks.len == seg->gen);
}

static void
flushpage(LogFile *lg, LogBuf *p)
{
	LogBlk *b;

	b = p->blk;
	if(b == nil){
		if(debug['l'])
			fprint(2, "logflush: nil blk\n");
		return;
	}
	if(p->tag == b->tag && p->seq == b->seq && b->used == p->used){
		if(debug['l'])
			fprint(2, "logflush: base %llud tag %#ux seq %llud still used only %ud\n", b->base, p->tag, p->seq, p->used);
		return;
	}
	b->tag = p->tag;
	b->seq = p->seq;
	b->used = p->used;
	if(debug['l'] || debug['S'])
		fprint(2, "logflush: base %llud tag %#ux seq %llud used %ud\n", b->base, p->tag, p->seq, p->used);
	writepage(lg, p);
}
	
static void
cleanpage(LogFile *lg, LogBuf *p)
{
	LogBlk *b;

	b = p->blk;
	if(b == nil){
		if(debug['l'])
			fprint(2, "cleanpage: nil blk\n");
		return;
	}
	if(debug['l'])
		fprint(2, "cleanpage: base %llud old tag %#ux old seq %llud used %ud\n", b->base, p->tag, p->seq, p->used);
	p->tag = Tnone;
	p->used = LOGBLKHDRLEN;
	b->tag = Tnone;
	b->used = p->used;
	b->seq = ~0LL;
	if(debug['l'])
		fprint(2, "cleanpage: base %llud tag %#ux seq %llud used %ud\n", b->base, p->tag, p->seq, p->used);
	writepage(lg, p);
}

static void
writepage(LogFile *lg, LogBuf *p)
{
	LogBlkHdr *h;

	h = (LogBlkHdr*)p->buf;
	h->tag = p->blk->tag;
	PBIT24(h->used, p->used);
	PBIT64(h->seq, p->blk->seq);
	memset(h->csum, 0, sizeof(h->csum));	/* TO DO */
	memset(p->buf+p->used, 0, p->limit - p->used);
	memmove(&p->buf[p->size - LOGBLKHDRLEN], h, LOGBLKHDRLEN);	/* copy of header at tail */
	if(pwrite(lg->fd, p->buf, p->size, p->blk->base) != p->size)
		error("log write error: base %llud: %r", p->blk->base);
}

/*
Notes:
	x cmd sequence
	x move pack/unpack
*/
