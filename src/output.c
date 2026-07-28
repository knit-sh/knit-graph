/*
 * output.c -- render a buffered result set in each sqlite3 CLI output mode.
 *
 * Every renderer reproduces the sqlite3 shell's bytes exactly (as of 3.45), so
 * a query's output can be diffed against the equivalent sqlite3 invocation. A
 * few shared quirks worth stating once:
 *   - An empty result set yields no output at all, in every mode.
 *   - list/ascii/tabs/csv honour the -header flag and the column/row
 *     separators; html honours -header too.
 *   - json and line always show names (there is no header to suppress);
 *     table/box/markdown always draw their header row even under -noheader,
 *     while -column suppresses it. This matches the shell.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "output.h"

#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

/* A cell's text, with SQL NULL rendered as the empty string. */
static const char *cell(const ResultSet *rs, int r, int c)
{
	const char *v = rs->rows[r][c];
	return v ? v : "";
}

/* ------------------------------------------------------------------ *
 * list / ascii / tabs: fields joined by colsep, rows ended by rowsep.
 * ------------------------------------------------------------------ */
static void render_separated(FILE *out, const ResultSet *rs,
			     const OutputOptions *o)
{
	if (o->header) {
		for (int c = 0; c < rs->ncols; c++) {
			if (c)
				fputs(o->colsep, out);
			fputs(rs->names[c], out);
		}
		fputs(o->rowsep, out);
	}
	for (int r = 0; r < rs->nrows; r++) {
		for (int c = 0; c < rs->ncols; c++) {
			if (c)
				fputs(o->colsep, out);
			fputs(cell(rs, r, c), out);
		}
		fputs(o->rowsep, out);
	}
}

/* ------------------------------------------------------------------ *
 * csv: quote a field iff it holds the separator, a quote, CR or LF.
 * ------------------------------------------------------------------ */
static void csv_field(FILE *out, const char *s, const char *sep)
{
	int quote = strpbrk(s, "\"\r\n") != NULL
		|| (sep[0] && strstr(s, sep) != NULL);
	if (!quote) {
		fputs(s, out);
		return;
	}
	fputc('"', out);
	for (const char *p = s; *p; p++) {
		if (*p == '"')
			fputc('"', out);
		fputc(*p, out);
	}
	fputc('"', out);
}

static void render_csv(FILE *out, const ResultSet *rs, const OutputOptions *o)
{
	if (o->header) {
		for (int c = 0; c < rs->ncols; c++) {
			if (c)
				fputs(o->colsep, out);
			csv_field(out, rs->names[c], o->colsep);
		}
		fputs(o->rowsep, out);
	}
	for (int r = 0; r < rs->nrows; r++) {
		for (int c = 0; c < rs->ncols; c++) {
			if (c)
				fputs(o->colsep, out);
			/* A SQL NULL is written empty and unquoted. */
			if (rs->rows[r][c])
				csv_field(out, rs->rows[r][c], o->colsep);
		}
		fputs(o->rowsep, out);
	}
}

/* ------------------------------------------------------------------ *
 * html: <TR> of <TH>/<TD> cells; escape the four markup-significant chars.
 * ------------------------------------------------------------------ */
static void html_escape(FILE *out, const char *s)
{
	for (const char *p = s; *p; p++) {
		switch (*p) {
		case '<': fputs("&lt;", out); break;
		case '>': fputs("&gt;", out); break;
		case '&': fputs("&amp;", out); break;
		case '"': fputs("&quot;", out); break;
		default:  fputc(*p, out);
		}
	}
}

static void render_html(FILE *out, const ResultSet *rs, const OutputOptions *o)
{
	if (o->header) {
		fputs("<TR>", out);
		for (int c = 0; c < rs->ncols; c++) {
			fputs("<TH>", out);
			html_escape(out, rs->names[c]);
			fputs("</TH>\n", out);
		}
		fputs("</TR>\n", out);
	}
	for (int r = 0; r < rs->nrows; r++) {
		fputs("<TR>", out);
		for (int c = 0; c < rs->ncols; c++) {
			fputs("<TD>", out);
			html_escape(out, cell(rs, r, c));
			fputs("</TD>\n", out);
		}
		fputs("</TR>\n", out);
	}
}

