/*
 * log entries
 *
 *	based on "A Flash File System for Inferno", C H Forsyth, 2002
 */

#include	"dat.h"
#include	"fns.h"

static int
logstrsize(char *s)
{
	if(s == nil)
		s = "";
	return strlen(s)+1+BIT8SZ;	/* n[1] s[n], n includes 0 byte */
}

uint
logpacksize(LogEntry *l)
{
	uint n;

	n = 0;
	n += BIT16SZ;	/* size */
	n += BIT8SZ;	/* op */
	n += BIT32SZ;	/* path */
	n += BIT64SZ;	/* gen */
	switch(l->op){
	default:
		return 0;

	case Create:
		n += BIT32SZ;	/* newpath */
		n += BIT32SZ;	/* perm */
		n += BIT32SZ;	/* mtime */
		n += BIT32SZ;	/* cvers */
		n += logstrsize(l->create.name);
		n += logstrsize(l->create.uid);
		n += logstrsize(l->create.gid);
		break;

	case Trunc:
		n += BIT32SZ;	/* mtime */
		n += BIT32SZ;	/* cvers */
		n += logstrsize(l->trunc.muid);
		break;

	case Remove:
		n += BIT32SZ;	/* mtime */
		n += logstrsize(l->remove.muid);
		break;

	case Write:
		n += BIT32SZ;	/* mtime */
		n += BIT32SZ;	/* offset */
		n += BIT32SZ;	/* count */
		n += BIT32SZ;	/* vers */
		n += BIT32SZ;	/* cvers */
		n += BIT32SZ;	/* eoff */
		n += BIT64SZ;	/* ext.base */
		n += BIT32SZ;	/* ext.length */
		n += BIT8SZ;	/* exind */
		n += logstrsize(l->write.muid);
		break;

	case Wstat:
		n += BIT32SZ;	/* perm */
		n += BIT32SZ;	/* mtime */
		n += BIT32SZ;	/* atime */
		n += logstrsize(l->wstat.name);
		n += logstrsize(l->wstat.uid);
		n += logstrsize(l->wstat.gid);
		n += logstrsize(l->wstat.muid);
		break;

	case Sync:
		break;
	}
	return n;
}

static uchar*
logputs(uchar *p, char *s)
{
	uint n;

	if(s == nil)
		s = "";
	n = strlen(s)+1;
	if(n > 255)
		error("logputs string too long");
	PBIT8(p, n);
	p += BIT8SZ;
	memmove(p, s, n);
	return p+n;
}

int
logpack(uchar *s, uint avail, LogEntry *l)
{
	uchar *p;
	uint n;

	n = logpacksize(l);
	if(avail < n)
		return -n;

	p = s;
	PBIT16(p, n);
	p += BIT16SZ;
	PBIT8(p, l->op);
	p += BIT8SZ;
	PBIT32(p, l->path);
	p += BIT32SZ;
	PBIT64(p, l->seq);
	p += BIT64SZ;

	switch(l->op){
	case Create:
		PBIT32(p, l->create.newpath);
		p += BIT32SZ;
		PBIT32(p, l->create.perm);
		p += BIT32SZ;
		PBIT32(p, l->create.mtime);
		p += BIT32SZ;
		PBIT32(p, l->create.cvers);
		p += BIT32SZ;
		p = logputs(p, l->create.name);
		p = logputs(p, l->create.uid);
		p = logputs(p, l->create.gid);
		break;

	case Trunc:
		PBIT32(p, l->trunc.mtime);
		p += BIT32SZ;
		PBIT32(p, l->trunc.cvers);
		p += BIT32SZ;
		p = logputs(p, l->trunc.muid);
		break;

	case Remove:
		PBIT32(p, l->remove.mtime);
		p += BIT32SZ;
		p = logputs(p, l->remove.muid);
		break;

	case Write:
		PBIT32(p, l->write.mtime);
		p += BIT32SZ;
		PBIT32(p, l->write.offset);
		p += BIT32SZ;
		PBIT32(p, l->write.count);
		p += BIT32SZ;
		PBIT32(p, l->write.vers);
		p += BIT32SZ;
		PBIT32(p, l->write.cvers);
		p += BIT32SZ;
		PBIT32(p, l->write.eoff);
		p += BIT32SZ;
		PBIT64(p, l->write.ext.base);
		p += BIT64SZ;
		PBIT32(p, l->write.ext.length);
		p += BIT32SZ;
		PBIT8(p, l->write.exind);
		p += BIT8SZ;
		p = logputs(p, l->write.muid);
		break;

	case Wstat:
		PBIT32(p, l->wstat.perm);
		p += BIT32SZ;
		PBIT32(p, l->wstat.mtime);
		p += BIT32SZ;
		PBIT32(p, l->wstat.atime);
		p += BIT32SZ;
		p = logputs(p, l->wstat.name);
		p = logputs(p, l->wstat.uid);
		p = logputs(p, l->wstat.gid);
		p = logputs(p, l->wstat.muid);
		break;

	case Sync:
	case Mark:
		break;
	}

	if(p != s+n)
		error("logpack size mismatch");
	return n;
} 

static uchar*
loggets(uchar *p, uchar *ep, char **s)
{
	uint n;

	if(p == nil)
		return nil;
	if(p+BIT8SZ > ep)
		return nil;
	n = GBIT8(p);	/* includes zero byte */
	p += BIT8SZ;
	if(p+n > ep || n == 0 || p[n-1] != 0)
		return nil;
	*s = (char*)p;	/* assumes caller will process record before reusing buffer */
	return p+n;
}

