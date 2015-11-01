#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <thread.h>

#include "errors.h"

typedef struct Array Array;
typedef struct Disk Disk;
typedef struct Entry Entry;
typedef struct Excl Excl;
typedef struct Extent Extent;
typedef struct Fid Fid;
typedef struct LogEntry LogEntry;
typedef struct LogFile LogFile;
typedef struct Nubfs Nubfs;
typedef struct String String;
typedef struct User User;
typedef struct Walkqid Walkqid;

#pragma incomplete Disk
#pragma incomplete LogFile

typedef u64int	DiskOffset;
typedef u32int	FileOffset;

enum{
	Nextent=	24,

	Tlock=	5*60,	/* seconds */
};

struct Array {
	int	len;
};

struct String {
	Ref;
	uint	hash;
	uint	n;
	String*	next;
	char	s[];
};

struct Nubfs {
	Fid*	fidmap[127];
	String*	strings[127];
	Entry*	pathmap[127];

	Entry*	root;

	int	fd;
	int	logfd;
};

struct Extent {
	DiskOffset	base;	/* offset in partition, in bytes */
	FileOffset	 length;		/* extent length in bytes */
};

struct Excl {
	Fid*	fid;
	u32int	time;
};

struct Entry {
	Ref;
	Entry*	pnext;	/* path list */
	Entry*	dnext;	/* directory list */
	Excl*	excl;	/* exclusive open */

	Qid	qid;
	Entry*	parent;
	char*	name;
	String*	uid;
	String*	gid;
	String*	muid;
	u32int	atime;
	u32int	mtime;
	u32int	mode;
	union{
		struct{
			Entry*	files;
		};	/* Dir */
		struct{
			u32int	cvers;	/* version of last create or trunc */
			FileOffset	length;
			usize	(*io)(Fid*, void*, usize, u64int, int);
			int	nd;
			Extent	data[Nextent];
		};	/* File */
	};
};

struct Fid {
	u32int	fid;
	int	open;
	Entry*	entry;
	String*	user;

	Fid*	next;
};

enum{
	/* block tags */
	Tnone=	0xAC,
	Tboot=	0xDC,
	Tlog=	0x18,	/* sweep set as low order nibble */
	Tlog0=	Tlog,
	Tlog1,
	Tlog2,
	Tlog3,

	NewExtent=	0x80,		/* new extent was allocated to Log Write request */
};

struct Walkqid
{
	Fid*	clone;
	int	nqid;
	Qid	qid[];
};

struct User
{
	String*	uid;
	String*	name;
	String*	leader;
	uint	n;
	String*	mem[];
};

/*
 * log entries
 */
enum{
	Create=	'c',
	Trunc=	't',
	Remove=	'r',
	Write=	'w',
	Wstat=	'W',
	Sync=	'S',
	Mark=	'z',	/* log was closed at this point (unused) */
};

struct LogEntry
{
	u8int op;
	u32int path;
	union{
		struct{
			u32int	newpath;
			char*	name;
			u32int	perm;
			char*	uid;
			char*	gid;
			u32int	mtime;
			u32int	cvers;
			/* TO DO: qid.vers, atime, muid, length? */
		} create;
		struct{
			u32int	mtime;
			u32int	cvers;
			char*	muid;
		} trunc;
		struct{
			u32int	mtime;	/* of parent directory */
			char*	muid;
		} remove;
		struct{
			u32int	mtime;
			char*	muid;
			FileOffset	offset;
			FileOffset	count;
			u32int	vers;
			u32int	cvers;
			u32int	eoff;
			Extent	ext;
			uchar	exind;
		} write;
		struct{	/* Wstat */
			u32int	perm;
			char*	name;
			char*	uid;
			char*	gid;
			char*	muid;
			u32int	mtime;
			u32int	atime;
			/* TO DO: length */
		} wstat;
		/* Sync (no parameters) */
		/* Mark (no parameters) */
	};
	u64int seq;
};

typedef struct Context Context;
struct Context {
	char	errbuf[ERRMAX+1];
	int	nerror;
	jmp_buf	errors[8];
};

Context staticctx;
uchar debug[256];
int	exiting;

#define	waserror()	(staticctx.nerror++, setjmp(staticctx.errors[staticctx.nerror-1]))
#define	poperror()	staticctx.nerror--

#define	NOW	time(nil)
#define	DBG(x)	if(debug[(x)])

#pragma	varargck	type	"L"	LogEntry*
#pragma	varargck	type "E"	Entry*