/* ------------------------------------------------------------------ *
 * json: an array of objects; numbers unquoted, text escaped, NULL -> null.
 * ------------------------------------------------------------------ */
static void json_string(FILE *out, const char *s)
{
	fputc('"', out);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		switch (*p) {
		case '"':  fputs("\\\"", out); break;
		case '\\': fputs("\\\\", out); break;
		case '\b': fputs("\\b", out); break;
		case '\f': fputs("\\f", out); break;
		case '\n': fputs("\\n", out); break;
		case '\r': fputs("\\r", out); break;
		case '\t': fputs("\\t", out); break;
		default:
			if (*p < 0x20)
				fprintf(out, "\\u%04x", *p);
			else
				fputc(*p, out);
		}
	}
	fputc('"', out);
}

static void render_json(FILE *out, const ResultSet *rs)
{
	fputc('[', out);
	for (int r = 0; r < rs->nrows; r++) {
		if (r)
			fputs(",\n", out);
		fputc('{', out);
		for (int c = 0; c < rs->ncols; c++) {
			if (c)
				fputc(',', out);
			json_string(out, rs->names[c]);
			fputc(':', out);
			int t = rs->types[r][c];
			if (t == SQLITE_NULL || !rs->rows[r][c])
				fputs("null", out);
			else if (t == SQLITE_INTEGER || t == SQLITE_FLOAT)
				fputs(rs->rows[r][c], out);
			else
				json_string(out, rs->rows[r][c]);
		}
		fputc('}', out);
	}
	fputs("]\n", out);
}

/* ------------------------------------------------------------------ *
 * line: "name = value", the name right-aligned to max(5, widest name).
 * ------------------------------------------------------------------ */
static void render_line(FILE *out, const ResultSet *rs)
{
	int w = 5;
	for (int c = 0; c < rs->ncols; c++) {
		int len = (int)strlen(rs->names[c]);
		if (len > w)
			w = len;
	}
	for (int r = 0; r < rs->nrows; r++) {
		if (r)
			fputc('\n', out);	/* blank line between rows */
		for (int c = 0; c < rs->ncols; c++)
			fprintf(out, "%*s = %s\n", w, rs->names[c],
				cell(rs, r, c));
	}
}

/* ------------------------------------------------------------------ *
 * Column-width helpers, shared by column/table/box/markdown.
 * ------------------------------------------------------------------ */

/* width[c] = max(name, widest cell); caller frees the returned array. */
static int *column_widths(const ResultSet *rs)
{
	int *w = malloc((rs->ncols > 0 ? rs->ncols : 1) * sizeof(*w));
	if (!w)
		return NULL;
	for (int c = 0; c < rs->ncols; c++) {
		w[c] = (int)strlen(rs->names[c]);
		for (int r = 0; r < rs->nrows; r++) {
			int len = (int)strlen(cell(rs, r, c));
			if (len > w[c])
				w[c] = len;
		}
	}
	return w;
}

/* Write s in a field of width w, left-aligned or centered. */
static void pad_cell(FILE *out, const char *s, int w, int center)
{
	int len = (int)strlen(s);
	int slack = w - len;
	if (slack < 0)
		slack = 0;
	int left = center ? slack / 2 : 0;
	int right = slack - left;
	for (int i = 0; i < left; i++)
		fputc(' ', out);
	fputs(s, out);
	for (int i = 0; i < right; i++)
		fputc(' ', out);
}

/* -column: left-aligned, two-space gaps; a dashed rule under the header. */
static void render_column(FILE *out, const ResultSet *rs,
			  const OutputOptions *o, const int *w)
{
	if (o->header) {
		for (int c = 0; c < rs->ncols; c++) {
			if (c)
				fputs("  ", out);
			pad_cell(out, rs->names[c], w[c], 0);
		}
		fputc('\n', out);
		for (int c = 0; c < rs->ncols; c++) {
			if (c)
				fputs("  ", out);
			for (int i = 0; i < w[c]; i++)
				fputc('-', out);
		}
		fputc('\n', out);
	}
	for (int r = 0; r < rs->nrows; r++) {
		for (int c = 0; c < rs->ncols; c++) {
			if (c)
				fputs("  ", out);
			pad_cell(out, cell(rs, r, c), w[c], 0);
		}
		fputc('\n', out);
	}
}

