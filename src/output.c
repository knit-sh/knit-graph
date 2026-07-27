/*
 * output.c -- sqlite3 default "-list" formatting (see output.h).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "output.h"

/* One '|'-separated line; a NULL field contributes nothing between the bars. */
static void write_line(FILE *out, int ncols, const char *const *fields)
{
	for (int i = 0; i < ncols; i++) {
		if (i)
			fputc('|', out);
		if (fields[i])
			fputs(fields[i], out);
	}
	fputc('\n', out);
}

void output_header(FILE *out, int ncols, const char *const *names)
{
	write_line(out, ncols, names);
}

void output_row(FILE *out, int ncols, const char *const *values)
{
	write_line(out, ncols, values);
}
