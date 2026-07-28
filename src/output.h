/*
 * output.h -- render a result set in any of the sqlite3 CLI's output modes.
 *
 * The result set is buffered whole (see ResultSet) because several modes need
 * to know every value before writing the first byte: column/table/box/markdown
 * size each column to its widest cell, and json emits a single array. Rendering
 * mirrors the sqlite3 shell byte for byte so a knit-graph query can be diffed
 * against the equivalent sqlite3 CLI invocation.
 *
 * Empty result sets produce no output at all, in every mode -- matching the
 * sqlite3 shell, which prints nothing (not even a header) when no rows match.
 */

#ifndef KNIT_GRAPH_OUTPUT_H
#define KNIT_GRAPH_OUTPUT_H

#include <stdio.h>

/* The sqlite3 CLI output modes, selected by the matching command-line flag. */
typedef enum {
	OUT_LIST,	/* -list:     fields joined by the column separator      */
	OUT_ASCII,	/* -ascii:    unit- and record-separator delimited       */
	OUT_CSV,	/* -csv:      RFC-4180-style comma separated             */
	OUT_TABS,	/* -tabs:     tab separated                              */
	OUT_HTML,	/* -html:     <TR>/<TH>/<TD> rows                        */
	OUT_JSON,	/* -json:     an array of objects                        */
	OUT_LINE,	/* -line:     one "name = value" per line                */
	OUT_COLUMN,	/* -column:   left-aligned, space-padded columns         */
	OUT_TABLE,	/* -table:    ASCII-art bordered table                   */
	OUT_BOX,	/* -box:      Unicode box-drawing table                  */
	OUT_MARKDOWN	/* -markdown: a Markdown pipe table                      */
} OutputMode;

/* Rendering options assembled from the command-line flags. */
typedef struct {
	OutputMode mode;
	int header;		/* show the header row where the mode honours it */
	const char *colsep;	/* column separator (list/csv/tabs/ascii)       */
	const char *rowsep;	/* row separator     (list/csv/tabs/ascii)      */
} OutputOptions;

/*
 * A buffered result set. names[] and every rows[r][c] are owned (malloc'd);
 * a NULL rows[r][c] is a SQL NULL. types[r][c] holds the sqlite3 column type
 * (SQLITE_INTEGER/FLOAT/TEXT/NULL/BLOB), which json rendering needs to decide
 * whether a value is quoted.
 */
typedef struct {
	int ncols;
	char **names;
	int nrows;
	char ***rows;
	int **types;
} ResultSet;

/* Render rs to out in the requested mode. */
void output_result(FILE *out, const ResultSet *rs, const OutputOptions *opts);

#endif /* KNIT_GRAPH_OUTPUT_H */
