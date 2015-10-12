/*
 * nubfs: Users
 */

#include "dat.h"
#include "fns.h"

enum{
	Ulog=	6,
	Usize=	1<<Ulog,
	Umask=	Usize-1,
};

typedef struct Ulist Ulist;
struct Ulist{
	String*	name;	/* User.name or User.uid */
	User*	user;
	Ulist*	next;
};

typedef struct Users Users;
struct Users {
	Ulist*	hash[Usize];
};

/*
 * Each User appears once in each table,
 * and uids and names are 1:1
 */
static struct{
	RWLock;
	int	n;
	Users	byuid;
	Users	byname;
} users;

static usize usersio(Fid*, void*, usize, u64int, int);

void
usersinit(Entry *r, String *user)
{
	Entry *uf;

	uf = mkentry(r, "users", (Qid){2, 0, 0}, 0664, user, user, NOW, 0);
	uf->io = usersio;
}

static Ulist**
lookuser(Users* tab, char *name)
{
	uint h;
	Ulist **l, *p;

	h = hashstr(name)&Umask;
	for(l = &tab->hash[h]; (p = *l) != nil; l = &p->next)
		if(strcmp(p->name->s, name) == 0)
			break;
	return l;
}

void
freeuser(User *u)
{
	int i;

	if(u != nil){
		putstring(u->uid);
		putstring(u->name);
		putstring(u->leader);
		for(i=0; i<u->n; i++)
			putstring(u->mem[i]);
		free(u);
	}
}

static User*
delrefuser(Users *tab, char *name)
{
	Ulist **l, *p;
	User *u;

	l = lookuser(tab, name);
	if((p = *l) == nil)
		return nil;
	u = p->user;
	*l = p->next;
	free(p);
	return u;
}

static void
addrefuser(Users *tab, String *s, User *u)
{
	Ulist **l, *p;

	l = lookuser(tab, s->s);
	if(*l != nil)
		sysfatal("internal: addrefuser");	/* shouldn't happen */
	p = emallocz(sizeof(*p), 0);
	p->name = s;
	p->user = u;
	p->next = nil;
	*l = p;
}

String*
name2uid(char *name)
{
	Ulist **l, *p;
	String *s;

	rlock(&users);
	l = lookuser(&users.byname, name);
	if((p = *l) == nil){
		runlock(&users);
		return nil;
	}
	s = sincref(p->user->uid);
	runlock(&users);
	return s;
}

String*
uid2name(char *uid)
{
	Ulist **l, *p;
	String *s;

	rlock(&users);
	l = lookuser(&users.byuid, uid);
	if((p = *l) == nil){
		runlock(&users);
		return nil;
	}
	s = sincref(p->user->name);
	runlock(&users);
	return s;
}

static void
checkname(char *s)
{
	Rune r;
	static char invalid[] = "#:,()";

	if(*s == 0 || *s == '-' || *s == '+')
		raise("illegal name");
	while((r = *s) != 0){
		if(r < Runeself){
			s++;
			if(r < 0x20 || r >= 0x7F && r < 0xA0 || strchr(invalid, r) != nil)
				r = Runeerror;
		}else
			s += chartorune(&r, s);
		if(r == Runeerror)
			raise("invalid character in name");
	}
}

void
adduser(char *uid, char *name, char *leader, int nm, char **mem)
{
	Ulist **l, *p, *q;
	User *u, *ou;
	int i;

	if(leader != nil){
		if(*leader == '\0')
			leader = nil;
		else if(strcmp(leader, uid) == 0)
			leader = uid;
	}
	checkname(uid);
	checkname(name);
	if(leader != nil && leader != uid)
		checkname(leader);
	for(i = 0; i < nm; i++)
		checkname(mem[i]);
	u = mallocz(sizeof(User)+nm*sizeof(u->mem[0]), 1);
	if(u == nil)
		raise(Enomem);
	u->uid = string(uid);
	u->name = string(name);
	if(leader == nil)
		u->leader = nil;
	else
		u->leader = string(leader);
	u->n = nm;
	for(i = 0; i < nm; i++)
		u->mem[i] = string(mem[i]);
	wlock(&users);
	l = lookuser(&users.byuid, uid);
	if((p = *l) != nil){
		/* replace */
		ou = p->user;
		if((q = *lookuser(&users.byname, name)) != nil && q->user != ou){
			wunlock(&users);
			raise("duplicate uname");
		}
		delrefuser(&users.byname, ou->name->s);
		delrefuser(&users.byuid, uid);
		freeuser(ou);
	}else
		users.n++;
	addrefuser(&users.byuid, u->uid, u);
	addrefuser(&users.byname, u->name, u);
	wunlock(&users);
}

