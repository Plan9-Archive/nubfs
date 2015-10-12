</$objtype/mkfile
BIN=$home/bin/$objtype

OFILES=\
	nub.$O\
	errstr.$O\
	ext.$O\
	etc.$O\
	ent.$O\
	log.$O\
	path.$O\
	rep.$O\
	str.$O\
	9p.$O\
	ctl.$O\
	uid.$O\

HFILES=\
	dat.h\
	fns.h\

TARG=nubfs

</sys/src/cmd/mkone
CFLAGS= -wFVT

errstr.c:	errors.h
	./mkerrstr >errstr.c

$O.tnub:	tnub.$O nub.$O errstr.$O
	$LD -o $target $prereq

$O.text:	text.$O ext.$O errstr.$O etc.$O
	$LD -o $target $prereq
