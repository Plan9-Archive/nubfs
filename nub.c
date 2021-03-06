/*
 * nubfs, part 1: The File System
 *
 *	based on "A Flash File System for Inferno", C H Forsyth, 2002
 *
 */

#include	"dat.h"
#include	"fns.h"

static String*	user;
static Entry*	root;
static Entry*	altroot;
static Disk*	disk;
static LogFile*	thelog;

static Dir*	e2d(Entry*);
static int accessok(Entry*, String*, uint);
static int nameexists(Entry*, char*);
static void checkfilename(char*);
static int leadseither(String*, String*, char*);
static void nublog(LogEntry, void*, usize);
static void nubnoexcl(Entry*, Fid*);
static int nubexcl(Entry*, Fid*);

void
nubinit(LogFile *alog, Disk *adisk, char *uid)
{
	thelog = alog;
	disk = adisk;
	user = string(uid);
	root = mkentry(nil, "", (Qid){0, 0, QTDIR}, DMDIR|0775, user, user, NOW, 0);
	altroot = mkentry(nil, "", (Qid){0, 0, QTDIR}, DMDIR|0555, user, user, NOW, 0);
	ctlinit(altroot, user);
	usersinit(altroot, user);
	logsetcopy(thelog, copyentry);
}

void
nubreplay(void)
{
	putpath(root);
	replayinit(disk);
	logreplay(thelog, 0, replayentry);	/* swept prefix */
	logreplay(thelog, 1, replayentry);	/* tail of active */
	logcomplete(thelog);	/* finish any partial sweep */
}

void
nubflush(void)
{
	/* could put Mark here, provided replicas can't then diverge */
	logflush(thelog);
}

void
nubsweep(void)
{
	logsweep(thelog);
}

Fid*
nubattach(Fid *f, char *uname, char *aname)
{
	String *uid;

	if(aname[0] != 0){
		if(strcmp(aname, "ctl") == 0)
			f->entry = altroot;
		else
			raise(Eattach);
	}else
		f->entry = root;
	incref(f->entry);
	uid = name2uid(uname);
	if(uid == nil)
		uid = string(uname);	/* bootstrap */
	f->user = uid;
	return f;
}

static Entry*
walk1(Entry *e, char *name, String *uid)
{
	Entry *f;

	if((e->mode & DMDIR) == 0)
		raise(Edir1);
	if(!accessok(e, uid, DMEXEC))
		raise(Eperm);
	if(strcmp(name, ".") == 0)
		return e;	/* keeps reference */
	if(strcmp(name, "..") == 0){
		f = e->parent;
		if(f == nil)
			return e;
	}else{
		f = e->files;
		for(;;){
			if(f == nil)
				raise(Enonexist);
			if(strcmp(f->name, name) == 0)
				break;
			f = f->dnext;
		}
	}
	incref(f);
	putentry(e);
	return f;
}

Walkqid*
nubwalk(Fid *f, Fid *newfid, int nname, char **names)
{
	Entry *e;
	Walkqid *wq;

	if(f->open >= 0)
		raise(Emode);
	wq = emallocz(sizeof(*wq)+nname*sizeof(wq->qid[0]), 1);
	wq->clone = nil;
	wq->nqid = 0;
	if(waserror()){
		free(wq);
		raise(nil);
	}
	e = f->entry;
	incref(e);
	if(nname > 0){
		for(int i = 0; i < nname; i++){
			if(waserror()){
				if(i == 0)
					raise(nil);
				goto Partial;	/* walk partially succeeded */
			}
			e = walk1(e, names[i], f->user);
			poperror();
			wq->qid[wq->nqid] = e->qid;
			wq->nqid++;
		}
	}
	if(newfid != nil && newfid != f){
		newfid->entry = e;
		newfid->user = sincref(f->user);
		wq->clone = newfid;
	}else{
		putentry(f->entry);
		f->entry = e;
		wq->clone = f;
	}
Partial:
	poperror();
	return wq;
}

