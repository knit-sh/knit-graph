/*
 * knit-graph -- translate a read-only Cypher statement into SQL and run it
 * against a SQLite provenance database.
 *
 * The plain DBFILE + query path runs end to end -- parse, translate to SQL
 * against the schema catalog, execute, and render the result set in any of the
 * sqlite3 CLI output modes (default "-list"), selected by the same flags the
 * sqlite3 shell uses (-json, -box, -csv, ..., -header/-noheader, -separator,
 * -newline).
 * `--ast` parses a statement and prints its syntax tree (no database needed).
 * `--catalog` introspects a database and lists its tables and columns, or
 * validates a single table / table.column reference. `--explain` translates a
 * statement into SQL (using the catalog) and prints it without executing.
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
#include "exec.h"
#include "names.h"
#include "transform.h"

static const char *PROG = "knit-graph";

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: %s [OUTPUT-OPTIONS] DBFILE 'CYPHER'\n"
		"       %s --ast 'CYPHER'\n"
		"       %s --explain DBFILE 'CYPHER'\n"
		"       %s --catalog DBFILE [TABLE[.COLUMN]]\n"
		"\n"
		"Translate a read-only Cypher statement into SQL and run it against the\n"
		"SQLite provenance database DBFILE (opened read-only).\n"
		"\n"
		"Modes:\n"
		"  --ast         parse only and print the syntax tree (no database)\n"
		"  --explain     translate to SQL and print it, without executing\n"
		"  --catalog     list the database's tables and columns; with a TABLE or\n"
		"                TABLE.COLUMN argument, validate that reference\n"
		"  -h, --help    show this help and exit\n"
		"\n"
		"Label resolution (default run and --explain):\n"
		"  --names SPEC          map entries 'table=name', separated by newlines\n"
		"                        or ';', so a label may be a table or command name\n"
		"  --names-file FILE     read the same map from FILE\n"
		"  --derive-table-names  with no map, derive it from the data\n"
		"\n"
		"Output options (mirroring the sqlite3 CLI; default is -list):\n"
		"  -ascii -box -column -csv -html -json -line -list -markdown -table -tabs\n"
		"  -header / -noheader   show or hide the column-name header\n"
		"  -separator SEP        column separator (list/csv/tabs/ascii modes)\n"
		"  -newline SEP          row separator     (list/csv/tabs/ascii modes)\n",
		PROG, PROG, PROG, PROG);
}

/*
 * If arg is an output flag, apply it to o and return 1. Following the sqlite3
 * shell, a mode flag only resets the separators it owns (csv/tabs the column
 * separator, ascii both; list touches neither), so a later -separator/-newline
 * always wins. -separator/-newline consume the following argv element, so *pi
 * is advanced past it; a missing value sets *want_value and returns 1 so the
 * caller can report it.
 */
static int output_flag(OutputOptions *o, char **argv, int argc, int *pi,
		       int *want_value)
{
	const char *a = argv[*pi];

	if (strcmp(a, "-list") == 0)          o->mode = OUT_LIST;
	else if (strcmp(a, "-csv") == 0)      { o->mode = OUT_CSV; o->colsep = ","; }
	else if (strcmp(a, "-tabs") == 0)     { o->mode = OUT_TABS; o->colsep = "\t"; }
	else if (strcmp(a, "-ascii") == 0)    { o->mode = OUT_ASCII;
						o->colsep = "\x1f"; o->rowsep = "\x1e"; }
	else if (strcmp(a, "-html") == 0)     o->mode = OUT_HTML;
	else if (strcmp(a, "-json") == 0)     o->mode = OUT_JSON;
	else if (strcmp(a, "-line") == 0)     o->mode = OUT_LINE;
	else if (strcmp(a, "-column") == 0)   o->mode = OUT_COLUMN;
	else if (strcmp(a, "-table") == 0)    o->mode = OUT_TABLE;
	else if (strcmp(a, "-box") == 0)      o->mode = OUT_BOX;
	else if (strcmp(a, "-markdown") == 0) o->mode = OUT_MARKDOWN;
	else if (strcmp(a, "-header") == 0)   o->header = 1;
	else if (strcmp(a, "-noheader") == 0) o->header = 0;
	else if (strcmp(a, "-separator") == 0 || strcmp(a, "-newline") == 0) {
		if (*pi + 1 >= argc) {
			*want_value = 1;
			return 1;
		}
		const char *val = argv[++(*pi)];
		if (a[1] == 's')
			o->colsep = val;
		else
			o->rowsep = val;
	} else {
		return 0;
	}
	return 1;
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
 * Build the explicit name map from the --names / --names-file options (at most
 * one may be given). Returns 0 with *out set (possibly NULL when neither option
 * was used), or a nonzero process exit code after reporting the error.
 */
static int build_explicit_map(const char *spec, const char *file, NameMap **out)
{
	*out = NULL;
	char *err = NULL;
	if (spec && file) {
		fprintf(stderr,
			"%s: specify at most one of --names / --names-file\n", PROG);
		return 2;
	}
	if (spec) {
		if (names_parse(spec, out, &err) != 0) {
			fprintf(stderr, "%s: %s\n", PROG,
				err ? err : "invalid name map");
			free(err);
			return 1;
		}
	} else if (file) {
		if (names_parse_file(file, out, &err) != 0) {
			fprintf(stderr, "%s: %s\n", PROG,
				err ? err : "invalid name map file");
			free(err);
			return 1;
		}
	}
	return 0;
}

/*
 * Choose the name map the transformer resolves labels through. An explicit map
 * (from --names/--names-file) always wins; otherwise, when --derive-table-names
 * was given, one is derived from the data (needing db and cat). Returns 0 and
 * sets *eff to the borrowed effective map (which may be NULL) and *owned to a
 * derived map the caller must names_free (or NULL). On failure returns 1 with a
 * malloc'd *err.
 */
static int effective_map(sqlite3 *db, const Catalog *cat,
                        const NameMap *explicit_map, int derive,
                        const NameMap **eff, NameMap **owned, char **err)
{
	*owned = NULL;
	*eff = explicit_map;
	if (!explicit_map && derive) {
		if (names_derive(db, cat, owned, err) != 0)
			return 1;
		*eff = *owned;
	}
	return 0;
}

/*
 * --explain: parse QUERY, translate it to SQL using DBFILE's schema, and print
 * the SQL without executing it. Returns a process exit code.
 */
static int run_explain(const char *dbfile, const char *query,
                      const NameMap *map, int derive)
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

	const NameMap *eff = NULL;
	NameMap *owned = NULL;
	if (effective_map(db, cat, map, derive, &eff, &owned, &err) != 0) {
		fprintf(stderr, "%s: %s\n", PROG,
			err ? err : "cannot derive table names");
		free(err);
		catalog_free(cat);
		sqlite3_close(db);
		return 1;
	}

	Query *q = parse_or_report(query);
	if (!q) {
		names_free(owned);
		catalog_free(cat);
		sqlite3_close(db);
		return 1;
	}

	char *sql = NULL;
	int status = 0;
	if (transform_query(q, cat, eff, &sql, &err) != 0) {
		fprintf(stderr, "%s: %s\n", PROG, err ? err : "cannot translate query");
		status = 1;
	} else {
		printf("%s\n", sql);
	}

	free(sql);
	free(err);
	ast_free_query(q);
	names_free(owned);
	catalog_free(cat);
	sqlite3_close(db);
	return status;
}

