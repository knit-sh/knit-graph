/*
 * transform.h -- translate a parsed Cypher Query into a SQL statement.
 *
 * M3 handles the two simplest shapes: a single node pattern and a single
 * relationship (with direction reversal). Constructs that later milestones add
 * (WHERE, chains, aggregation, variable-length paths, whole-entity RETURN, ...)
 * are rejected with a clear message rather than silently mistranslated.
 */

#ifndef KNIT_GRAPH_TRANSFORM_H
#define KNIT_GRAPH_TRANSFORM_H

#include "ast.h"
#include "catalog.h"

/*
 * Translate Query q into SQL, resolving labels and validating properties
 * against cat. On success returns 0 and stores a malloc'd SQL string in *sql
 * (caller frees). On failure returns non-zero and, if errmsg is non-NULL,
 * stores a malloc'd message there (caller frees).
 */
int transform_query(const Query *q, const Catalog *cat,
                    char **sql, char **errmsg);

#endif /* KNIT_GRAPH_TRANSFORM_H */
