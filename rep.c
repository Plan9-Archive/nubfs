/*
 * nubfs, part 5: Action Replay
 *
 *	based on "A Flash File System for Inferno", C H Forsyth, 2002
 *
 */

#include	"dat.h"
#include	"fns.h"

static Disk*	disk;
static u64int	cmdseq;

static int recreate(LogEntry*);
static int retrunc(LogEntry*);
static int reremove(LogEntry*);
static int rewrite(LogEntry*);
static int rewstat(LogEntry*);

void
replayinit(Disk *adisk)
{
	disk = adisk;
}

u64int
nextcmdseq(void)
{
	return ++cmdseq;
}

static void
badreplay(LogEntry *l)
{
	error("can't replay: %L", l);
}

static Entry*
lookfile(u32int p, char *why)
{
	Entry *e;

	e = lookpath(p, 0);
	if(e != nil){
		if(e->mode & DMDIR){
			error("replay: illegal %s of directory #%#8.8ux [%s]", why, p, e->name);
			return nil;
		}
		return e;
	}
	return nil;
}

/*
 * Log blocks are partitioned into swept and active pages, where
 * some commands in active might already have been swept, and
 * run by a previous replay. The sequence number allows duplicates
 * to be skipped when active is replayed.
 */
void
replayentry(LogEntry *le)
{
	if(le->seq <= cmdseq){
		if(debug['l'])
			fprint(2, "replay: dup/obsolete: %L\n", le);
		return;
	}
	cmdseq = le->seq;

	if(debug['l'])
		fprint(2, "replay: %L\n", le);

	switch(le->op){
	case Create:
		maxpath(le->path);
		maxpath(le->create.newpath);
		if(!recreate(le))
			badreplay(le);
		break;
	case Trunc:
		if(le->path == 1)
			break;
		maxpath(le->path);
		if(!retrunc(le))
			badreplay(le);
		break;
	case Remove:
		maxpath(le->path);
		if(!reremove(le))
			badreplay(le);
		break;
	case Write:
		maxpath(le->path);
		if(!rewrite(le))
			badreplay(le);
		break;
	case Wstat:
		maxpath(le->path);
		if(!rewstat(le))
			badreplay(le);
		break;
	default:
		error("unexpected log entry: op %#x", le->op);
		break;
	}
}

static int
recreate(LogEntry *le)
{
	Entry *ne, *parent;
	Qid q;

	parent = lookpath(le->path, 0);
	if(parent == nil)
		return 0;
	if(parent->mode & DMDIR){
		q = (Qid){le->create.newpath, 0, QTFILE};
		ne = mkentry(parent, le->create.name, q, le->create.perm, string(le->create.uid), string(le->create.gid), le->create.mtime, le->create.cvers);	/* muid? */
		if(ne != nil){
			if(ne->ref > 1)
				decref(ne);	/* drop reference from mkentry, since no Fid as yet */
			//ne->atime = le->create.atime;
			putpath(ne);
		}else
			error("replay: can't mkentry %s", le->create.name);
	}else{
		error("replay: create in non-directory: %#8.8ux [%s] of %#8.8ux [%s]",
			le->path, parent->name, le->create.newpath, le->create.name);
	}
	return 1;
}

static int
retrunc(LogEntry *le)
{
	Entry *f;

	f = lookfile(le->path, "trunc");
	if(f == nil)
		return 0;
	f->mtime = le->trunc.mtime;
	putstring(f->muid);
	f->muid = string(le->trunc.muid);
	f->cvers = le->trunc.cvers;
	truncatefile(f);
	return 1;
}

static void
badext(Entry *f, int i, char *why)
{
	error("inconsistency in extents: file %q path %#llux extent %#ux: %s", f->name, f->qid.path, i, why);
}

static int
rewrite(LogEntry *le)
{
	Entry *f;
	u64int offset, length;
	u32int count;
	Extent ext;
	int i;

	f = lookfile(le->path, "write");
	if(f == nil)
		return 0;
	offset = le->write.offset;
	count = le->write.count;
	ext = le->write.ext;
	if(debug['w'])
		fprint(2, "w %8.8ux %lld[%d] v%d -> %llux %llux\n", le->path, offset, count, le->write.cvers, ext.base, ext.base+ext.length);
	if(f->cvers != le->write.cvers)
		return 0;
	i = le->write.exind & ~NewExtent;
	if(le->write.exind & NewExtent){
		if(i != f->nd)
			badext(f, i, "index");
		f->data[i] = ext;
		f->nd++;
		ext = allocdiskat(disk, ext.base, ext.length);
		if(ext.length == 0)
			badext(f, le->write.exind, "replay allocation");
		/* following test will ensure that replay's allocation is in step with original */
	}else{
		if(i >= f->nd)
			badext(f, i, "index");
	}
	if(f->data[i].base != ext.base ||
	   f->data[i].length != ext.length)
		badext(f, le->write.exind, "value");
	length = offset+count;
	if(length > f->length)
		f->length = length;
	f->mtime = le->write.mtime;
	putstring(f->muid);
	f->muid = string(le->write.muid);
	f->qid.vers++;
	return 1;
}

