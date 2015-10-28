void	nubinit(LogFile*, Disk*, char*);
Fid*	nubattach(Fid*, String*, char*);
Fid*	nubopen(Fid*, uint);
Fid*	nubcreate(Fid*, char*, uint, u32int);
Walkqid*	nubwalk(Fid*, Fid*, int, char**);
usize	nubread(Fid*, void*, usize, u64int);
void	nubreplay(void);
usize	nubwrite(Fid*, void*, usize, u64int);
void	nubremove(Fid*);
void	nubstat(Fid*, Dir*);
void	nubsync(Fid*);
void	nubwstat(Fid*, Dir*);
void	nubclunk(Fid*);
void	nubflush(void);
void	nubsweep(void);

Entry*	mkentry(Entry*, char*, Qid, u32int, String*, String*, u32int, u32int);
void	putentry(Entry*);
void	truncatefile(Entry*);

Disk*	diskinit(int, uint, u64int, u32int);
Extent	allocdisk(Disk*, u32int);
Extent	allocdiskat(Disk*, u64int, u32int);
void	diskread(Disk*, uchar*, usize, u64int);
void	diskwrite(Disk*, uchar*, usize, u64int);
void	diskzero(Disk*, u32int, u64int);
void	freedisk(Disk*, Extent);
char*	diskdump(Disk*);
int	eqextent(Extent, Extent);
u32int	extentsize(Disk*, u32int, u32int, uint);
uint	secsize(Disk*);
uint	byte2sec(Disk*, u32int);

LogFile*	logopen(int, u64int);
void	logreplay(LogFile*, int, void (*)(LogEntry*, uint));
void	logsetcopy(LogFile*, int (*)(LogEntry*));
void	logappend(LogFile*, LogEntry*);
void	logcomplete(LogFile*);
void	logflush(LogFile*);
void	logsweep(LogFile*);

uint	logpacksize(LogEntry*);
int	logpack(uchar*, uint, LogEntry*);
int	logunpack(uchar*, uint, LogEntry*);
int	fmtL(Fmt*);

Fid*	mkfid(u32int, String*);
void	putfid(Fid*);

u64int	nextcmdseq(void);

Entry*	lookpath(u32int, int);
void	maxpath(u32int);
u32int	nextpath(void);
void	putpath(Entry*);

void	replayinit(Disk*);
void	replayentry(LogEntry*, uint);
int copyentry(LogEntry*);

void	ctlinit(Entry*, String*);
void	srvexits(char*);

void	error(char*, ...);
void	raise(char*);

#pragma	varargck	argpos	error		1

void*	emallocz(usize, int);
void	putstring(String*);
void	setstring(String**, String*);
String*	string(char*);
String*	sincref(String*);
char*	estrdup(char*);
uint	hashstr(char*);

String*	uid2name(char*);
String*	name2uid(char*);
void	adduser(char*, char*, char*, int, char**);
int	deluser(char*);
void	unamecmd(int, char**);
void	usersinit(Entry*, String*);
