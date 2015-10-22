/*
 * nubfs, part 3: Extents
 *
 *	based on "Disk file allocation based on the buddy system", Philip D L Koch,
 *		ACM Transactions on Computer Systems, Vol. 5, No. 4, November 1987, pp 352-370.
 *		(Koch describes the algorithms and performance of the DTSS disk file subsystem.)
 */

#include	"dat.h"
#include	"fns.h"

enum{
	Nslice=	32,
};

typedef struct Slice Slice;
struct Slice {
	u64int	addr;	/* relative to Disk.base */
	Slice*	next;
};

typedef struct Disk Disk;
struct Disk {
	int	fd;
	u64int	base;
	uint	secsize;
	uint	secshift;
	Slice*	slices[Nslice];
	Slice*	freeslices;
};

/*
 * log2
 */

static uchar log2v[256];

static void
loginit(void)
{
	int i;

	for(i=2; i<nelem(log2v); i++)
		log2v[i] = log2v[i/2] + 1;
}

static int
log2of(uint n)
{
	int r;

	r = (n & (n-1)) != 0;	/* not a power of two => round up */
	if((n>>8) == 0)
		return log2v[n] + r;
	if((n>>16) == 0)
		return 8 + log2v[n>>8] + r;
	if((n>>24) == 0)
		return 16 + log2v[n>>16] + r;
	return 24 + log2v[n>>24] + r;
}

static void freeslice(Disk*, u64int, u32int);
static void freeslices(Disk*, u64int, u32int);

Disk*
diskinit(int fd, uint secsize, u64int base, u32int length)
{
	Disk *disk;

	loginit();
	disk = emallocz(sizeof(*disk), 1);
	disk->fd = fd;
	disk->base = base;
	disk->secsize = secsize;
	disk->secshift = log2of(secsize);
	freeslices(disk, 0, length>>disk->secshift);
	return disk;
}

/*
 * return the smallest block of at least size bytes,
 * splitting a much larger available block into smaller ones, if required.
 */
Extent
allocdisk(Disk *disk, u32int size)
{
	Slice *s;
	u64int addr;
	uint n0, n;

	size = (size + disk->secsize-1) >> disk->secshift;
	n0 = log2of(size);
	for(n = n0; n < Nslice; n++){
		size = (u32int)1<<n;
		DBG('d')print("%ud %d?\n", size, n);
		if((s = disk->slices[n]) != nil){
			disk->slices[n] = s->next;
			addr = s->addr;
			s->next = disk->freeslices;
			disk->freeslices = s;
			for(; n > n0; n--){
				size >>= 1;
				freeslice(disk, addr+size, size);
			}
			return (Extent){addr<<disk->secshift, size<<disk->secshift};
		}
	}
	return (Extent){0, 0};
}

Extent
allocdiskat(Disk *disk, u64int reqaddr, u32int size)
{
	Slice *s, **l;
	u64int addr, avail;
	uint n0, n;

	size = (size + disk->secsize-1) >> disk->secshift;
	reqaddr >>= disk->secshift;
	n0 = log2of(size);
	for(n = n0; n < Nslice; n++){
		size = (u32int)1<<n;
		DBG('d')print("at: %ud %d?\n", size, n);
		for(l = &disk->slices[n]; (s = *l) != nil; l = &s->next)
			if(s->addr <= reqaddr && reqaddr < s->addr+size)
				break;
		if(*l != nil){
			*l = s->next;
			addr = s->addr;
			s->next = disk->freeslices;
			disk->freeslices = s;
			if(addr != reqaddr)
				freeslices(disk, addr, reqaddr-addr);
			if(n != n0){
				avail = reqaddr+(1<<n0);
				freeslices(disk, avail, addr+size-avail);
			}
			size = 1<<n0;
			return (Extent){reqaddr<<disk->secshift, size<<disk->secshift};
		}
	}
	return (Extent){0, 0};
}

void
freedisk(Disk *disk, Extent ext)
{
	freeslice(disk, ext.base>>disk->secshift, ext.length>>disk->secshift);
}

