/*
 * exec.c -- prepare/step the generated SQL, buffer the result set, and render
 * it via the output module.
 *
 * The whole result set is materialised before any output because several modes
 * (column/table/box/markdown) size their columns to the widest value and json
 * emits a single array. Every allocation is owned by the ResultSet and freed on
 * all paths, including errors.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "exec.h"
#include "output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Duplicate a C string or abort; nothing sensible to do on OOM here. */
static char *dup_or_die(const char *s)
{
	char *p = strdup(s ? s : "");
	if (!p) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	return p;
}

/* Free a ResultSet's owned storage (names, rows, per-cell types). */
static void resultset_free(ResultSet *rs)
{
	for (int c = 0; c < rs->ncols; c++)
		free(rs->names[c]);
	free(rs->names);
	for (int r = 0; r < rs->nrows; r++) {
		for (int c = 0; c < rs->ncols; c++)
			free(rs->rows[r][c]);
		free(rs->rows[r]);
		free(rs->types[r]);
	}
	free(rs->rows);
	free(rs->types);
}

int exec_query(sqlite3 *db, const char *sql, const OutputOptions *opts,
	       char **errmsg)
{
	if (errmsg)
		*errmsg = NULL;

	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		if (errmsg)
			*errmsg = dup_or_die(sqlite3_errmsg(db));
		return 1;
	}

	ResultSet rs = { 0 };
	rs.ncols = sqlite3_column_count(stmt);
	rs.names = calloc(rs.ncols > 0 ? rs.ncols : 1, sizeof(*rs.names));
	if (!rs.names) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	for (int c = 0; c < rs.ncols; c++)
		rs.names[c] = dup_or_die(sqlite3_column_name(stmt, c));

	int cap = 0;
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		if (rs.nrows == cap) {
			cap = cap ? cap * 2 : 8;
			char ***nr = realloc(rs.rows, cap * sizeof(*rs.rows));
			int **nt = realloc(rs.types, cap * sizeof(*rs.types));
			if (!nr || !nt) {
				free(nr ? nr : rs.rows);
				free(nt ? nt : rs.types);
				fprintf(stderr, "knit-graph: out of memory\n");
				exit(1);
			}
			rs.rows = nr;
			rs.types = nt;
		}

		char **row = calloc(rs.ncols > 0 ? rs.ncols : 1, sizeof(*row));
		int *types = calloc(rs.ncols > 0 ? rs.ncols : 1, sizeof(*types));
		if (!row || !types) {
			free(row);
			free(types);
			fprintf(stderr, "knit-graph: out of memory\n");
			exit(1);
		}
		for (int c = 0; c < rs.ncols; c++) {
			types[c] = sqlite3_column_type(stmt, c);
			const unsigned char *v = sqlite3_column_text(stmt, c);
			/* A SQL NULL stays a NULL pointer; other types keep
			 * sqlite's own text rendering (e.g. "1.5", "0.0"). */
			row[c] = v ? dup_or_die((const char *)v) : NULL;
		}
		rs.rows[rs.nrows] = row;
		rs.types[rs.nrows] = types;
		rs.nrows++;
	}

	int status = 0;
	if (rc != SQLITE_DONE) {
		if (errmsg)
			*errmsg = dup_or_die(sqlite3_errmsg(db));
		status = 1;
	} else {
		output_result(stdout, &rs, opts);
	}

	resultset_free(&rs);
	sqlite3_finalize(stmt);
	return status;
}
