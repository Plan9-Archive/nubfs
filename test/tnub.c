/*
 * test nub
 */

#include "dat.h"
#include "fns.h"

void	xstat(Fid*);

void
main(int argc, char **argv)
{
	Fid *root, *d, *f;
	char *names[10];
	String *user;
	Walkqid *wq;

	ARGBEGIN{
	}ARGEND

	quotefmtinstall();
	nubinit("user");
	if(waserror()){
		print("error: %r\n");
		exits(nil);
	}
	user = string("user");
	root = mkfid(1, user);
	nubattach(root, user, "");
print("root=%#p %#p qid %#llux %#ux\n", root, root->entry, root->entry->qid.path, root->entry->qid.type);
	d = mkfid(2, user);
	wq = nubwalk(root, d, 0, nil);
	if(wq->clone == nil)
		error("can't clone root");
print("clone=%#p %#p\n", wq->clone, wq->clone->entry);
	d = wq->clone;
	f = nubcreate(d, "f1", OWRITE, 0666);
	xstat(f);
	nubwrite(f, "hello, world", 12, 0);
	xstat(f);
	nubwrite(f, "goodbye\n", 8, 12);
	xstat(f);
	if(f->entry->parent == nil)
		print("nil parent\n");
	else
		print("parent mode %uo\n", f->entry->parent->mode);
	nubremove(f);
	exits(nil);
}

void
xstat(Fid *f)
{
	Dir dir;

	nubstat(f, &dir);
	print("stat: name %#q mode %luo length %llud uid %#q gid %#q muid %#q\n",
		dir.name, dir.mode, dir.length, dir.uid, dir.gid, dir.muid);
}
