#!/bin/sh
# names.sh -- end-to-end tests for the command-name <-> table-name map and the
# inline relationship property maps that fill the `alias` column.
#
# The fixture deliberately gives each override command a table whose name
# differs from the `*_name` its edges record (table `jobs` / name `submit`,
# table `montecarlo` / name `submit:montecarlo`), so the map's two readings are
# actually exercised -- unlike the shared fixture, where table == name. Skipped
# if the sqlite3 CLI is unavailable.

: "${KG:?KG must point at the knit-graph binary}"

command -v sqlite3 >/dev/null 2>&1 || {
	echo "sqlite3 CLI unavailable; skipping names test"
	exit 77
}

db="names_fixture.db"
rm -f "$db"
sqlite3 "$db" <<'SQL'
CREATE TABLE "jobs"       (id TEXT, procs INTEGER);
CREATE TABLE "montecarlo" (id TEXT, result REAL);
CREATE TABLE "setup:libs" (id TEXT);
CREATE TABLE "__provenance__" (
	source_id   TEXT,
	source_name TEXT,
	target_id   TEXT,
	target_name TEXT,
	edge_type   TEXT,
	start_time  REAL,
	end_time    REAL,
	alias       TEXT
);
INSERT INTO "jobs"       VALUES ('j1', 8);
INSERT INTO "montecarlo" VALUES ('m1', 3.14), ('m2', 2.71);
INSERT INTO "setup:libs" VALUES ('s1');
INSERT INTO "__provenance__" VALUES
	-- setup used by the job: source = setup, target = job (name 'submit').
	('s1', 'setup:libs', 'j1', 'submit',            'used_by', NULL, NULL, NULL),
	-- two calls of the same body, told apart by their knit_as alias.
	('j1', 'submit',     'm1', 'submit:montecarlo', 'call',    1.0,  2.0,  'fast'),
	('j1', 'submit',     'm2', 'submit:montecarlo', 'call',    3.0,  4.0,  'slow');
SQL

# The live map knit query would build for this experiment.
MAP='jobs=submit
montecarlo=submit:montecarlo
setup:libs=setup:libs'

fail=0

# expect QUERY EXPECTED: run QUERY with $MAP; stdout must match EXPECTED (\n
# escapes via printf %b, as in exec.sh).
expect() {
	"$KG" --names "$MAP" "$db" "$1" > names_got.out 2>&1
	printf '%b' "$2" > names_exp.out
	if ! diff -u names_exp.out names_got.out; then
		echo "FAIL: $1"
		fail=1
	fi
}

# reject ARGS...: the invocation must fail with a non-zero exit.
reject() {
	if "$KG" "$@" >/dev/null 2>&1; then
		echo "FAIL: should have been rejected: $*"
		fail=1
	fi
}

# A label written as the command name joins the table and filters on the name.
expect 'MATCH (j:submit) RETURN j.procs' 'procs\n8\n'
# The table-name spelling returns the same row.
expect 'MATCH (j:jobs) RETURN j.procs' 'procs\n8\n'

# Inline alias singles out one of the two otherwise-identical calls.
expect "MATCH (j:submit)-[{alias:'fast'}]->(m:montecarlo) RETURN m.result" \
	'result\n3.14\n'
expect "MATCH (j:submit)-[{alias:'slow'}]->(m:montecarlo) RETURN m.result" \
	'result\n2.71\n'
# The equivalent WHERE form yields the same row (inline lowers to it).
expect "MATCH (j:submit)-[e]->(m:montecarlo) WHERE e.alias = 'fast' RETURN m.result" \
	'result\n3.14\n'

# used_by hop from the setup (source) to the job it is used by (target). The
# dispatcher command name `submit` reads better than the table name `jobs`.
expect 'MATCH (s:`setup:libs`)-[:used_by]->(j:submit) RETURN j.procs' 'procs\n8\n'

# --names-file supplies the same map from a file.
printf '%s\n' "$MAP" > names_map.txt
"$KG" --names-file names_map.txt "$db" 'MATCH (j:submit) RETURN j.procs' \
	> names_got.out 2>&1
printf '%b' 'procs\n8\n' > names_exp.out
if ! diff -u names_exp.out names_got.out; then
	echo "FAIL: --names-file"
	fail=1
fi

# --derive-table-names reconstructs the map from the data (no map supplied): the
# name 'submit' is resolved to the table `jobs` that holds its id.
"$KG" --derive-table-names "$db" 'MATCH (j:submit) RETURN j.procs' \
	> names_got.out 2>&1
printf '%b' 'procs\n8\n' > names_exp.out
if ! diff -u names_exp.out names_got.out; then
	echo "FAIL: --derive-table-names"
	fail=1
fi

# Errors.
reject --names 'jobs=submit
x=jobs' "$db" 'MATCH (j:jobs) RETURN j.procs'      # ambiguous label
reject --names 'no-equals-sign' "$db" 'MATCH (j:jobs) RETURN j.procs'  # bad spec
reject --names '=submit' "$db" 'MATCH (j:jobs) RETURN j.procs'         # empty table
reject --names-file no_such_file "$db" 'MATCH (j:jobs) RETURN j.procs' # missing file
reject --names 'both=one' --names-file names_map.txt "$db" \
	'MATCH (j:jobs) RETURN j.procs'                                # both map sources
reject --names 'ghost=phantom' "$db" 'MATCH (a:phantom) RETURN a.id'   # maps to no table
reject --names "$MAP" "$db" \
	"MATCH (j:submit)-[{nope:'x'}]->(m:montecarlo) RETURN m.result" # bad edge column

rm -f "$db" names_got.out names_exp.out names_map.txt
exit $fail