static int
reremove(LogEntry *le)
{
	Entry *e, *p, **l;

	e = lookpath(le->path, 1);
	if(e == nil)
		return 0;
	if((p = e->parent) == nil)
		error("replay: parentless entry %8.8ux [%s]", le->path, e->name);
	if(p->mode & DMDIR){
		truncatefile(e);
		for(l = &p->files; *l != nil && *l != e; l = &(*l)->dnext){
			/* skip */
		}
		if(*l != nil){
			*l = e->dnext;
			if(e->ref != 1)
				error("replay: entry %#8.8ux [%q] still in use (%ld)", le->path, e->name, e->ref);
			putentry(e);
		}else
			error("replay: lost entry %8.8ux [%s] in %8.8llux [%s]", le->path, e->name, p->qid.path, p->name);
		p->mtime = le->remove.mtime;
		putstring(p->muid);
		p->muid = string(le->remove.muid);
	}else
		error("replay: remove entry %8.8ux [%s] from file %8.8llux [%s]", le->path, e->name, p->qid.path, p->name);
	return 1;
}

static int
rewstat(LogEntry *le)
{
	Entry *f;

	f = lookpath(le->path, 0);
	if(f == nil)
		return 0;
	if(le->wstat.perm != ~0)
		f->mode = (f->mode & DMDIR) | (le->wstat.perm &~ DMDIR);
	if(*le->wstat.uid != 0){
		putstring(f->uid);
		f->uid = string(le->wstat.uid);
	}
	if(*le->wstat.gid != 0){
		putstring(f->gid);
		f->gid = string(le->wstat.gid);
	}
	if(*le->wstat.muid != 0){
		putstring(f->muid);
		f->muid = string(le->wstat.muid);
	}
	if(le->wstat.mtime != ~0)
		f->mtime = le->wstat.mtime;
	if(le->wstat.atime != ~0)
		f->atime = le->wstat.atime;
	return 1;
}

/*
 * copy log entry, discarding if redundant
 */

static void
copyerror(char *why, LogEntry *le)
{
	error("copylog error: %s: %L", why, le);
}

int
copyentry(LogEntry *le)
{
	Entry *f, *parent;
	int i, keep, repack;

	if(debug['l'])
		fprint(2, "copylog: %L ...", le);

	keep = 0;
	repack = 0;	/* keep == 0 => repack == 0 */

	switch(le->op){
	case Create:
		f = lookpath(le->create.newpath, 0);
		if(f == nil)
			break;
		parent = lookpath(le->path, 0);
		if(parent == nil)
			copyerror("missing parent", le);
		if((le->create.perm & DMDIR) != (f->mode & DMDIR))
			copyerror("dir/file mismatch", le);
		if((f->mode & DMDIR) == 0){
			if(f->cvers != le->create.cvers){
				/*
				 * path exists, so file is current, but cvers differs,
				 * meaning same file will later be truncated.
				 * update the cvers in the current log,
				 * so as to retain only the matching set of Write requests
				 */
				le->create.cvers = f->cvers;
				repack = 1;
			}
		}
		/* update log entry with current name etc., allowing subsequent Wstats to be removed */
		if(strcmp(le->create.name, f->name) != 0){
			le->create.name = f->name;
			repack = 1;
		}
		if(le->create.perm != f->mode){
			le->create.perm = f->mode;
			repack = 1;
		}
		if(le->create.mtime != f->mtime){
			le->create.mtime = f->mtime;
			repack = 1;
		}
		//if(le->create.atime != f->atime){
		//	le->create.atime = f->atime;
		//	repack = 1;
		//}
		if(strcmp(le->create.uid, f->uid->s) != 0){
			le->create.uid = f->uid->s;
			repack = 1;
		}
		if(strcmp(le->create.gid, f->gid->s) != 0){
			le->create.gid = f->gid->s;
			repack = 1;
		}
		//if(strcmp(le->create.muid, f->muid->s) != 0){
		//	le->create.muid = f->muid->s;
		//	repack = 1;
		//}
		//if(le->create.length != f->length){
		//	le->create.length = f->length;
		//	repack = 1;
		//}
		keep = 1;
		break;
	case Trunc:
		/* always obsolete */
		break;
	case Remove:
		/* always obsolete, but check that it has gone */
		f = lookpath(le->path, 0);
		if(f != nil)
			copyerror("Remove: path exists", le);
		break;
	case Write:
		f = lookpath(le->path, 0);
		if(f == nil)
			break;
		if(f->cvers != le->write.cvers)
			break;	/* completely obsolete */
		i = le->write.exind & ~0x80;
		if(i >= f->nd)
			copyerror("extent index too high", le);	/* can't happen: truncate would change cvers */
		if(!eqextent(f->data[i], le->write.ext))
			break;	/* obsolete extent (all data overwritten) */
		keep = 1;
		break;
	case Wstat:
		/* always obsolete: Create has been updated from in-memory Entry */
		break;
	default:
		copyerror("unexpected entry type", le);
		break;
	}

	if(debug['l']){
		if(keep){
			if(repack)
				fprint(2, " ->\n\t%L\n", le);
			else
				fprint(2, " [kept]\n");
		}else
			fprint(2, " [discard]\n");
	}

	return keep+repack;
}
