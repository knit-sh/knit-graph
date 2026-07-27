/*
 * sqlbuf.h -- a small growable string buffer used by the transformer to build
 * SQL text incrementally.
 */

#ifndef KNIT_GRAPH_SQLBUF_H
#define KNIT_GRAPH_SQLBUF_H

#include <stddef.h>

typedef struct SqlBuf {
	char *data;   /* NUL-terminated; NULL until the first append */
	size_t len;   /* bytes used, excluding the terminating NUL */
	size_t cap;   /* bytes allocated */
} SqlBuf;

/* Initialise an empty buffer. */
void sqlbuf_init(SqlBuf *b);

/* Append a NUL-terminated string. */
void sqlbuf_append(SqlBuf *b, const char *s);

/* Append a single character. */
void sqlbuf_append_char(SqlBuf *b, char c);

/* Append printf-style formatted text. */
void sqlbuf_appendf(SqlBuf *b, const char *fmt, ...);

/*
 * Hand the accumulated string to the caller (who must free it) and reset the
 * buffer to empty. Returns a malloc'd empty string if nothing was appended.
 */
char *sqlbuf_detach(SqlBuf *b);

/* Release the buffer's storage. */
void sqlbuf_free(SqlBuf *b);

#endif /* KNIT_GRAPH_SQLBUF_H */
