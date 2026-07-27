/*
 * exec.h -- run generated SQL against the open database.
 *
 * exec_query prepares and steps the statement, feeding the column names and
 * each row's values to the output module (M4: default "-list" format).
 */

#ifndef KNIT_GRAPH_EXEC_H
#define KNIT_GRAPH_EXEC_H

#include <sqlite3.h>

/*
 * Prepare SQL, execute it against db, and write the result set to stdout.
 * Returns 0 on success. On failure returns non-zero and, if errmsg is
 * non-NULL, stores a malloc'd message there (caller frees).
 */
int exec_query(sqlite3 *db, const char *sql, char **errmsg);

#endif /* KNIT_GRAPH_EXEC_H */