int
logunpack(uchar *ap, uint nap, LogEntry *l)
{
	uchar *p, *ep;
	uint size;

	p = ap;
	ep = p+nap;

	if(p+BIT16SZ+BIT8SZ+BIT32SZ+BIT64SZ > ep)
		return 0;
	size = GBIT16(p);
	p += BIT16SZ;

	if(size < BIT16SZ+BIT8SZ+BIT32SZ+BIT64SZ)
		return 0;

	l->op = GBIT8(p);
	p += BIT8SZ;
	l->path = GBIT32(p);
	p += BIT32SZ;
	l->seq = GBIT64(p);
	p += BIT64SZ;

	switch(l->op){
	case Create:
		if(p+4*BIT32SZ > ep)
			return 0;
		l->create.newpath = GBIT32(p);
		p += BIT32SZ;
		l->create.perm = GBIT32(p);
		p += BIT32SZ;
		l->create.mtime = GBIT32(p);
		p += BIT32SZ;
		l->create.cvers = GBIT32(p);
		p += BIT32SZ;
		p = loggets(p, ep, &l->create.name);
		p = loggets(p, ep, &l->create.uid);
		p = loggets(p, ep, &l->create.gid);
		break;

	case Trunc:
		if(p+2*BIT32SZ > ep)
			return 0;
		l->trunc.mtime = GBIT32(p);
		p += BIT32SZ;
		l->trunc.cvers = GBIT32(p);
		p += BIT32SZ;
		p = loggets(p, ep, &l->trunc.muid);
		break;

	case Remove:
		if(p+BIT32SZ > ep)
			return 0;
		l->remove.mtime = GBIT32(p);
		p += BIT32SZ;
		p = loggets(p, ep, &l->remove.muid);
		break;

	case Write:
		if(p+8*BIT32SZ+BIT8SZ > ep)
			return 0;
		l->write.mtime = GBIT32(p);
		p += BIT32SZ;
		l->write.offset = GBIT32(p);
		p += BIT32SZ;
		l->write.count = GBIT32(p);
		p += BIT32SZ;
		l->write.vers = GBIT32(p);
		p += BIT32SZ;
		l->write.cvers = GBIT32(p);
		p += BIT32SZ;
		l->write.eoff = GBIT32(p);
		p += BIT32SZ;
		l->write.ext.base = GBIT32(p);
		p += BIT64SZ;
		l->write.ext.length = GBIT32(p);
		p += BIT32SZ;
		l->write.exind = GBIT8(p);
		p += BIT8SZ;
		p = loggets(p, ep, &l->write.muid);
		break;

	case Wstat:
		if(p+3*BIT32SZ > ep)
			return 0;
		l->wstat.perm = GBIT32(p);
		p += BIT32SZ;
		l->wstat.mtime = GBIT32(p);
		p += BIT32SZ;
		l->wstat.atime = GBIT32(p);
		p += BIT32SZ;
		p = loggets(p, ep, &l->wstat.name);
		p = loggets(p, ep, &l->wstat.uid);
		p = loggets(p, ep, &l->wstat.gid);
		p = loggets(p, ep, &l->wstat.muid);
		break;

	case Sync:
	case Mark:
		break;
	}

	if(p == nil || p > ep)
		return 0;

	return p-ap;
} 

int
fmtL(Fmt *f)
{
	LogEntry *l;
	int n;

	l = va_arg(f->args, LogEntry*);
	n = fmtprint(f, "%llud ", l->seq);
	switch(l->op){
	case Create:
		return n+fmtprint(f, "Create path %#ux newpath %#ux name %#q perm %#uo uid %#q gid %#q mtime %ud cvers %ud",
			l->path, l->create.newpath, l->create.name, l->create.perm, l->create.uid, l->create.gid, l->create.mtime, l->create.cvers);
	case Trunc:
		return n+fmtprint(f, "Trunc path %#ux mtime %ud cvers %ud muid %#q",
			l->path, l->trunc.mtime, l->trunc.cvers, l->trunc.muid);
	case Remove:
		return n+fmtprint(f, "Remove path %#ux mtime %ud muid %#q", l->path, l->remove.mtime, l->remove.muid);
	case Write:
		return n+fmtprint(f, "Write path %#ux mtime %ud muid %#q offset %ud count %ud vers %ud cvers %ud eoff %ud ext %#llux %#ux exind %#ux",
			l->path, l->write.mtime, l->write.muid, l->write.offset, l->write.count, l->write.vers, l->write.cvers, l->write.eoff,
			l->write.ext.base, l->write.ext.length, l->write.exind);
	case Wstat:
		return n+fmtprint(f, "Wstat path %#ux perm %#uo name %#q uid %#q gid %#q muid %#q mtime %ud atime %ud",
			l->path, l->wstat.perm, l->wstat.name, l->wstat.uid, l->wstat.gid, l->wstat.muid, l->wstat.mtime, l->wstat.atime);
	case Mark:
		return n+fmtprint(f, "Mark");
	case Sync:
		return n+fmtprint(f, "Sync");
	default:
		return n+fmtprint(f, "bad log entry %d", l->op);
	}
}
