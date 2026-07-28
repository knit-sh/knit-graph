#!/bin/sh
# output.sh -- M10 output modes. For each sqlite3 CLI output mode, translate a
# Cypher query with knit-graph and diff its rendered output against the sqlite3
# CLI's output for the equivalent SQL (obtained from --explain). Both sides run
# the same sqlite3 version, so this pins knit-graph's formatting to the shell's.
# Skipped if the sqlite3 CLI is unavailable.

: "${KG:?KG must point at the knit-graph binary}"
: "${srcdir:?srcdir must be set}"

. "$srcdir/fixture.sh"

db="output_fixture.db"
if ! make_fixture "$db"; then
	echo "sqlite3 CLI unavailable; skipping output test"
	exit 77
fi

# A rich node table exercising the formatters' edge cases: a comma and a double
# quote (csv quoting), a backslash (json escaping), markup characters (html
# escaping), a NULL, negative/zero reals, and disparate column widths.
sqlite3 "$db" 'CREATE TABLE "t:x" (id TEXT, val TEXT, num INTEGER, flt REAL)' || exit 1
sqlite3 "$db" "INSERT INTO \"t:x\" VALUES \
	('r1','plain',1,1.5),\
	('r2','a,b\"c\\d<e>&f',22,-3.25),\
	('r3',NULL,333,0.0)" || exit 1

MODES="list ascii csv tabs html json line column table box markdown"

fail=0

# check CYPHER: diff every mode, with and without a header, against sqlite3.
check() {
	sql=$("$KG" --explain "$db" "$1") || { echo "FAIL: --explain: $1"; fail=1; return; }
	for m in $MODES; do
		for h in -header -noheader; do
			"$KG" $h -$m "$db" "$1" > out_kg.out 2>&1
			sqlite3 $h -$m "$db" "$sql" > out_s3.out 2>&1
			if ! diff -u out_s3.out out_kg.out > out_diff.out; then
				echo "FAIL: mode $m $h: $1"
				cat out_diff.out
				fail=1
			fi
		done
	done
}

# A relationship query: text, integer and real columns plus a NULL (r.alias).
check 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN a.id, a.x, b.z, r.alias'
# Whole entities: each row's node expands to one json_object value.
check 'MATCH (a:`ns:f`) RETURN a'
# The rich table: special characters and disparate column widths.
check 'MATCH (a:`t:x`) RETURN a.id, a.val, a.num, a.flt'
# An empty result set: every mode must emit nothing at all.
check 'MATCH (a:`ns:f`)<-[r:calls]-(b:`ns2:g`) RETURN b.id'

# -separator / -newline override the column/row separators. Compared under
# -noheader so only the separators differ from the plain -list default.
sep_check() {
	cy='MATCH (a:`ns:f`) RETURN a.id, a.x'
	sql=$("$KG" --explain "$db" "$cy")
	"$KG" "$@" -noheader -list "$db" "$cy" > out_kg.out 2>&1
	sqlite3 "$@" -noheader -list "$db" "$sql" > out_s3.out 2>&1
	if ! diff -u out_s3.out out_kg.out > out_diff.out; then
		echo "FAIL: separator/newline $*"
		cat out_diff.out
		fail=1
	fi
}
sep_check -separator ';'
sep_check -newline '@'
sep_check -separator '::' -newline '#'

rm -f "$db" out_kg.out out_s3.out out_diff.out
exit $fail
