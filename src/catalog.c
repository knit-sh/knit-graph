/*
 * catalog.c -- read a provenance database's schema into a Catalog.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Duplicate a C string or abort; nothing sensible to do on OOM here. */
static char *xstrdup(const char *s)
{
	char *p = strdup(s ? s : "");
	if (!p) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	return p;
}

static void set_err(char **errmsg, const char *msg)
{
	if (errmsg && *errmsg == NULL)
		*errmsg = xstrdup(msg);
}

/* Append one column name to a table's growable column array. */
static void table_add_column(CatalogTable *t, const char *col)
{
	char **grown = realloc(t->columns, (t->ncolumns + 1) * sizeof(*grown));
	if (!grown) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	t->columns = grown;
	t->columns[t->ncolumns++] = xstrdup(col);
}

/* Read the column names of one table via PRAGMA table_info. */
static int load_columns(sqlite3 *db, CatalogTable *t, char **errmsg)
{
	char *sql = sqlite3_mprintf("PRAGMA table_info(%Q)", t->name);
	if (!sql) {
		set_err(errmsg, "out of memory");
		return 1;
	}

	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	sqlite3_free(sql);
	if (rc != SQLITE_OK) {
		set_err(errmsg, sqlite3_errmsg(db));
		return 1;
	}

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		/* table_info columns: cid, name, type, notnull, dflt_value, pk */
		const unsigned char *name = sqlite3_column_text(stmt, 1);
		if (name)
			table_add_column(t, (const char *)name);
	}
	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE) {
		set_err(errmsg, sqlite3_errmsg(db));
		return 1;
	}
	return 0;
}

/* Append a table (name only; columns filled in by the caller). */
static CatalogTable *catalog_add_table(Catalog *cat, const char *name)
{
	CatalogTable *grown =
		realloc(cat->tables, (cat->ntables + 1) * sizeof(*grown));
	if (!grown) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	cat->tables = grown;
	CatalogTable *t = &cat->tables[cat->ntables++];
	t->name = xstrdup(name);
	t->columns = NULL;
	t->ncolumns = 0;
	return t;
}

int catalog_load(sqlite3 *db, Catalog **out, char **errmsg)
{
	*out = NULL;
	if (errmsg)
		*errmsg = NULL;

	Catalog *cat = calloc(1, sizeof(*cat));
	if (!cat) {
		set_err(errmsg, "out of memory");
		return 1;
	}

	/* Enumerate user tables (skip SQLite's internal ones), sorted by name. */
	const char *sql =
		"SELECT name FROM sqlite_master "
		"WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
		"ORDER BY name";
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		set_err(errmsg, sqlite3_errmsg(db));
		catalog_free(cat);
		return 1;
	}

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const unsigned char *name = sqlite3_column_text(stmt, 0);
		if (name)
			catalog_add_table(cat, (const char *)name);
	}
	sqlite3_finalize(stmt);

	if (rc != SQLITE_DONE) {
		set_err(errmsg, sqlite3_errmsg(db));
		catalog_free(cat);
		return 1;
	}

	for (int i = 0; i < cat->ntables; i++) {
		if (load_columns(db, &cat->tables[i], errmsg) != 0) {
			catalog_free(cat);
			return 1;
		}
	}

	*out = cat;
	return 0;
}

void catalog_free(Catalog *cat)
{
	if (!cat)
		return;
	for (int i = 0; i < cat->ntables; i++) {
		CatalogTable *t = &cat->tables[i];
		for (int j = 0; j < t->ncolumns; j++)
			free(t->columns[j]);
		free(t->columns);
		free(t->name);
	}
	free(cat->tables);
	free(cat);
}

const CatalogTable *catalog_find_table(const Catalog *cat, const char *name)
{
	if (!cat || !name)
		return NULL;
	for (int i = 0; i < cat->ntables; i++) {
		if (strcmp(cat->tables[i].name, name) == 0)
			return &cat->tables[i];
	}
	return NULL;
}

int catalog_table_has_column(const CatalogTable *t, const char *column)
{
	if (!t || !column)
		return 0;
	for (int j = 0; j < t->ncolumns; j++) {
		if (strcmp(t->columns[j], column) == 0)
			return 1;
	}
	return 0;
}
