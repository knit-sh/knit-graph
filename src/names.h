/*
 * names.h -- the command-name <-> table-name map knit-graph resolves labels
 * through.
 *
 * A Cypher node label plays two roles against the provenance schema: it names
 * the table to JOIN (to read a node's columns) and it supplies the value a
 * `source_name`/`target_name` edge filter matches. knit-graph assumes both
 * equal the label, but in a Knit database an override command records its
 * *command name* in `*_name` while its rows live in a differently named table
 * (e.g. the `submit` command's rows live in `jobs`). The map bridges the two:
 * each entry pairs a table name with the `*_name` value that table's edges
 * carry.
 *
 * A label is resolved to a (table, name) pair. The map is read both ways, so a
 * label may be written as either spelling:
 *   - as a table name  -> (that table, its recorded name)
 *   - as a command name -> (its table, that command name)
 * A label that is a table for one command and a command name for a *different*
 * command is a genuine ambiguity and is reported as an error rather than
 * guessed. A label absent from the map resolves to itself, (label, label), so
 * the default (no map) behaviour is unchanged.
 *
 * The map owns all of its memory; names_free releases it.
 */

#ifndef KNIT_GRAPH_NAMES_H
#define KNIT_GRAPH_NAMES_H

#include <sqlite3.h>

#include "catalog.h"

typedef struct NameMap NameMap;

/*
 * Parse an inline map specification: entries separated by newlines or ';',
 * each `table=name`. Surrounding whitespace on either side is trimmed and empty
 * entries are ignored. Returns 0 and stores the map in *out on success; on
 * failure returns non-zero and, if errmsg is non-NULL, stores a malloc'd
 * message there (caller frees). An empty spec yields an empty (but valid) map.
 */
int names_parse(const char *spec, NameMap **out, char **errmsg);

/* Read a spec from PATH, then names_parse it. */
int names_parse_file(const char *path, NameMap **out, char **errmsg);

/*
 * Derive the map from the data (the opt-in --derive-table-names path): for each
 * distinct `*_name` value in the edge table, resolve one of its ids to the
 * catalog table that holds it (uuid-exact) and record (table, name). Names with
 * no holding table are skipped. Returns 0 / stores *out, or non-zero with a
 * malloc'd errmsg.
 */
int names_derive(sqlite3 *db, const Catalog *cat, NameMap **out, char **errmsg);

/*
 * Resolve LABEL to its (table, name) pair. On success returns 0 and stores
 * borrowed pointers in *table and *name (valid while the map and label live).
 * A NULL map or a label absent from the map resolves to (label, label); a NULL
 * label yields (NULL, NULL). A genuinely ambiguous label returns non-zero with
 * a malloc'd errmsg.
 */
int names_resolve(const NameMap *map, const char *label,
                  const char **table, const char **name, char **errmsg);

void names_free(NameMap *map);

#endif /* KNIT_GRAPH_NAMES_H */
