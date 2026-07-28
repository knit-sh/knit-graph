# knit-graph

A standalone C program that accepts a read-only [Cypher](https://opencypher.org/) statement,
translates it to SQL, and runs it against a SQLite **provenance** database from the Knit framework — so graph-shaped
questions can be asked in graph syntax while all storage and execution stay in SQLite.

- Design / specification: [specifications.md](specifications.md)
- Milestones: [milestones.md](milestones.md)

knit-graph is inspired by [graphqlite](https://github.com/dpapathanasiou/graphqlite) (MIT): it
reuses the ideas — a Cypher→SQL transpiler pipeline, backtick-quoted labels, an `--explain` mode —
but is an independent implementation with its own compact parser and a transformer specialised to the
fixed provenance schema below. See [LICENSE](LICENSE).

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

## Usage

```
knit-graph [OUTPUT-OPTIONS] DBFILE 'CYPHER'
knit-graph --ast 'CYPHER'
knit-graph --explain DBFILE 'CYPHER'
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

In CI, the [Code coverage workflow](.github/workflows/coverage.yml) then uploads the result to
[Codecov](https://codecov.io); the generated bison/flex sources are excluded via
[`codecov.yml`](codecov.yml), and the project/patch targets are enforced there.

For a quick local report without Codecov, `coverage.sh` captures line coverage with `lcov`, applies
the same exclusions, prints a summary, writes a browsable report to `BUILDDIR/coverage-html/` (when
`genhtml` is present), and fails below a threshold:

```sh
./coverage.sh build 80                 # BUILDDIR THRESHOLD%
```

## License

MIT — see [LICENSE](LICENSE).
