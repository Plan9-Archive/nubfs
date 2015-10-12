/*
 * test extents
 */

#include "dat.h"
#include "fns.h"

static void
usage(void)
{
	fprint(2, "usage: text [-ar] [-s secsize] [-{debug}] addr size\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	Extent e[100];
	u32int size, maxsize, maxalloc;
	Disk *disk;
	u64int lim, base;
	int i, j;
	int rflag, aflag;
	uint bsize;

	rflag = 0;
	aflag = 0;
	bsize = 1024;
	ARGBEGIN{
	case 'r':	rflag = 1; break;
	case 'b':
	case 's':	bsize = strtoul(EARGF(usage()), nil, 0); break;
	case 'a':	aflag = 1; break;
	default:	debug[_argc&0xFF] = 1; break;
	}ARGEND

	if(argc != 2)
		usage();
	srand(getpid());
	quotefmtinstall();
	base = strtoull(argv[0], nil, 0);
	maxsize = strtoul(argv[1], nil, 0);
	disk = diskinit(-1, bsize, base, maxsize);
	if((maxsize >> 24) != 0)
		maxalloc = maxsize >> 8;
	else if((maxsize >> 16) != 0)
		maxalloc = maxsize >> 4;
	else
		maxalloc = maxsize >> 1;
	diskdump(disk);
	for(i=0; i<nelem(e); i++){
		size = nrand(maxalloc);
		e[i] = allocdisk(disk, size);
		lim = e[i].base + e[i].length;
		if(e[i].length != 0)
			print("%ud -> %ud %llud (%#llux) %llud (%#llux)\n", size, e[i].length, e[i].base, e[i].base, lim, lim);
		else
			print("%ud refused\n", size);
	}

	if(aflag){	/* replay allocations to test allocdiskat */
		i = 0;
		if(rflag){
			/* first free a few */
			for(; i<10; i++)
				freedisk(disk, e[i]);

			/* several new allocations */
			do{
				i--;
				e[i] = allocdisk(disk, e[i].length);
			}while(i >= 7);
		}
		diskdump(disk);

		/* restart allocator */
		disk = diskinit(-1, bsize, base, maxsize);
		diskdump(disk);

		/* test allocations */
		for(; i<nelem(e); i++){
			Extent f;
			f = allocdiskat(disk, e[i].base, e[i].length);
			if(f.length == 0){
				print("%d rejected %#llux %ud failed\n", i, e[i].base, e[i].length);
				diskdump(disk);
				exits("error");
			}else if(f.base != e[i].base || f.length != e[i].length){
				print("%d misallocated %#llux %ud -> %#llux %ud\n", i,
					e[i].base, e[i].length, f.base, f.length);
			}
		}
		diskdump(disk);
		exits(nil);
	}

	diskdump(disk);
	if(rflag){
		while(--i >= 0){
			if(e[i].length != 0)
				freedisk(disk, e[i]);
		}
	}else{
		while(i != 0){
			j = nrand(i);
			if(e[j].length != 0)
				freedisk(disk, e[j]);
			e[j] = e[--i];
		}
	}
	diskdump(disk);


	if(0)
	for(i = 0; i < 20; i++){
		u32int b = nrand(64*1024)+128;
		u32int l = nrand(128*1024);
		uint j = nrand(8);
		if(j == 0)
			l = 0;
		print("ext %d %ud %ud %ud\n", j, b, l, extentsize(disk, b, l, j)*secsize(disk));
	}
	exits(nil);
}
