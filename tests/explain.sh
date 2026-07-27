#!/bin/sh
# explain.sh -- assert the SQL that --explain generates for the M3 shapes:
# a single node, a single relationship, and a reversed relationship. Also
# checks that label/property resolution and not-yet-supported constructs are
# reported as errors. Skipped if the sqlite3 CLI is unavailable.

: "${KG:?KG must point at the knit-graph binary}"
: "${srcdir:?srcdir must be set}"

. "$srcdir/fixture.sh"

db="explain_fixture.db"
if ! make_fixture "$db"; then
	echo "sqlite3 CLI unavailable; skipping explain test"
	exit 77
fi

fail=0

# expect QUERY EXPECTED-SQL: the generated SQL must match exactly.
expect() {
	got=$("$KG" --explain "$db" "$1" 2>&1)
	if [ "$got" != "$2" ]; then
		echo "FAIL: $1"
		echo "  expected: $2"
		echo "  got:      $got"
		fail=1
	fi
}

# reject QUERY: translation must fail with a non-zero exit.
reject() {
	if "$KG" --explain "$db" "$1" >/dev/null 2>&1; then
		echo "FAIL: should have been rejected: $1"
		fail=1
	fi
}

# Single node.
expect 'MATCH (a:`ns:f`) RETURN a.x' \
	'SELECT a."x" FROM "ns:f" a'
expect 'MATCH (a:`ns:f`) RETURN a.x AS ex, a.y' \
	'SELECT a."x" AS "ex", a."y" FROM "ns:f" a'

# Single relationship: the edge table drives the FROM; the returned node joins
# back; both endpoints contribute source_name/target_name predicates.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN b.id' \
	'SELECT b."id" FROM "__provenance__" r JOIN "ns2:g" b ON b."id" = r."target_id" WHERE r."edge_type" = '"'"'calls'"'"' AND r."source_name" = '"'"'ns:f'"'"' AND r."target_name" = '"'"'ns2:g'"'"''

# Reversed relationship: '<-' swaps the source and target roles.
expect 'MATCH (a:`ns:f`)<-[r:calls]-(b:`ns2:g`) RETURN b.id' \
	'SELECT b."id" FROM "__provenance__" r JOIN "ns2:g" b ON b."id" = r."source_id" WHERE r."edge_type" = '"'"'calls'"'"' AND r."source_name" = '"'"'ns2:g'"'"' AND r."target_name" = '"'"'ns:f'"'"''

# A relationship property resolves to the edge table; no join is needed for it.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN r.alias' \
	'SELECT r."alias" FROM "__provenance__" r WHERE r."edge_type" = '"'"'calls'"'"' AND r."source_name" = '"'"'ns:f'"'"' AND r."target_name" = '"'"'ns2:g'"'"''

# Resolution errors.
reject 'MATCH (a:`ns:f`) RETURN a.nope'      # unknown column
reject 'MATCH (a:`nope:x`) RETURN a.id'      # unknown table
reject 'MATCH (a) RETURN a.x'                # node has no label

# Not-yet-supported constructs must be rejected, not mistranslated.
reject 'MATCH (a) WHERE a.x = 1 RETURN a.x'  # WHERE (M5)
reject 'MATCH (a:`ns:f`)--(b:`ns2:g`) RETURN a.id'   # undirected (M6)
reject 'MATCH (a)-[:calls*1..3]->(b) RETURN b.id'    # var-length (M8)
reject 'MATCH (a:`ns:f`) RETURN a'           # whole-node RETURN (M9)

rm -f "$db"
exit $fail
