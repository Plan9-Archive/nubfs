/*
 * nubfs, part 2: The 9P Interface
 *
 *	based on "A Flash File System for Inferno", C H Forsyth, 2002
 *
 * load users through control device
 */

#include	"dat.h"
#include	"fns.h"

enum
{
	Maxfdata	= 128*1024,
};

typedef struct Req Req;
struct Req {
	uchar	data[IOHDRSZ+Maxfdata];	/* should allocate */
	uchar*	statbuf;
	usize	statsize;
	int	n;
	Fcall	t;
	Fcall	r;
	ulong	pid;
};

int	messagesize = IOHDRSZ+Maxfdata;

static void server(int);
static int readreq(int, Req*);
static void reply(int, Req*);
static void replyerr(int, Req*, char*);
static Fid* findfid(u32int);
static Fid*	newfid(u32int, String*);
static void	clunkfid(Fid*);

char*	mountpoint = "/n/kfs";
char*	logname;	/* TO DO: pair */
char*	diskname;
char*	srvfile = "#s/nubfs";
int	exiting;

static void
usage(void)
{
	fprint(2, "usage: %s [-Ddebug] [-s srvname] datafile logfile\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	int dfd, lfd, srvfd, pip[2];
	char *p;
	Dir *d;
	LogFile *lf;

	ARGBEGIN{
	case 'D':
		p = ARGF();
		if(p != nil && *p){
			for(; *p; p++)
				debug[*p&0xFF] = 1;
		}
		break;
	case 's':
		srvfile = smprint("#s/%s", EARGF(usage()));
		break;
	default:
		usage();
	}ARGEND

	if(argc != 2)
		usage();
	diskname = argv[0];
	logname = argv[1];

	quotefmtinstall();
	fmtinstall('F', fcallfmt);
	fmtinstall('D', dirfmt);
	fmtinstall('M', dirmodefmt);

	lfd = open(logname, ORDWR);
	if(lfd < 0)
		error("can't open %s: %r", logname);
	d = dirfstat(lfd);
	if(d == nil)
		error("can't fstat %s: %r", logname);
	lf = logopen(lfd, d->length);
	free(d);

	dfd = open(diskname, ORDWR);
	if(dfd < 0)
		error("can't open %s: %r", diskname);
	d = dirfstat(dfd);
	if(d == nil)
		error("can't fstat %s: %r", diskname);
	
	nubinit(lf, diskinit(dfd, 1024, 0, d->length), getuser());
	free(d);

	if(debug['R'] == 0)
		nubreplay();

	if(pipe(pip) < 0)
		error("can't pipe: %r");
	srvfd = create(srvfile, OWRITE|ORCLOSE, 0600);
	if(srvfd < 0)
		error("can't create %s: %r", srvfile);
	fprint(srvfd, "%d", pip[1]);
	close(pip[1]);
	switch(rfork(RFFDG|RFPROC|RFNAMEG|RFNOTEG)){
	case 0:
		server(pip[0]);
		exits(nil);
	case -1:
		error("can't fork: %r");
		break;
	default:
		exits(nil);
	}
}

static void rversion(Req*);
static void rauth(Req*);
static void rattach(Req*);
static void rwalk(Req*);
static void ropen(Req*);
static void rcreate(Req*);
static void rread(Req*);
static void rwrite(Req*);
static void rclunk(Req*);
static void rremove(Req*);
static void rstat(Req*);
static void rwstat(Req*);

static void (*fcalls[])(Req*) =
{
	[Tversion] rversion,
	[Tauth] rauth,
	[Tattach] rattach,
	[Twalk] rwalk,
	[Topen] ropen,
	[Tcreate] rcreate,
	[Tread] rread,
	[Twrite] rwrite,
	[Tclunk] rclunk,
	[Tremove] rremove,
	[Tstat] rstat,
	[Twstat] rwstat,
};

void
srvexits(char *e)
{
	fprint(2, "server ends: flush\n");
	nubflush();
	exits(e);
}

static void
server(int fd)
{
	Req *r;
	void (*op)(Req*);
	char err[ERRMAX];

	rfork(RFCNAMEG);
	atexit(nubflush);
	r = emallocz(sizeof(*r), 1);
	r->pid = getpid();
	while(!exiting && readreq(fd, r) > 0){
		if(debug['9'])
			fprint(2, "nubfs: %lud: <-%F\n", r->pid, &r->t);
		if(r->t.type >= nelem(fcalls) || (op = fcalls[r->t.type]) == nil){
			replyerr(fd, r, "invalid 9p operation");
			break;
		}
		if(waserror()){
			rerrstr(err, sizeof(err));
			replyerr(fd, r, err);
		}else{
			(*op)(r);
			reply(fd, r);
			poperror();
		}
	}
	srvexits(nil);
}

static int
readreq(int fd, Req *r)
{
	char buf[ERRMAX];

	r->n = read9pmsg(fd, r->data, messagesize);
	if(r->n > 0){
		if(convM2S(r->data, r->n, &r->t) == 0)
			error("bad message format");
		return r->n;
	}
	if(r->n < 0){
		rerrstr(buf, sizeof(buf));
		if(buf[0]=='\0' || strstr(buf, "hungup"))
			return 0;
		error("mount read");
	}
	return 0;
}

static void
reply(int fd, Req *r)
{
	int n;

	r->r.tag = r->t.tag;
	r->r.type = r->t.type+1;
	if(debug['9'])
		fprint(2, "nubfs %lud: ->%F\n", r->pid, &r->r);
	n = convS2M(&r->r, r->data, messagesize);
	if(n == 0)
		error("convS2M error on write");
	if(write(fd, r->data, n) != n)
		error("mount write");
}

static void
replyerr(int fd, Req *r, char *msg)
{
	int n;

	r->r.tag = r->t.tag;
	r->r.type = Rerror;
	r->r.ename = msg;
	if(debug['9'])
		fprint(2, "nubfs %lud: ->%F\n", r->pid, &r->r);
	n = convS2M(&r->r, r->data, messagesize);
	if(n == 0)
		error("convS2M error on write");
	if(write(fd, r->data, n) != n)
		error("mount write");
}

static void
rversion(Req *r)
{
	if(r->t.msize < 256)
		raise("message size too small");
	if(messagesize > r->t.msize)
		messagesize = r->t.msize;
	r->r.msize = messagesize;
	r->r.version = VERSION9P;
}

static void
rauth(Req*)
{
	raise("authentication not required");	/* TO DO */
}

static void
rflush(Req*)
{
	/* "synchronous, so easy" */
}

static void
rattach(Req *r)
{
	Fid *f;

	if(r->t.afid != NOFID)
		raise(Eauth);
	f = newfid(r->t.fid, string(r->t.uname));
	if(f == nil)
		raise(Efidinuse);
	if(waserror()){
		clunkfid(f);
		raise(nil);
	}
	nubattach(f, string(r->t.uname), r->t.aname);
	poperror();
	r->r.qid = f->entry->qid;
}

static void
rwalk(Req *r)
{
	Fid *f, *nf;
	Walkqid *wq;

	f = findfid(r->t.fid);
	if(r->t.newfid != NOFID){
		nf = newfid(r->t.newfid, f->user);
		if(nf == nil)
			raise(Efidinuse);
	}else
		nf = nil;
	if(waserror()){
		if(nf != nil)
			clunkfid(nf);
		raise(nil);
	}
	wq = nubwalk(f, nf, r->t.nwname, r->t.wname);
	if(wq->nqid != r->t.nwname && nf != nil)
		clunkfid(nf);
	poperror();
	r->r.nwqid = wq->nqid;
	memmove(r->r.wqid, wq->qid, wq->nqid*sizeof(*wq->qid));
	free(wq);
}

static void
ropen(Req *r)
{
	Fid *f;

	f = findfid(r->t.fid);
	if(f->open >= 0)
		raise(Eopened);
	nubopen(f, r->t.mode);
	r->r.qid = f->entry->qid;
	r->r.iounit = messagesize-IOHDRSZ;
}

static void
rcreate(Req *r)
{
	Fid *f;

	f = findfid(r->t.fid);
	if(f->open >= 0)
		raise(Eopened);
	nubcreate(f, r->t.name, r->t.mode, r->t.perm);
	r->r.qid = f->entry->qid;
	r->r.iounit = messagesize-IOHDRSZ;
}

static void
rread(Req *r)
{
	Fid *f;
	usize n;

	f = findfid(r->t.fid);
	n = r->t.count;
	if(n > sizeof(r->data)-IOHDRSZ)
		n = sizeof(r->data)-IOHDRSZ;
	r->r.count = nubread(f, r->data+IOHDRSZ, n, r->t.offset);
	r->r.data = (char*)r->data+IOHDRSZ;
}

static void
rwrite(Req *r)
{
	Fid *f;
	usize n;

	f = findfid(r->t.fid);
	n = r->t.count;
	if(n > sizeof(r->data))
		raise(Ecount);	/* can't happen */
	r->r.count = nubwrite(f, r->t.data, n, r->t.offset);
}

static void
rclunk(Req *r)
{
	Fid *f;

	f = findfid(r->t.fid);
	clunkfid(f);
}

static void
rremove(Req *r)
{
	Fid *f;

	f = findfid(r->t.fid);
	if(waserror()){
		clunkfid(f);	/* remove(5) requires fid to be clunked even on error */
		raise(nil);	/* propagate nubremove's error, though */
	}
	nubremove(f);
	poperror();
	clunkfid(f);
}

static void
rstat(Req *r)
{
	Fid *f;
	Dir d;
	usize n;

	f = findfid(r->t.fid);
	nubstat(f, &d);	/* note that d refers to volatile strings */
	n = sizeD2M(&d);
	if(n > r->statsize){
		free(r->statbuf);
		r->statbuf = emallocz(n, 0);
		r->statsize = n;
	}
	r->r.nstat = convD2M(&d, r->statbuf, r->statsize);
	r->r.stat = r->statbuf;
}

static void
rwstat(Req *r)
{
	Fid *f;
	Dir d;
	char *strs;

	f = findfid(r->t.fid);
	if(r->t.nstat > 32*1024)
		raise(Estatsize);
	strs = emallocz(r->t.nstat, 0);
	if(waserror()){
		free(strs);
		raise(nil);
	}
	if(convM2D(r->t.stat, r->t.nstat, &d, strs) == 0)
		raise("wstat -- conversion botch");
	nubwstat(f, &d);
	poperror();
	free(strs);
}

/*
 * fid maps
 */

enum{
	FIDSIZE=127,
};

static Fid	*fids[FIDSIZE];

static Fid**
hashfid(u32int fid)
{
	Fid **hp, *f;

	for(hp = &fids[fid&(FIDSIZE-1)]; (f = *hp) != nil; hp = &f->next)
		if(f->fid == fid)
			break;
	return hp;
}

static Fid*
findfid(u32int fid)
{
	Fid *f;

	f = *hashfid(fid);
	if(f == nil)
		raise(Ebadfid);
	return f;
}

static Fid*
newfid(u32int fid, String *user)
{
	Fid *f, **hp;

	hp = hashfid(fid);
	if(*hp != nil)
		return nil;
	f = mkfid(fid, user);
	*hp = f;
	return f;
}

static void
clunkfid(Fid *f)
{
	Fid **hp;

	hp = hashfid(f->fid);
	if((f = *hp) != nil){
		*hp = f->next;
		putfid(f);
	}else
		fprint(2, "nubfs: clunkfid no fid %ud\n", f->fid);	/* eventually, fatal */
}
