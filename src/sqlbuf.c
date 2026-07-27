/*
 * sqlbuf.c -- growable string buffer. Allocation failures abort: there is
 * nothing useful to do on OOM while assembling a query, and every caller would
 * otherwise have to thread an error path through pure string building.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "sqlbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void oom(void)
{
	fprintf(stderr, "knit-graph: out of memory\n");
	exit(1);
}

/* Ensure room for at least `extra` more bytes plus a terminating NUL. */
static void reserve(SqlBuf *b, size_t extra)
{
	size_t need = b->len + extra + 1;
	if (need <= b->cap)
		return;

	size_t cap = b->cap ? b->cap : 64;
	while (cap < need)
		cap *= 2;

	char *grown = realloc(b->data, cap);
	if (!grown)
		oom();
	b->data = grown;
	b->cap = cap;
}

void sqlbuf_init(SqlBuf *b)
{
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

void sqlbuf_append(SqlBuf *b, const char *s)
{
	size_t n = strlen(s);
	reserve(b, n);
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

void sqlbuf_append_char(SqlBuf *b, char c)
{
	reserve(b, 1);
	b->data[b->len++] = c;
	b->data[b->len] = '\0';
}

void sqlbuf_appendf(SqlBuf *b, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		oom();

	reserve(b, (size_t)n);
	va_start(ap, fmt);
	vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

char *sqlbuf_detach(SqlBuf *b)
{
	char *out = b->data ? b->data : strdup("");
	if (!out)
		oom();
	sqlbuf_init(b);
	return out;
}

void sqlbuf_free(SqlBuf *b)
{
	free(b->data);
	sqlbuf_init(b);
}