int
deluser(char *name)
{
	User *u;

	wlock(&users);
	u = delrefuser(&users.byname, name);
	if(u == nil){
		wunlock(&users);
		return 0;
	}
	delrefuser(&users.byuid, u->uid->s);
	users.n--;
	wunlock(&users);
	return 1;
}

static int
ismember(char *s, int n, String **mem)
{
	int i;

	for(i = 0; i < n; i++)
		if(strcmp(s, mem[i]->s) == 0)
			return 1;
	return 0;
}

int
ingroup(char *uid, char *gid)
{
	Ulist *p;
	User *g;

	if(uid == gid || strcmp(uid, gid) == 0)
		return 1;
	rlock(&users);
	p = *lookuser(&users.byname, gid);
	if(p != nil){
		g = p->user;
		if(ismember(uid, g->n, g->mem)){
			runlock(&users);
			return 1;
		}
	}
	runlock(&users);
	return 0;
}

int
leadsgroup(char *uid, char *gid)
{
	Ulist *p;
	User *g;

	rlock(&users);
	p = *lookuser(&users.byname, gid);
	if(p != nil){
		g = p->user;
		if(g->leader != nil && strcmp(uid, g->leader->s) == 0 ||
		   g->leader == nil && ismember(uid, g->n, g->mem)){
			runlock(&users);
			return 1;
		}
	}else
		g = nil;
	runlock(&users);
	return g == nil && strcmp(uid, gid) == 0;
}

static int
usercmp(void *a, void *b)
{
	User **ua, **ub;

	ua = a;
	ub = b;
	return strcmp((*ua)->uid->s, (*ub)->uid->s);
}

static char*
usersread(void)
{
	int i, m, o;
	Ulist *p;
	User *u;
	Fmt fmt;
	User **v;

	fmtstrinit(&fmt);
	rlock(&users);
	v = emallocz((users.n+1)*sizeof(*v), 0);
	o = 0;
	for(i = 0; i < nelem(users.byuid.hash); i++){
		for(p = users.byuid.hash[i]; p != nil; p = p->next)
			v[o++] = p->user;
	}
	qsort(v, o, sizeof(*v), usercmp);
	for(i=0; i < users.n; i++){
		u = v[i];
		fmtprint(&fmt, "%q %q", u->uid->s, u->name->s);
		if(u->leader != nil || u->n != 0){
			fmtprint(&fmt, " %q", u->leader != nil? u->leader->s: "");
			for(m = 0; m < u->n; m++)
				fmtprint(&fmt, " %q", u->mem[m]->s);
		}
		fmtprint(&fmt, "\n");
	}
	runlock(&users);
	return fmtstrflush(&fmt);
}

static usize
userswrite(void *buf, usize n)
{
	int i, nf;
	char *p, *s, *e, *flds[100];

	if(n == 0)
		return n;
	if(n > 16*1024)
		raise(Etoobig);
	p = malloc(n+1);
	if(p == nil)
		raise(Enomem);
	if(waserror()){
		free(p);
		raise(nil);
	}
	memmove(p, buf, n);
	p[n] = '\0';
	if(p[n-1] != '\n')
		raise("incomplete line");
	for(s = p; (e = strchr(s, '\n')) != nil; s = e){
		*e++ = '\0';
		if(*s == '#')
			continue;
		nf = tokenize(s, flds, nelem(flds));
		if(nf == nelem(flds))
			raise("too many group members");
		if(strcmp(flds[0], "-") == 0){
			for(i = 1; i < nf; i++)
				deluser(flds[i]);
		}else if(nf > 2)
			adduser(flds[0], flds[1], flds[2], nf-3, flds+3);
		else if(nf > 1)
			adduser(flds[0], flds[1], nil, 0, nil);
		else if(nf != 0)
			adduser(flds[0], flds[0], nil, 0, nil);
	}
	poperror();
	free(p);
	return n;
}

/*
 * uname name [id [leader [member ...]]]	# add
 * uname - name	# remove
 *
 * Note that the uname command reverses order of name and id compared to /adm/users
 */
void
unamecmd(int nf, char **flds)
{
	int i;

	if(strcmp(flds[0], "-") == 0){
		for(i = 1; i < nf; i++)
			deluser(flds[i]);
	}else if(nf > 2)
		adduser(flds[1], flds[0], flds[2], nf-3, flds+3);
	else if(nf > 1)
		adduser(flds[1], flds[0], nil, 0, nil);
	else if(nf != 0)
		adduser(flds[0], flds[0], nil, 0, nil);
}

static usize
usersio(Fid *f, void *a, usize count, u64int offset, int write)
{
	char *s;
	int n;

	USED(f);
	if(write)
		return userswrite(a, count);
	s = usersread();
	n = strlen(s);
	if(offset > n)
		offset = n;
	if(offset+count > n)
		count = n-offset;
	memmove(a, s+offset, count);
	free(s);
	return count;
}