/*
 * Default path: parse QUERY, translate it to SQL against DBFILE's schema,
 * execute it, and print the result set in "-list" format. Returns an exit code.
 */
static int run_query(const char *dbfile, const char *query,
		     const OutputOptions *opts, const NameMap *map, int derive)
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

	const NameMap *eff = NULL;
	NameMap *owned = NULL;
	if (effective_map(db, cat, map, derive, &eff, &owned, &err) != 0) {
		fprintf(stderr, "%s: %s\n", PROG,
			err ? err : "cannot derive table names");
		free(err);
		catalog_free(cat);
		sqlite3_close(db);
		return 1;
	}

	Query *q = parse_or_report(query);
	if (!q) {
		names_free(owned);
		catalog_free(cat);
		sqlite3_close(db);
		return 1;
	}

	char *sql = NULL;
	int status = 0;
	if (transform_query(q, cat, eff, &sql, &err) != 0) {
		fprintf(stderr, "%s: %s\n", PROG, err ? err : "cannot translate query");
		status = 1;
	} else if (exec_query(db, sql, opts, &err) != 0) {
		fprintf(stderr, "%s: %s\n", PROG, err ? err : "cannot run query");
		status = 1;
	}

	free(sql);
	free(err);
	ast_free_query(q);
	names_free(owned);
	catalog_free(cat);
	sqlite3_close(db);
	return status;
}

int main(int argc, char **argv)
{
	int ast_mode = 0;
	int explain_mode = 0;
	int catalog_mode = 0;
	int derive = 0;
	const char *names_spec = NULL;
	const char *names_file = NULL;
	OutputOptions opts = { OUT_LIST, 1, "|", "\n" };
	const char *pos[2] = { NULL, NULL };
	int npos = 0;
	int i;

	for (i = 1; i < argc; i++) {
		int want_value = 0;
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			return 0;
		} else if (strcmp(argv[i], "--ast") == 0) {
			ast_mode = 1;
		} else if (strcmp(argv[i], "--explain") == 0) {
			explain_mode = 1;
		} else if (strcmp(argv[i], "--catalog") == 0) {
			catalog_mode = 1;
		} else if (strcmp(argv[i], "--derive-table-names") == 0) {
			derive = 1;
		} else if (strcmp(argv[i], "--names") == 0
				|| strcmp(argv[i], "--names-file") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s: %s expects an argument\n",
					PROG, argv[i]);
				usage(stderr);
				return 2;
			}
			if (argv[i][7] == '\0') /* --names */
				names_spec = argv[++i];
			else                    /* --names-file */
				names_file = argv[++i];
		} else if (output_flag(&opts, argv, argc, &i, &want_value)) {
			if (want_value) {
				fprintf(stderr, "%s: %s expects an argument\n",
					PROG, argv[i]);
				usage(stderr);
				return 2;
			}
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
		NameMap *map = NULL;
		int mrc = build_explicit_map(names_spec, names_file, &map);
		if (mrc)
			return mrc;
		int rc = run_explain(pos[0], pos[1], map, derive);
		names_free(map);
		return rc;
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

	NameMap *map = NULL;
	int mrc = build_explicit_map(names_spec, names_file, &map);
	if (mrc)
		return mrc;
	int rc = run_query(pos[0], pos[1], &opts, map, derive);
	names_free(map);
	return rc;
}
