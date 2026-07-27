/*
 * knit-graph -- translate a read-only Cypher statement into SQL and run it
 * against a SQLite provenance database.
 *
 * M0: build skeleton. Argument handling, read-only database open, and a stub
 * that echoes what would be processed. The parser, transformer, executor, and
 * output formatting arrive in later milestones (see milestones.md).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

static const char *PROG = "knit-graph";

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: %s DBFILE 'CYPHER'\n"
		"\n"
		"Translate a read-only Cypher statement into SQL and run it against the\n"
		"SQLite provenance database DBFILE (opened read-only).\n"
		"\n"
		"Options:\n"
		"  -h, --help    show this help and exit\n",
		PROG);
}

int main(int argc, char **argv)
{
	const char *dbfile = NULL;
	const char *query = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			return 0;
		} else if (dbfile == NULL) {
			dbfile = argv[i];
		} else if (query == NULL) {
			query = argv[i];
		} else {
			fprintf(stderr, "%s: unexpected extra argument: %s\n", PROG, argv[i]);
			usage(stderr);
			return 2;
		}
	}

	if (dbfile == NULL || query == NULL) {
		fprintf(stderr, "%s: expected DBFILE and a Cypher statement\n", PROG);
		usage(stderr);
		return 2;
	}

	sqlite3 *db = NULL;
	int rc = sqlite3_open_v2(dbfile, &db, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "%s: cannot open database '%s': %s\n",
			PROG, dbfile, sqlite3_errmsg(db));
		sqlite3_close(db);
		return 1;
	}

	/* M0 stub: prove the pipeline entry points are wired up. */
	printf("knit-graph: opened '%s' read-only\n", dbfile);
	printf("knit-graph: received query: %s\n", query);
	printf("knit-graph: (Cypher parsing and SQL translation arrive in M1-M4)\n");

	sqlite3_close(db);
	return 0;
}