static void
freeslices(Disk *disk, u64int addr, u32int size)
{
	uint i;
	u32int m;

	DBG('d')print("freeslices %#llux %ud\n", addr, size);

	/* align the address */
	for(i=0; i<32; i++){
		m = (u32int)1<<i;
		if(size < m)
			break;
		if(addr & m){
			freeslice(disk, addr, m);
			size -= m;
			addr += m;
		}
	}
	if(size == 0)
		return;

	/* split the size */
	m = (u32int)1<<log2of(size);
	for(; size != 0; m >>= 1){
		if((size & m) != 0){
			freeslice(disk, addr, m);
			addr += m;
			size &= ~m;
		}
	}
}

static void
freeslice(Disk *disk, u64int addr, u32int size)
{
	Slice **l, *s;
	uint n;

	DBG('d')print("freeslice %llud %ud\n", addr, size);
	if(size == 0 || (addr & (size-1)) != 0)
		error("invalid slice free: %llud %ud", addr, size);
	n = log2of(size);
	size = (u32int)1<<n;
	l = &disk->slices[n];
	while(n < Nslice && (s = *l) != nil){
		DBG('d')print("merge? %#llux %#llux %#ux\n", addr, s->addr, size);
		if((s->addr^addr) == size){	/* buddy? */
			DBG('d')print("merge %#llux %#llux %#ux\n", addr, s->addr, size);
			if(s->addr < addr)
				addr = s->addr;
			*l = s->next;
			s->next = disk->freeslices;
			disk->freeslices = s;
			n++;
			size <<= 1;
			l = &disk->slices[n];
		}else{
			if(addr < s->addr)
				break;
			l = &s->next;
		}
	}
	DBG('d')print("free %llud %ud %d\n", addr, size, n);
	s = disk->freeslices;
	if(s != nil){
		disk->freeslices = s->next;
		s->next = nil;
	}else
		s = emallocz(sizeof(*s), 1);
	s->addr = addr;
	s->next = *l;
	*l = s;
}

void
diskdump(Disk *disk)
{
	Slice *s;
	int i;

	print("disk %#p slices %d\n", disk, Nslice);
	for(i = 0; i < Nslice; i++){
		if(disk->slices[i] != nil){
			print("\t%2d:", i);
			for(s = disk->slices[i]; s != nil; s = s->next)
				print(" %llud", s->addr);
			/* check for missed buddies (shouldn't happen) */
			for(s = disk->slices[i]; s->next != nil; s = s->next)
				if(s->addr == (s->next->addr ^ (1<<i)))
					print(" [missed %llud %llud]", s->addr, s->next->addr);
			print(" [%ud]\n", (u32int)1<<i);
		}
	}
}

int
eqextent(Extent a, Extent b)
{
	return a.base == b.base && a.length == b.length;
}

/*
 * b: size of write in bytes
 * l: current file length in bytes
 * i: number of extents allocated
 * Returns number of bytes to request
 */
u32int
extentsize(Disk *disk, u32int b, u32int l, uint i)
{
	u32int p;

	if(i >= Nextent)
		return 0;
	b = (b + disk->secsize - 1) >> disk->secshift;
	l = (l + disk->secsize - 1) >> disk->secshift;
	p = (u32int)1 << i;
	DBG('d')print("b=%d l=%d p=%d\n", b, l, p);
	if(l < p)
		p = l;
	if(b > p)
		p = b;
	return 1<<(log2of(p)+disk->secshift);	/* max(b, min(l, 2**i)) */
}

uint
secsize(Disk *d)
{
	return d->secsize;
}

uint
byte2sec(Disk *d, u32int bytes)
{
	return (bytes + d->secsize - 1)/d->secsize;
}

void
diskread(Disk *disk, uchar *p, usize n, u64int offset)
{
	if(pread(disk->fd, p, n, disk->base+offset) != n)
		raise(nil);
}

void
diskwrite(Disk *disk, uchar *p, usize n, u64int offset)
{
	if(pwrite(disk->fd, p, n, disk->base+offset) != n)
		raise(nil);
}

void
diskzero(Disk *disk, u32int count, u64int offset)
{
	usize n, nz;
	uchar *p;

	nz = 32*1024;
	if(nz > count)
		nz = count;
	p = emallocz(nz, 1);
	if(waserror()){
		free(p);
		raise(nil);
	}
	while((n = count) != 0){
		if(n > nz)
			n = nz;
		diskwrite(disk, p, n, offset);
		count -= n;
		offset += n;
	}
	poperror();
	free(p);
}
