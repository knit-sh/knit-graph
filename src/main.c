/*
 * knit-graph -- translate a read-only Cypher statement into SQL and run it
 * against a SQLite provenance database.
 *
 * Current state (M1): the Cypher parser is wired in. `--ast` parses a statement
 * and prints its syntax tree (no database needed). The database path still only
 * opens the DB read-only and confirms the query parses; SQL translation and
 * execution arrive in M3/M4.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "ast.h"

static const char *PROG = "knit-graph";

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: %s [--ast] DBFILE 'CYPHER'\n"
		"       %s --ast 'CYPHER'\n"
		"\n"
		"Translate a read-only Cypher statement into SQL and run it against the\n"
		"SQLite provenance database DBFILE (opened read-only).\n"
		"\n"
		"Options:\n"
		"  --ast         parse only and print the syntax tree (no database)\n"
		"  -h, --help    show this help and exit\n",
		PROG, PROG);
}

/* Parse the query, reporting any error to stderr. Returns the tree or NULL. */
static Query *parse_or_report(const char *query)
{
	Query *q = NULL;
	char *err = NULL;

	if (cypher_parse(query, &q, &err) != 0) {
		fprintf(stderr, "%s: %s\n", PROG, err ? err : "parse error");
		free(err);
		return NULL;
	}
	return q;
}

int main(int argc, char **argv)
{
	int ast_mode = 0;
	const char *pos[2] = { NULL, NULL };
	int npos = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			return 0;
		} else if (strcmp(argv[i], "--ast") == 0) {
			ast_mode = 1;
		} else if (npos < 2) {
			pos[npos++] = argv[i];
		} else {
			fprintf(stderr, "%s: unexpected extra argument: %s\n", PROG, argv[i]);
			usage(stderr);
			return 2;
		}
	}

	/* --ast: parse a single statement and dump the tree, no database. */
	if (ast_mode) {
		if (npos != 1) {
			fprintf(stderr, "%s: --ast expects a single Cypher statement\n", PROG);
			usage(stderr);
			return 2;
		}
		Query *q = parse_or_report(pos[0]);
		if (!q)
			return 1;
		ast_dump(stdout, q);
		ast_free_query(q);
		return 0;
	}

	if (npos != 2) {
		fprintf(stderr, "%s: expected DBFILE and a Cypher statement\n", PROG);
		usage(stderr);
		return 2;
	}

	const char *dbfile = pos[0];
	const char *query = pos[1];

	sqlite3 *db = NULL;
	int rc = sqlite3_open_v2(dbfile, &db, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "%s: cannot open database '%s': %s\n",
			PROG, dbfile, sqlite3_errmsg(db));
		sqlite3_close(db);
		return 1;
	}

	Query *q = parse_or_report(query);
	if (!q) {
		sqlite3_close(db);
		return 1;
	}

	/* M1 stub: parsing succeeded. SQL translation/execution land in M3/M4. */
	printf("knit-graph: opened '%s' read-only\n", dbfile);
	printf("knit-graph: query parsed successfully\n");

	ast_free_query(q);
	sqlite3_close(db);
	return 0;
}