Fid*
nubopen(Fid *f, uint omode)
{
	Entry *e;
	uint perm;

	e = f->entry;
	switch(omode&3){
	case OWRITE:
		perm = DMWRITE;
		break;
	case ORDWR:
		perm = DMREAD|DMWRITE;
		break;
	case OEXEC:
		perm = DMREAD|DMEXEC;
		break;
	case OREAD:
	default:
		perm = DMREAD;
		break;
	}
	if(omode & OTRUNC)
		perm |= DMWRITE;
	if(e->mode & DMDIR && perm & DMWRITE)
		raise(Eperm);
	if(!accessok(e, f->user, perm))
		raise(Eperm);
	if(omode & ORCLOSE && !accessok(e->parent, f->user, DMWRITE))
		raise(Eperm);
	if((e->mode & DMEXCL) != 0 && !nubexcl(e, f))
		raise(Elocked);
	f->open = omode;
	if(omode & OTRUNC && (e->mode & DMAPPEND) == 0 && e->io == nil){
		e->mtime = NOW;
		setstring(&e->muid, f->user);
		if(e->nd != 0 || e->length != 0){
			truncatefile(e);
			LogEntry log = {Trunc, e->qid.path, {.trunc={e->mtime, e->cvers, f->user->s}}};
			nublog(log, nil, 0);
		}
	}
	return f;
}

Fid*
nubcreate(Fid *f, char *name, uint omode, u32int perm)
{
	Entry *dir, *ne;

	dir = f->entry;
	if((dir->qid.type & QTDIR) == 0)
		raise(Enotdir);
	if(!accessok(dir, f->user, DMWRITE))
		raise(Eperm);
	checkfilename(name);
	if(nameexists(dir, name))
		raise(Eexist);
	if(perm & DMDIR){
		if(omode & OTRUNC || (omode&3) != OREAD)
			raise(Eperm);
		perm &= (~0777 | (dir->mode&0777));
	}else
		perm &= (~0666 | (dir->mode&0666));
	ne = mkentry(dir, name, (Qid){nextpath(), 0, perm>>24}, perm, f->user, dir->gid, NOW, 0);
	if(ne == nil)
		raise(nil);
	LogEntry log = {Create, dir->qid.path, {
			.create={ne->qid.path, name, perm, ne->uid->s, ne->gid->s, ne->mtime, ne->cvers}}};
	nublog(log, nil, 0);
	putentry(dir);
	f->entry = nil;
	if((ne->mode & DMEXCL) != 0 && !nubexcl(ne, f))
		raise(Elocked);
	f->entry = ne;
	f->open = omode;
	putpath(ne);
	return f;
}

static void
checkfilename(char *s)
{
	if(*s == 0 || strcmp(s, ".") == 0 || strcmp(s, "..") == 0)
		raise("invalid file name");
	for(; *s; s++)
		if((*s&0xFF) < 0x20)
			raise("invalid character in file name");
}

static int
accessok(Entry *e, String *uid, uint perm)
{
	if(e == nil)
		return 0;
	if(strcmp(uid->s, "none") != 0){
		if(e->uid == uid && ((e->mode>>6)&perm) == perm)
			return 1;
		if(ingroup(uid->s, e->gid->s) && ((e->mode>>3)&perm) == perm)
			return 1;
	}
	if((e->mode&perm) == perm)
		return 1;
	if(nopermcheck)
		return 1;
	return 0;
}

/*
 * for writes, one could choose to use strict logging (no overwrites),
 * overwrite, or a mixture (overwrite until close, then it's immutable).
 * the latter might give good semantics for ordinary files,
 * but not for update-in-place databases.
 */
usize
nubwrite(Fid *f, void *a, usize count, u64int offset)
{
	Entry *e;
	u64int extoffset, cap;
	usize n;
	int i;
	uchar *p;
	int newext;
	Extent ext;

	e = f->entry;
	if(e->qid.type & QTDIR)
		raise(Eperm);	/* should be detected earlier */
	if(e->excl != nil && !nubexcl(e, f))
		raise(Elockbroken);
	if(count == 0)
		return 0;
	if(e->qid.type & QTAPPEND)
		offset = e->length;
	e->mtime = NOW;
	if(e->io != nil)
		return e->io(f, a, count, offset, 1);
	p = a;
	cap = 0;
	extoffset = offset;
	for(i = 0; i < e->nd; i++){
		if(extoffset < e->data[i].length)
			break;
		extoffset -= e->data[i].length;
		cap += e->data[i].length;
	}
	while(count != 0){
		if(i < e->nd){
			/* still space */
			n = count;
			if(extoffset+n > e->data[i].length)
				n = e->data[i].length - extoffset;
			ext = e->data[i];
			newext = 0;
		}else{
			/* allocate new space */
			n = extentsize(disk, extoffset+count, cap, i);
			if(n == 0)
				raise(Efilesize);
			ext = allocdisk(disk, n);
			n = ext.length;
			if(n == 0)
				raise(Efull);
			if(n > count)
				n = count;
			e->data[e->nd++] = ext;
			newext = NewExtent;
			extoffset = 0;
		}
		cap += e->data[i].length;
		e->qid.vers++;
		diskwrite(disk, p, n, e->data[i].base+extoffset);
		LogEntry log = {Write, e->qid.path, {.write={e->mtime, e->muid->s, offset, n, e->qid.vers, e->cvers, extoffset, ext, i | newext}}};
		nublog(log, p, n);
		if((offset += n) > e->length)
			e->length = offset;
		extoffset = 0;
		p += n;
		count -= n;
		i++;
	}
	return p-(uchar*)a;
}

