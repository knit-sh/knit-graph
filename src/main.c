/*
 * knit-graph -- translate a read-only Cypher statement into SQL and run it
 * against a SQLite provenance database.
 *
 * Current state (M3): the Cypher parser is wired in and a schema catalog can be
 * read from the database. `--ast` parses a statement and prints its syntax tree
 * (no database needed). `--catalog` introspects a database and lists its tables
 * and columns, or validates a single table / table.column reference.
 * `--explain` translates a statement into SQL (using the catalog) and prints it
 * without executing. The plain DBFILE + query path still only opens the DB
 * read-only and confirms the query parses; execution arrives in M4.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "ast.h"
#include "catalog.h"
#include "transform.h"

static const char *PROG = "knit-graph";

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: %s [--ast] DBFILE 'CYPHER'\n"
		"       %s --ast 'CYPHER'\n"
		"       %s --explain DBFILE 'CYPHER'\n"
		"       %s --catalog DBFILE [TABLE[.COLUMN]]\n"
		"\n"
		"Translate a read-only Cypher statement into SQL and run it against the\n"
		"SQLite provenance database DBFILE (opened read-only).\n"
		"\n"
		"Options:\n"
		"  --ast         parse only and print the syntax tree (no database)\n"
		"  --explain     translate to SQL and print it, without executing\n"
		"  --catalog     list the database's tables and columns; with a TABLE or\n"
		"                TABLE.COLUMN argument, validate that reference\n"
		"  -h, --help    show this help and exit\n",
		PROG, PROG, PROG, PROG);
}

/* Open DBFILE read-only, reporting failure to stderr. Returns NULL on error. */
static sqlite3 *open_db_ro(const char *dbfile)
{
	sqlite3 *db = NULL;
	int rc = sqlite3_open_v2(dbfile, &db, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "%s: cannot open database '%s': %s\n",
			PROG, dbfile, sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}
	return db;
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

static void print_table(const CatalogTable *t)
{
	printf("table %s\n", t->name);
	for (int j = 0; j < t->ncolumns; j++)
		printf("  column %s\n", t->columns[j]);
}

/*
 * --catalog: introspect DBFILE. With no reference, list every table and its
 * columns. With TABLE, list that one table. With TABLE.COLUMN, confirm the
 * column exists. Returns a process exit code.
 */
static int run_catalog(const char *dbfile, const char *ref)
{
	sqlite3 *db = open_db_ro(dbfile);
	if (!db)
		return 1;

	Catalog *cat = NULL;
	char *err = NULL;
	if (catalog_load(db, &cat, &err) != 0) {
		fprintf(stderr, "%s: %s\n", PROG, err ? err : "cannot read schema");
		free(err);
		sqlite3_close(db);
		return 1;
	}

	int status = 0;

	if (ref == NULL) {
		for (int i = 0; i < cat->ntables; i++)
			print_table(&cat->tables[i]);
	} else {
		/* Split TABLE.COLUMN at the last dot; TABLE has no dot. */
		const char *dot = strrchr(ref, '.');
		char *table = NULL;
		const char *column = NULL;
		if (dot) {
			size_t n = (size_t)(dot - ref);
			table = malloc(n + 1);
			if (!table) {
				fprintf(stderr, "%s: out of memory\n", PROG);
				catalog_free(cat);
				sqlite3_close(db);
				return 1;
			}
			memcpy(table, ref, n);
			table[n] = '\0';
			column = dot + 1;
		}

		const char *tname = table ? table : ref;
		const CatalogTable *t = catalog_find_table(cat, tname);
		if (!t) {
			fprintf(stderr, "%s: unknown table: %s\n", PROG, tname);
			status = 1;
		} else if (column) {
			if (catalog_table_has_column(t, column)) {
				printf("%s.%s\n", tname, column);
			} else {
				fprintf(stderr, "%s: unknown column: %s.%s\n",
					PROG, tname, column);
				status = 1;
			}
		} else {
			print_table(t);
		}
		free(table);
	}

	catalog_free(cat);
	sqlite3_close(db);
	return status;
}

/*
 * --explain: parse QUERY, translate it to SQL using DBFILE's schema, and print
 * the SQL without executing it. Returns a process exit code.
 */
static int run_explain(const char *dbfile, const char *query)
{
	sqlite3 *db = open_db_ro(dbfile);
	if (!db)
		return 1;

	Catalog *cat = NULL;
	char *err = NULL;
	if (catalog_load(db, &cat, &err) != 0) {
		fprintf(stderr, "%s: %s\n", PROG, err ? err : "cannot read schema");
		free(err);
		sqlite3_close(db);
		return 1;
	}

	Query *q = parse_or_report(query);
	if (!q) {
		catalog_free(cat);
		sqlite3_close(db);
		return 1;
	}

	char *sql = NULL;
	int status = 0;
	if (transform_query(q, cat, &sql, &err) != 0) {
		fprintf(stderr, "%s: %s\n", PROG, err ? err : "cannot translate query");
		status = 1;
	} else {
		printf("%s\n", sql);
	}

	free(sql);
	free(err);
	ast_free_query(q);
	catalog_free(cat);
	sqlite3_close(db);
	return status;
}

int main(int argc, char **argv)
{
	int ast_mode = 0;
	int explain_mode = 0;
	int catalog_mode = 0;
	const char *pos[2] = { NULL, NULL };
	int npos = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			return 0;
		} else if (strcmp(argv[i], "--ast") == 0) {
			ast_mode = 1;
		} else if (strcmp(argv[i], "--explain") == 0) {
			explain_mode = 1;
		} else if (strcmp(argv[i], "--catalog") == 0) {
			catalog_mode = 1;
		} else if (npos < 2) {
			pos[npos++] = argv[i];
		} else {
			fprintf(stderr, "%s: unexpected extra argument: %s\n", PROG, argv[i]);
			usage(stderr);
			return 2;
		}
	}

	if (ast_mode + explain_mode + catalog_mode > 1) {
		fprintf(stderr,
			"%s: --ast, --explain and --catalog are mutually exclusive\n",
			PROG);
		usage(stderr);
		return 2;
	}

	/* --explain: translate QUERY to SQL against DBFILE and print it. */
	if (explain_mode) {
		if (npos != 2) {
			fprintf(stderr,
				"%s: --explain expects a database file and a Cypher statement\n",
				PROG);
			usage(stderr);
			return 2;
		}
		return run_explain(pos[0], pos[1]);
	}

	/* --catalog: introspect DBFILE, optionally validating a reference. */
	if (catalog_mode) {
		if (npos < 1) {
			fprintf(stderr, "%s: --catalog expects a database file\n", PROG);
			usage(stderr);
			return 2;
		}
		return run_catalog(pos[0], npos == 2 ? pos[1] : NULL);
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

	sqlite3 *db = open_db_ro(dbfile);
	if (!db)
		return 1;

	Query *q = parse_or_report(query);
	if (!q) {
		sqlite3_close(db);
		return 1;
	}

	/* M2 stub: parsing succeeded. SQL translation/execution land in M3/M4. */
	printf("knit-graph: opened '%s' read-only\n", dbfile);
	printf("knit-graph: query parsed successfully\n");

	ast_free_query(q);
	sqlite3_close(db);
	return 0;
}
