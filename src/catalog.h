/*
 * catalog.h -- schema introspection for a knit-graph provenance database.
 *
 * On load the catalog reads sqlite_master and PRAGMA table_info to learn which
 * tables exist and what columns each has. Function (node) tables use their
 * name directly (colons denote namespaces, e.g. "ns:f"); the edge table is
 * "__provenance__". The transformer (M3+) uses this to expand "RETURN a",
 * resolve labels/properties to columns, and report unknown tables/columns.
 *
 * The catalog owns all of its memory; catalog_free releases it.
 */

#ifndef KNIT_GRAPH_CATALOG_H
#define KNIT_GRAPH_CATALOG_H

#include <sqlite3.h>

/* The conventional name of the provenance edge table. */
#define KG_EDGE_TABLE "__provenance__"

typedef struct CatalogTable {
	char  *name;        /* table name, e.g. "ns:f" or "__provenance__" */
	char **columns;     /* column names, in schema (cid) order */
	int    ncolumns;
} CatalogTable;

typedef struct Catalog {
	CatalogTable *tables;   /* sorted by name */
	int           ntables;
} Catalog;

/*
 * Load the catalog from an open database. Returns 0 and stores the catalog in
 * *out on success. On failure returns non-zero and, if errmsg is non-NULL,
 * stores a malloc'd message there (caller frees).
 */
int catalog_load(sqlite3 *db, Catalog **out, char **errmsg);

void catalog_free(Catalog *cat);

/* Look up a table by exact name; returns NULL if there is no such table. */
const CatalogTable *catalog_find_table(const Catalog *cat, const char *name);

/* Non-zero if the table has a column with the given name. */
int catalog_table_has_column(const CatalogTable *t, const char *column);

#endif /* KNIT_GRAPH_CATALOG_H */