usize
nubread(Fid *f, void *a, usize count, u64int offset)
{
	Entry *e, *x;
	Dir *dir;
	u64int off;
	usize n;
	int i;
	uchar *p;

	if(f->open < 0)
		raise(Eopen);
	if((f->open&3) == OWRITE)
		raise(Eaccess);
	e = f->entry;
	if(e->excl != nil && !nubexcl(e, f))
		raise(Elockbroken);
	e->atime = NOW;
	p = a;
	if(e->qid.type & QTDIR){
		off = 0;
		for(x = e->files; x != nil && count != 0; x = x->dnext){
			dir = e2d(x);
			n = sizeD2M(dir);
			if(off < offset){
				free(dir);
				off += n;
				continue;
			}
			if(count < n){
				free(dir);
				break;
			}
			n = convD2M(dir, p, count);
			free(dir);
			count -= n;
			p += n;
		}
		return p-(uchar*)a;
	}
	if(e->io != nil)
		return e->io(f, a, count, offset, 0);
	if(count == 0 || offset > e->length)
		return 0;
	if(offset+count > e->length)
		count = e->length - offset;
	for(i = 0; i < e->nd; i++){
		if(offset < e->data[i].length)
			break;
		offset -= e->data[i].length;
	}
	for(; i < e->nd && count != 0; i++){
		n = count;
		if(offset + n > e->data[i].length)
			n = e->data[i].length - offset;
		diskread(disk, p, n, e->data[i].base+offset);
		offset = 0;
		count -= n;
		p += n;
	}
	return p-(uchar*)a;
}

void
nubremove(Fid *f)
{
	Entry *e, *p, **l;

	if(waserror()){
		nubclunk(f);
		raise(nil);
	}
	e = f->entry;
	if(e->parent == nil)
		raise(Eperm);
	if(e->qid.type & QTDIR){
		if(e->files != nil)
			raise(Enotempty);
	}
	p = e->parent;
	if((p->qid.type & QTDIR) == 0)
		raise(Ephase);
	for(l = &p->files; *l != nil && *l != e; l = &(*l)->dnext){
		/* skip */
	}
	if(*l == nil)
		raise(Ephase);
	*l = e->dnext;
	putentry(e);	/* directory entry */
	if((e->qid.type & QTDIR) == 0)
		truncatefile(e);
	p->mtime = NOW;
	setstring(&p->muid, f->user);
	LogEntry log = {Remove, e->qid.path, {.remove={p->mtime, p->muid->s}}};
	nublog(log, nil, 0);
	lookpath(e->qid.path, 1);
//print("e %q ref %ld\n", e->name, e->ref);
	poperror();
	nubclunk(f);
}

Dir*
nubstat(Fid *f)
{
	return e2d(f->entry);
}

void
nubsync(Fid *f)
{
	LogEntry log = {Sync, f->entry->qid.path};
	nublog(log, nil, 0);
}