/* A horizontal rule for table/box/markdown: corners/tee joined by hbar. */
static void rule(FILE *out, const char *left, const char *mid,
		 const char *right, const char *hbar, const int *w, int n)
{
	fputs(left, out);
	for (int c = 0; c < n; c++) {
		if (c)
			fputs(mid, out);
		for (int i = 0; i < w[c] + 2; i++)
			fputs(hbar, out);
	}
	fputs(right, out);
	fputc('\n', out);
}

/* A "| a | b |" data/header row for table/box/markdown. */
static void grid_row(FILE *out, const char *vert, const char *const *vals,
		     const int *w, int n, int center)
{
	for (int c = 0; c < n; c++) {
		fputs(vert, out);
		fputc(' ', out);
		pad_cell(out, vals[c], w[c], center);
		fputc(' ', out);
	}
	fputs(vert, out);
	fputc('\n', out);
}

/* Collect a row's cell texts (NULL -> "") into buf for grid_row. */
static void row_cells(const ResultSet *rs, int r, const char **buf)
{
	for (int c = 0; c < rs->ncols; c++)
		buf[c] = cell(rs, r, c);
}

/* -table (+---+) and -box (Unicode) share this bordered layout. */
static void render_boxed(FILE *out, const ResultSet *rs, const int *w,
			 const char *tl, const char *tm, const char *tr,
			 const char *ml, const char *mm, const char *mr,
			 const char *bl, const char *bm, const char *br,
			 const char *vert, const char *hbar, const char **buf)
{
	rule(out, tl, tm, tr, hbar, w, rs->ncols);
	grid_row(out, vert, (const char *const *)rs->names, w, rs->ncols, 1);
	rule(out, ml, mm, mr, hbar, w, rs->ncols);
	for (int r = 0; r < rs->nrows; r++) {
		row_cells(rs, r, buf);
		grid_row(out, vert, buf, w, rs->ncols, 0);
	}
	rule(out, bl, bm, br, hbar, w, rs->ncols);
}

/* -markdown: a header row, a dashed separator, then data. No outer border. */
static void render_markdown(FILE *out, const ResultSet *rs, const int *w,
			    const char **buf)
{
	grid_row(out, "|", (const char *const *)rs->names, w, rs->ncols, 1);
	rule(out, "|", "|", "|", "-", w, rs->ncols);
	for (int r = 0; r < rs->nrows; r++) {
		row_cells(rs, r, buf);
		grid_row(out, "|", buf, w, rs->ncols, 0);
	}
}

void output_result(FILE *out, const ResultSet *rs, const OutputOptions *o)
{
	/* Like the sqlite3 shell, an empty result prints nothing at all. */
	if (rs->nrows == 0)
		return;

	switch (o->mode) {
	case OUT_LIST:
	case OUT_ASCII:
	case OUT_TABS:
		render_separated(out, rs, o);
		return;
	case OUT_CSV:
		render_csv(out, rs, o);
		return;
	case OUT_HTML:
		render_html(out, rs, o);
		return;
	case OUT_JSON:
		render_json(out, rs);
		return;
	case OUT_LINE:
		render_line(out, rs);
		return;
	default:
		break;
	}

	/* The columnar modes size each column to its widest cell first. */
	int *w = column_widths(rs);
	const char **buf = malloc((rs->ncols > 0 ? rs->ncols : 1) * sizeof(*buf));
	if (!w || !buf) {
		free(w);
		free(buf);
		fprintf(stderr, "knit-graph: out of memory\n");
		return;
	}

	switch (o->mode) {
	case OUT_COLUMN:
		render_column(out, rs, o, w);
		break;
	case OUT_TABLE:
		render_boxed(out, rs, w,
			"+", "+", "+", "+", "+", "+", "+", "+", "+",
			"|", "-", buf);
		break;
	case OUT_BOX:
		render_boxed(out, rs, w,
			"┌", "┬", "┐",	/* top    */
			"├", "┼", "┤",	/* middle */
			"└", "┴", "┘",	/* bottom */
			"│", "─", buf);	/* vert, hbar */
		break;
	case OUT_MARKDOWN:
		render_markdown(out, rs, w, buf);
		break;
	default:
		break;
	}

	free(w);
	free(buf);
}
