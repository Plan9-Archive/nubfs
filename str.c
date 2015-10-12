#include	"dat.h"
#include	"fns.h"

/*
 * strings
 */

static String*	strings[127];

/* hashpjw from aho & ullman */
uint
hashstr(char *s)
{
	uint g, h;

	h = 0;
	for(; *s != 0; s++){
		h = (h<<4) + (*s & 0xFF);
		g = h & 0xF0000000;
		h ^= (g>>24) | g;
	}
	return h;
}

String*
string(char *c)
{
	uint h, n;
	String *s, **hp;

	h = hashstr(c);
	for(hp = &strings[h%nelem(strings)]; (s = *hp) != nil; hp = &s->next){
		if(s->hash == h && strcmp(s->s, c) == 0){
			incref(s);
			return s;
		}
	}
	n = strlen(c);
	s = emallocz(sizeof(*s)+n+1, 0);
	s->ref = 1;
	s->n = n;
	s->hash = h;
	memmove(s->s, c, n+1);
	s->next = nil;
	*hp = s;
	return s;
}

String*
sincref(String *s)
{
	if(s != nil)
		incref(s);
	return s;
}

void
setstring(String **s, String *v)
{
	putstring(*s);
	*s = sincref(v);
}

void
putstring(String *s)
{
	String **hp;

	if(s != nil && decref(s) == 0){
		for(hp = &strings[s->hash%nelem(strings)]; *hp != nil; hp = &(*hp)->next){
			if(*hp == s){
				*hp = s->next;
				break;
			}
		}
	}
}