void
nubwstat(Fid *f, Dir *d)
{
	int dosync;
	Entry *e;
	String *uid, *gid;

	if(strcmp(f->user->s, "none") == 0)
		raise(Eperm);
	e = f->entry;
	dosync = 1;
	if(d->name != nil && *d->name != 0 && strcmp(d->name, e->name) != 0){	/* change name (write permission in parent) */
		checkfilename(d->name);
		if(!accessok(e->parent, f->user, DMWRITE) && !wstatallow)
			raise(Eperm);
		if(e->parent != nil && nameexists(e->parent, d->name))
			raise(Eexist);
		dosync = 0;
	}
	if(d->uid != nil && *d->uid != 0 && strcmp(d->uid, e->uid->s) != 0){	/* change user (privileged) */
		if(!wstatallow)
			raise(Eperm);
		uid = name2uid(d->uid);
		if(uid == nil)
			raise("wstat -- unknown uid");
		putstring(uid);
		dosync = 0;
	}
	if(d->gid != nil && *d->gid != 0 && strcmp(d->gid, e->gid->s) != 0){	/* change group (owner or old group leader, or member new group) */
		if(!wstatallow && e->uid != f->user && !leadsgroup(f->user->s, e->gid->s))
			raise(Eperm);
		gid = name2uid(d->gid);
		if(gid == nil)
			raise("wstat -- unknown gid");
		putstring(gid);
		dosync = 0;
	}
	if(d->mode != ~0){	/* change mode (owner or group leader) */
		if(d->mode & ~(DMDIR|DMAPPEND|DMEXCL|DMTMP|0777))
			raise("wstat -- unknown bits in mode/qid.type");
		if((d->mode & DMDIR) != (e->mode & DMDIR))
			raise("wstat -- attempt to change directory");
		if(!wstatallow && e->uid != f->user && !leadseither(f->user, e->gid, d->gid))
			raise(Eperm);
		dosync = 0;
	}
	if(d->mtime != ~0){	/* change time */
		if(!wstatallow && e->uid != f->user && !leadseither(f->user, e->gid, d->gid))
			raise(Eperm);
		dosync = 0;
	}
	if(d->length != ~(u64int)0){
		if((d->mode & DMAPPEND) != 0)
			raise("wstat -- attempt to change length of append-only file");
		if(d->length != 0){
			if(e->qid.type & QTDIR)
				raise("wstat -- attempt to change length of directory");
			raise("wstat -- attempt to change length");	/* TO DO: later */
		}
		dosync = 0;
	}
	if(dosync){	/* sync-wstat */
		nubsync(f);
		return;
	}
	if(d->mode != ~0){
		e->mode = d->mode;
		e->qid.type = d->mode>>24;
	}
	if(d->name != nil && *d->name != 0 && strcmp(d->name, e->name) != 0){
		free(e->name);
		e->name = estrdup(d->name);
	}
	if(d->uid != nil && *d->uid != 0 && strcmp(d->uid, e->uid->s) != 0){
		putstring(e->uid);
		e->uid = name2uid(d->uid);
	}
	if(d->gid != nil && *d->gid != 0 && strcmp(d->gid, e->gid->s) != 0){
		putstring(e->gid);
		e->gid = name2uid(d->gid);
	}
	if(e->io != nil)
		return;
	if(d->length == 0 && e->length != 0){
		truncatefile(e);
		LogEntry log = {Trunc, e->qid.path, {.trunc={e->mtime, e->cvers, f->user->s}}};
		nublog(log, nil, 0);
	}
	if(d->mtime != ~0)
		e->mtime = d->mtime;
	LogEntry log = {Wstat, e->qid.path, {.wstat = {d->mode, d->name, d->uid, d->gid, e->muid->s, e->mtime, e->atime}}};
	nublog(log, nil, 0);
}

static int
leadseither(String *uid, String *egid, char *ngid)
{
	if(leadsgroup(uid->s, egid->s))
		return 1;
	if(ngid != nil && *ngid != 0 && leadsgroup(uid->s, ngid))
		return 1;
	return 0;
}

void
nubclunk(Fid *f)
{
	Entry *e;

	if(f == nil)
		return;
	if(f->open >= 0 && f->open & ORCLOSE){
		f->open &= ~ORCLOSE;
		/* clunk(5) says errors ignored */
		if(!waserror()){
			nubremove(f);
			poperror();
			/* remove also clunks */
			return;
		}
	}
	e = f->entry;
	f->open = -1;
	f->entry = nil;
	if(e->excl != nil)
		nubnoexcl(e, f);
	putentry(e);
}

static String*
nameofuid(String *uid)
{
	String *n;
	char nbuf[128];

	if(*uid->s == 0)
		return sincref(uid);
	n = uid2name(uid->s);
	if(n != nil)
		return n;
	snprint(nbuf, sizeof(nbuf), "(%s)", uid->s);
	return sincref(string(nbuf));
}

static char*
appstr(char** p, char *s)
{
	char *t;

	t = *p;
	strcpy(t, s);
	*p = t+strlen(t)+1;
	return t;
}

