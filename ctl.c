/*
 * nubfs: Control File System
 */

#include "dat.h"
#include "fns.h"

static usize ctlio(Fid*, void*, usize, u64int, int);

void
ctlinit(Entry *r, String *user)
{
	Entry *cf;

	cf = mkentry(r, "ctl", (Qid){1, 0, 0}, 0664, user, user, NOW, 0);
	cf->io = ctlio;
}

static usize
ctlwrite(Fid *f, void *a, usize count)
{
	char req[512], *flds[10];
	int n;

	USED(f);
	if(count > sizeof(req)-1)
		raise(Elength);
	memmove(req, a, count);
	req[count] = 0;
	n = tokenize(req, flds, nelem(flds));
	if(n < 1)
		raise(Ebadctl);
	if(strcmp(flds[0], "halt") == 0)
		exiting = 1;
	else if(strcmp(flds[0], "sync") == 0)
		nubflush();
	else if(strcmp(flds[0], "uname") == 0)
		unamecmd(n-1, flds+1);
	else if(strcmp(flds[0], "sweep") == 0)
		nubsweep();
	else if(strcmp(flds[0], "allow") == 0)
		wstatallow = 1;
	else if(strcmp(flds[0], "disallow") == 0)
		wstatallow = 0;
	else if(strcmp(flds[0], "permit") == 0)
		nopermcheck = 0;
	else if(strcmp(flds[0], "nopermit") == 0)
		nopermcheck = 1;
	else
		raise(Ebadctl);
	return count;
}

static usize
ctlio(Fid *f, void *a, usize count, u64int offset, int write)
{
	USED(offset);
	if(write)
		return ctlwrite(f, a, count);
	return 0;
}
