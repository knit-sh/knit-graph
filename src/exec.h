/*
 * exec.h -- run generated SQL against the open database.
 *
 * exec_query prepares and steps the statement, buffers the whole result set,
 * and hands it to the output module to render in the requested mode (opts).
 */

#ifndef KNIT_GRAPH_EXEC_H
#define KNIT_GRAPH_EXEC_H

#include <sqlite3.h>

#include "output.h"

/*
 * Prepare SQL, execute it against db, and write the result set to stdout in the
 * format described by opts. Returns 0 on success. On failure returns non-zero
 * and, if errmsg is non-NULL, stores a malloc'd message there (caller frees).
 */
int exec_query(sqlite3 *db, const char *sql, const OutputOptions *opts,
	       char **errmsg);

#endif /* KNIT_GRAPH_EXEC_H */