static Dir*
e2d(Entry *e)
{
	String *u, *g, *m;
	Dir *d;
	char *p;

	u = nameofuid(e->uid);
	g = nameofuid(e->gid);
	m = nameofuid(e->muid);
	d = emallocz(sizeof(*d)+strlen(u->s)+strlen(g->s)+strlen(m->s)+strlen(e->name)+4, 0);
	p = (char*)d+sizeof(*d);
	d->type = 0;
	d->dev = 0;
	d->name = appstr(&p, e->name);
	d->uid = appstr(&p, u->s);
	putstring(u);
	d->gid = appstr(&p, g->s);
	putstring(g);
	d->muid = appstr(&p, m->s);
	putstring(m);
	d->name = e->name;
	d->mtime = e->mtime;
	d->mode = e->mode;
	d->atime = e->mtime;
	d->qid = e->qid;
	if((e->mode & DMDIR) == 0){
		d->length = e->length;
		d->qid.vers |= e->cvers << 16;
	}else{
		d->length = 0;
	}
	return d;
}

/*
 * locks
 */

static int
nubexcl(Entry *e, Fid *f)
{
	Excl *x;

	x = e->excl;
	if(x == nil){
		x = emallocz(sizeof(*x), 0);
		e->excl = x;
	}else if(x->fid == nil || x->time < NOW){
		/* old lock broken */
		if(x->fid == f){
			nubnoexcl(e, f);
			return 0;
		}
	}else if(x->fid != f)
		return 0;
	/* locked by f */
	x->fid = f;
	x->time = NOW+Tlock;
	return 1;
}

static void
nubnoexcl(Entry *e, Fid *f)
{
	Excl *x;

	x = e->excl;
	if(x != nil && x->fid == f){
		e->excl = nil;
		free(x);
	}
}

/*
 * fids
 */

Fid*
mkfid(u32int fid, String *user)
{
	Fid *f;

	f = emallocz(sizeof(*f), 0);
	f->fid = fid;
	f->open = -1;
	f->entry = nil;
	f->user = sincref(user);
	f->next = nil;
	return f;
}

void
putfid(Fid *f)
{
	if(f == nil)
		return;
	putstring(f->user);
	putentry(f->entry);
	free(f);
}

/*
 * entries
 */

static int
nameexists(Entry *dir, char *name)
{
	Entry *e;

	for(e = dir->files; e != nil; e = e->dnext){
		if(strcmp(e->name, name) == 0)
			return 1;
	}
	return 0;
}

Entry*
mkentry(Entry *parent, char *name, Qid qid, u32int perm, String *uid, String *gid, u32int mtime, u32int cvers)
{
	Entry *e, **lp;

	qid.type = perm>>24;
	e = emallocz(sizeof(*e), 0);
	setmalloctag(e, getcallerpc(&parent));
	e->ref = 1;
	e->qid = qid;
	e->name = estrdup(name);
	e->mode = perm;
	e->uid = sincref(uid);
	e->gid = sincref(gid);
	e->muid = sincref(uid);
	e->mtime = mtime;
	e->atime = e->mtime;
	if((perm & DMDIR) == 0){
		e->cvers = cvers;
		e->length = 0;
		e->nd = 0;
		e->io = nil;
	}else
		e->files = nil;
	e->parent = parent;
	e->dnext = nil;
	e->excl = nil;

	if(parent != nil){
		parent->qid.vers++;
		parent->mtime = e->mtime;		/* TO DO: flush parent version? */

		/* it would be easier to put new files at the front, but that confuses directory reading */
		incref(e);
		for(lp = &parent->files; *lp != nil; lp = &(*lp)->dnext)
			{}
		*lp = e;
	}

	return e;
}

void
putentry(Entry *e)
{
	if(e != nil && decref(e) == 0){
		putstring(e->uid);
		putstring(e->gid);
		putstring(e->muid);
		free(e->name);
		free(e);
	}
}

void
truncatefile(Entry *f)
{
	if(f->ref <= 0)
		abort();
	f->mtime = NOW;
	f->cvers++;
	f->qid.vers++;
	f->length = 0;
	for(int i = 0; i < f->nd; i++)
		freedisk(disk, f->data[i]);
	f->nd = 0;
}

/*
 * log entries
 */
static void
nublog(LogEntry l, void *a, usize n)
{
	l.seq = nextcmdseq();
	if(debug['l'])
		print("%L\n", &l);
	USED(a);		/* TO DO: send data to replicas */
	USED(n);
	logappend(thelog, &l);
}
