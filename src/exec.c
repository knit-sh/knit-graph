/*
 * exec.c -- prepare/step the generated SQL and feed rows to the output layer.
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
static char *dup_err(const char *s)
{
	char *p = strdup(s ? s : "");
	if (!p) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	return p;
}

int exec_query(sqlite3 *db, const char *sql, char **errmsg)
{
	if (errmsg)
		*errmsg = NULL;

	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		if (errmsg)
			*errmsg = dup_err(sqlite3_errmsg(db));
		return 1;
	}

	int ncols = sqlite3_column_count(stmt);
	/* Scratch array of field pointers reused for the header and every row. */
	const char **fields = malloc((ncols > 0 ? ncols : 1) * sizeof(*fields));
	if (!fields) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}

	for (int i = 0; i < ncols; i++)
		fields[i] = sqlite3_column_name(stmt, i);
	output_header(stdout, ncols, fields);

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		for (int i = 0; i < ncols; i++)
			fields[i] = (const char *)sqlite3_column_text(stmt, i);
		output_row(stdout, ncols, fields);
	}

	free(fields);
	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE) {
		if (errmsg)
			*errmsg = dup_err(sqlite3_errmsg(db));
		return 1;
	}
	return 0;
}
