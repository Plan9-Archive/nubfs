/*
 * paths
 *
 *	based on "A Flash File System for Inferno", C H Forsyth, 2002
 *
 */

#include	"dat.h"
#include	"fns.h"

static Entry*	paths[127];
static u32int	pathgen;

u32int
nextpath(void)
{
	return ++pathgen;
}

void
maxpath(u32int p)
{
	if(p > pathgen)
		pathgen = p;
}

void
putpath(Entry *e)
{
	Entry **hp;

	hp = &paths[e->qid.path%nelem(paths)];
	e->pnext = *hp;
	*hp = e;
}

Entry*
lookpath(u32int path, int del)
{
	Entry *e, **hp;

	hp = &paths[path%nelem(paths)];
	for(; (e = *hp) != nil; hp = &e->pnext){
		if(e->qid.path == path){
			if(del)
				*hp = e->pnext;
			return e;
		}
	}
	return nil;
}
