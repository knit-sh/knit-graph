/*
 * output.h -- render a result set for the terminal.
 *
 * M4 implements only sqlite3's default "-list" mode: fields separated by a
 * single '|', one row per line, a NULL field shown as empty. A header row of
 * column names is written first. The remaining sqlite3 CLI modes arrive in M10.
 */

#ifndef KNIT_GRAPH_OUTPUT_H
#define KNIT_GRAPH_OUTPUT_H

#include <stdio.h>

/* Write the header line: the column names, pipe-separated. */
void output_header(FILE *out, int ncols, const char *const *names);

/* Write one data line: the field values, pipe-separated (NULL -> empty). */
void output_row(FILE *out, int ncols, const char *const *values);

#endif /* KNIT_GRAPH_OUTPUT_H */
