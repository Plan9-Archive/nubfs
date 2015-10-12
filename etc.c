/*
 * nubfs: miscellaneous functions
 */

#include	"dat.h"
#include	"fns.h"

Context	staticctx;

void
error(char *s, ...)
{
	va_list ap;
	char b[256];

	va_start(ap, s);
	vsnprint(b, sizeof(b), s, ap);
	va_end(ap);
	fprint(2, "%s\n", b);
	exits("error");
}

void
raise(char *err)
{
	Context *ctx;

	ctx = &staticctx;	/* TO DO: privates */
	if(err != nil)
		werrstr("%s", err);
	if(ctx->nerror <= 0 || ctx->nerror > nelem(ctx->errors))
		error("error stack overflow/underflow");
	ctx->nerror--;
	longjmp(ctx->errors[ctx->nerror], 1);
}

void*
emallocz(usize n, int zero)
{
	void *p;

	p = mallocz(n, zero);
	if(p == nil)
		error("out of memory");
	return p;
}

char*
estrdup(char *s)
{
	char *t;

	t = strdup(s);
	if(t == nil)
		error("out of memory");
	return t;
}
