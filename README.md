# knit-graph

[![CI](https://github.com/knit-sh/knit-graph/actions/workflows/ci.yml/badge.svg)](https://github.com/knit-sh/knit-graph/actions/workflows/ci.yml)
[![Code coverage](https://github.com/knit-sh/knit-graph/actions/workflows/coverage.yml/badge.svg)](https://github.com/knit-sh/knit-graph/actions/workflows/coverage.yml)
[![codecov](https://codecov.io/gh/knit-sh/knit-graph/branch/main/graph/badge.svg)](https://codecov.io/gh/knit-sh/knit-graph)

A standalone C program that accepts a read-only [Cypher](https://opencypher.org/) statement,
translates it to SQL, and runs it against a SQLite **provenance** database from the Knit framework — so graph-shaped
questions can be asked in graph syntax while all storage and execution stay in SQLite.

knit-graph is inspired by [graphqlite](https://github.com/dpapathanasiou/graphqlite): it
reuses the ideas — a Cypher→SQL transpiler pipeline, backtick-quoted labels, an `--explain` mode —
but is an independent implementation with its own compact parser and a transformer specialised to the
fixed provenance schema below.

## The database schema

knit-graph targets one specific shape of SQLite database:

- **Node tables** — one per function, named e.g. `` `ns:f` `` (namespaces separated by colons). The
  columns are the function's arguments and return values, plus an `id` column holding a uuid7 that
  identifies a single call.
- **Edge table** — a single `__provenance__` table with columns `source_id, source_name, target_id,
  target_name, edge_type, start_time, end_time, alias`. One row per relationship (e.g. `f` *calls*
  `g`): `*_name` is the peer's table name, `*_id` its uuid, and `alias` disambiguates repeated calls
  (`NULL` by default).

The database is always opened **read-only**; write Cypher clauses are rejected.

## Building

## Building from git

```sh
autoreconf -i
mkdir build && cd build
../configure
make
```

Build dependencies: gcc, autoconf, automake, bison, flex, and the sqlite3 C library. No libtool
(knit-graph is a standalone binary). These apply to the **git checkout**, where the parser/scanner
are regenerated from `cypher_parser.y` / `cypher_scanner.l`.

### Building from a release tarball

A `make dist` tarball already contains the generated parser and scanner (`cypher_parser.c`,
`cypher_parser.h`, `cypher_scanner.c`), so building it needs **only gcc and `libsqlite3-dev`** —
autotools, bison, and flex are *not* required:

```sh
tar xzf knit-graph-0.1.0.tar.gz && cd knit-graph-0.1.0
mkdir build && cd build
../configure
make
make install
```

### Building against a non-default SQLite

By default the build uses the SQLite header and library on the compiler's search
paths (a system-wide `libsqlite3-dev`). To build against a SQLite installed under
a custom prefix instead, pass `--with-sqlite3=DIR`:

```sh
../configure --with-sqlite3=/opt/sqlite
```

This adds `DIR/include` to the header search path and `DIR/lib` to the library
search path, and records an rpath to `DIR/lib` so the resulting binary finds a
shared `libsqlite3` there at runtime. Knit uses this to build knit-graph against
its own private SQLite (`.knit/sqlite`).

## Usage

```
knit-graph [OUTPUT-OPTIONS] [NAME-OPTIONS] DBFILE 'CYPHER'
knit-graph --ast 'CYPHER'
knit-graph [NAME-OPTIONS] --explain DBFILE 'CYPHER'
knit-graph --catalog DBFILE [TABLE[.COLUMN]]
```

Modes:

| Mode        | Effect                                                                       |
|-------------|------------------------------------------------------------------------------|
| (default)   | translate to SQL, run it against `DBFILE`, print the result set              |
| `--ast`     | parse only and print the syntax tree (no database needed)                    |
| `--explain` | translate to SQL and print it, without executing                            |
| `--catalog` | list the database's tables and columns; with `TABLE`/`TABLE.COLUMN`, validate that reference |
| `-h`, `--help` | show usage                                                                |

### Output options

Mirroring the sqlite3 CLI (default is `-list`):

```
-ascii  -box  -column  -csv  -html  -json  -line  -list  -markdown  -table  -tabs
-header / -noheader        show or hide the column-name header (default: header on)
-separator SEP             column separator (list/csv/tabs/ascii modes)
-newline SEP               row separator    (list/csv/tabs/ascii modes)
```

Each mode reproduces the sqlite3 shell's formatting byte-for-byte for the equivalent query; the test
suite pins this by diffing against the installed `sqlite3` (see below). Two intentional differences:
the header is **on** by default (the sqlite3 CLI defaults to off), and an empty result set prints
nothing in every mode (matching the sqlite3 shell).

### Label resolution (name map)

A Cypher node label serves two purposes against the provenance schema: it names the table to JOIN
and it supplies the value a `source_name`/`target_name` edge filter matches. knit-graph assumes both
equal the label, which holds for a plain function table. But a Knit *override* command records its
command name in `*_name` while its rows live in a differently named table (e.g. the `submit` command
writes `jobs`). A **name map** bridges the two — each entry pairs a table name with the recorded name
its edges carry — and is read both ways, so a label may be written as either spelling:

```
--names SPEC          map entries `table=name`, separated by newlines or `;`
--names-file FILE      read the same map from FILE
--derive-table-names   with no map, derive it from the data (uuid-exact)
```

With a map of `jobs=submit`, both `(:jobs)` and `(:submit)` resolve to a JOIN on `jobs` filtered by
`*_name = 'submit'`. A label that is a table name for one command and a command name for a *different*
command is genuinely ambiguous and is reported as an error rather than guessed. A label absent from
the map (or when no map is given) resolves to itself, so the default behaviour is unchanged. The map
options apply to the default run and `--explain`; `knit query` in Knit builds this map live from the
experiment's registered commands and passes it on every invocation.

### Examples

```sh
# The five most-called ns2:g targets, as JSON.
knit-graph -json prov.db \
  "MATCH (a:\`ns:f\`)-[:calls]->(b:\`ns2:g\`) RETURN b.id, count(*) AS n ORDER BY n DESC LIMIT 5"

# A whole node expands to a JSON object over its columns.
knit-graph prov.db "MATCH (a:\`ns:f\`) RETURN a"

# Untyped edge (-->): match a relationship of ANY type. The generated SQL simply
# omits the edge_type filter (compare with the -[:calls]-> example above).
knit-graph prov.db "MATCH (a:\`ns:f\`)-->(b:\`ns2:g\`) RETURN b.id, count(*) AS n ORDER BY n DESC LIMIT 5"

# See the generated SQL without touching the database.
knit-graph --explain prov.db "MATCH (a:\`ns:f\`)-[:calls*1..3]->(b:\`ns2:g\`) RETURN b.id"
```

## Supported Cypher (read subset)

knit-graph is specifically designed for the needs of the Knit framework, hence it only implements a subset of Cypher that it needs (namely, read operations).

- `MATCH` / `WHERE` / `RETURN`, `ORDER BY`, `SKIP`, `LIMIT`, `DISTINCT`
- Patterns: single node, relationship with direction (`->`, `<-`) or undirected (`--`), multi-hop
  chains, comma-separated patterns, and variable-length paths (`-[:calls*1..3]->`, `*`, `*m..`),
  compiled to a recursive CTE. The relationship type is optional — `-->` (or `-[r]->`) matches an
  edge of any type, dropping the `edge_type` filter from the generated SQL
- `WHERE`: `= <> < > <= >=`, `AND` / `OR` / `NOT`, `IN […]`, `IS [NOT] NULL`,
  `STARTS WITH` / `ENDS WITH` / `CONTAINS` (→ `LIKE`)
- Aggregation: `count`, `collect` (→ `json_group_array`), `sum`, `avg`, `min`, `max`, with implicit
  `GROUP BY` on the non-aggregated `RETURN` items
- `RETURN a` / `RETURN r` expand a whole node/relationship to a `json_object(...)` of its columns
- Inline relationship property maps: `-[{alias:'fast'}]->` lowers to the same `edge.alias = 'fast'`
  predicate as the `WHERE` form (each key must be an edge column and each value a literal). Supported
  on directed and undirected single hops; not on variable-length relationships

Write clauses (`CREATE`, `MERGE`, `SET`, `DELETE`, `REMOVE`, `FOREACH`, `LOAD CSV`, …) are rejected
with a clear error and a nonzero exit.

## Testing

```sh
make check        # from the build directory; runs the automake TESTS
```

The suite (in `tests/`) covers parsing (valid/invalid batteries, AST golden files), the catalog,
`--explain` translation, end-to-end execution, all output modes (diffed against the installed
`sqlite3` CLI), and a `valgrind` leak check. Tests needing the `sqlite3` CLI or `valgrind` are
skipped gracefully when those tools are absent.

## Coverage

Configure with `--enable-coverage` to build instrumented objects:

```sh
mkdir build && cd build && ../configure --enable-coverage && make && make check
```

In CI, the [Code coverage workflow](.github/workflows/coverage.yml) runs `coverage.sh` to produce an
`lcov` report and uploads that `.info` file to [Codecov](https://codecov.io) (rather than relying on
Codecov's own gcov discovery, which does not handle this out-of-tree autotools build). The generated
bison/flex sources are excluded from the report; [`codecov.yml`](codecov.yml) keeps a redundant
exclusion and enforces the project/patch targets there.

The same `coverage.sh` gives a quick local report without Codecov: it captures line coverage with
`lcov`, applies those exclusions, prints a summary, writes a browsable report to
`BUILDDIR/coverage-html/` (when `genhtml` is present), and fails below a threshold:

```sh
./coverage.sh build 80                 # BUILDDIR THRESHOLD%
```

## License

MIT — see [LICENSE](LICENSE).
