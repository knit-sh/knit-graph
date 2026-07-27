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

# WHERE (M5): boolean expressions ANDed onto the pattern predicates.
# Comparisons and AND (parenthesised).
expect 'MATCH (a:`ns:f`) WHERE a.x >= 1 AND a.x < 5 RETURN a.x' \
	'SELECT a."x" FROM "ns:f" a WHERE (a."x" >= 1 AND a."x" < 5)'
# OR / NOT.
expect 'MATCH (a:`ns:f`) WHERE a.x = 1 OR a.x = 2 RETURN a.x' \
	'SELECT a."x" FROM "ns:f" a WHERE (a."x" = 1 OR a."x" = 2)'
expect 'MATCH (a:`ns:f`) WHERE NOT a.x = 2 RETURN a.x' \
	'SELECT a."x" FROM "ns:f" a WHERE (NOT a."x" = 2)'
# IN and IS [NOT] NULL.
expect 'MATCH (a:`ns:f`) WHERE a.x IN [1, 2, 3] RETURN a.x' \
	'SELECT a."x" FROM "ns:f" a WHERE a."x" IN (1, 2, 3)'
expect 'MATCH (a:`ns:f`) WHERE a.y IS NOT NULL RETURN a.y' \
	'SELECT a."y" FROM "ns:f" a WHERE a."y" IS NOT NULL'
# STARTS WITH / CONTAINS -> LIKE with a backslash escape; metacharacters in the
# literal are escaped so only the added wildcards match.
expect 'MATCH (a:`ns:f`) WHERE a.y STARTS WITH "a" RETURN a.y' \
	'SELECT a."y" FROM "ns:f" a WHERE a."y" LIKE '"'"'a%'"'"' ESCAPE '"'"'\'"'"''
expect 'MATCH (a:`ns:f`) WHERE a.y CONTAINS "a_b" RETURN a.y' \
	'SELECT a."y" FROM "ns:f" a WHERE a."y" LIKE '"'"'%a\_b%'"'"' ESCAPE '"'"'\'"'"''
# A WHERE reference to a node forces that node's table to be joined in, even when
# only another entity is returned.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) WHERE r.alias IS NULL RETURN a.id' \
	'SELECT a."id" FROM "__provenance__" r JOIN "ns:f" a ON a."id" = r."source_id" WHERE r."edge_type" = '"'"'calls'"'"' AND r."source_name" = '"'"'ns:f'"'"' AND r."target_name" = '"'"'ns2:g'"'"' AND r."alias" IS NULL'
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) WHERE b.z > 1 RETURN a.id' \
	'SELECT a."id" FROM "__provenance__" r JOIN "ns:f" a ON a."id" = r."source_id" JOIN "ns2:g" b ON b."id" = r."target_id" WHERE r."edge_type" = '"'"'calls'"'"' AND r."source_name" = '"'"'ns:f'"'"' AND r."target_name" = '"'"'ns2:g'"'"' AND b."z" > 1'

# Chains, undirected & multi-pattern MATCH (M6).
# A two-hop chain: the second edge JOINs __provenance__ and ties its source to
# the first edge's target (the shared node b); each returned node joins its table.
expect 'MATCH (a:`ns:f`)-[r1:calls]->(b:`ns2:g`)-[r2:wraps]->(c:`ns:f`) RETURN a.id, c.id' \
	'SELECT a."id", c."id" FROM "__provenance__" r1 JOIN "__provenance__" r2 ON r2."source_id" = r1."target_id" AND r2."source_name" = r1."target_name" JOIN "ns:f" a ON a."id" = r1."source_id" JOIN "ns:f" c ON c."id" = r2."target_id" WHERE r1."edge_type" = '"'"'calls'"'"' AND r1."source_name" = '"'"'ns:f'"'"' AND r1."target_name" = '"'"'ns2:g'"'"' AND r2."edge_type" = '"'"'wraps'"'"' AND r2."source_name" = '"'"'ns2:g'"'"' AND r2."target_name" = '"'"'ns:f'"'"''
# An undirected hop: OR over both orientations of the edge.
expect 'MATCH (a:`ns2:g`)-[r:calls]-(b:`ns:f`) RETURN a.id, b.id' \
	'SELECT a."id", b."id" FROM "__provenance__" r JOIN "ns2:g" a ON 1 = 1 JOIN "ns:f" b ON 1 = 1 WHERE r."edge_type" = '"'"'calls'"'"' AND (r."source_id" = a."id" AND r."source_name" = '"'"'ns2:g'"'"' AND r."target_id" = b."id" AND r."target_name" = '"'"'ns:f'"'"' OR r."target_id" = a."id" AND r."target_name" = '"'"'ns2:g'"'"' AND r."source_id" = b."id" AND r."source_name" = '"'"'ns:f'"'"')'
# Comma-separated patterns in one MATCH: a cross product of the two node tables.
expect 'MATCH (a:`ns:f`), (b:`ns2:g`) RETURN a.id, b.id' \
	'SELECT a."id", b."id" FROM "ns:f" a JOIN "ns2:g" b ON 1 = 1'
# A bare `--` (undirected, untyped): only the endpoint labels constrain the OR;
# an unreferenced endpoint contributes just its name, a referenced one its id too.
expect 'MATCH (a:`ns:f`)--(b:`ns2:g`) RETURN a.id' \
	'SELECT a."id" FROM "__provenance__" r JOIN "ns:f" a ON 1 = 1 WHERE (r."source_id" = a."id" AND r."source_name" = '"'"'ns:f'"'"' AND r."target_name" = '"'"'ns2:g'"'"' OR r."target_id" = a."id" AND r."target_name" = '"'"'ns:f'"'"' AND r."source_name" = '"'"'ns2:g'"'"')'

# Aggregation, DISTINCT, ORDER BY, SKIP, LIMIT (M7).
# An aggregate implies GROUP BY the non-aggregated RETURN items.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN b.id, count(*) AS n' \
	'SELECT b."id", count(*) AS "n" FROM "__provenance__" r JOIN "ns2:g" b ON b."id" = r."target_id" WHERE r."edge_type" = '"'"'calls'"'"' AND r."source_name" = '"'"'ns:f'"'"' AND r."target_name" = '"'"'ns2:g'"'"' GROUP BY b."id"'
# count over a column, ORDER BY the aggregate's alias, LIMIT.
expect 'MATCH (a:`ns:f`) RETURN a.y, count(a.x) AS n ORDER BY n DESC LIMIT 5' \
	'SELECT a."y", count(a."x") AS "n" FROM "ns:f" a GROUP BY a."y" ORDER BY "n" DESC LIMIT 5'
# DISTINCT.
expect 'MATCH (a:`ns:f`) RETURN DISTINCT a.y' \
	'SELECT DISTINCT a."y" FROM "ns:f" a'
# collect -> json_group_array; a sole aggregate needs no GROUP BY.
expect 'MATCH (a:`ns:f`) RETURN collect(a.id) AS ids' \
	'SELECT json_group_array(a."id") AS "ids" FROM "ns:f" a'
# count(DISTINCT ...).
expect 'MATCH (a:`ns:f`) RETURN count(DISTINCT a.y) AS n' \
	'SELECT count(DISTINCT a."y") AS "n" FROM "ns:f" a'
# ORDER BY a property, SKIP + LIMIT -> LIMIT ... OFFSET.
expect 'MATCH (a:`ns:f`) RETURN a.x ORDER BY a.x SKIP 1 LIMIT 2' \
	'SELECT a."x" FROM "ns:f" a ORDER BY a."x" LIMIT 2 OFFSET 1'
# SKIP without LIMIT -> LIMIT -1 OFFSET.
expect 'MATCH (a:`ns:f`) RETURN a.x SKIP 1' \
	'SELECT a."x" FROM "ns:f" a LIMIT -1 OFFSET 1'

# M7 errors.
reject 'MATCH (a:`ns:f`) RETURN foo(a.x)'          # unknown aggregate
reject 'MATCH (a:`ns:f`) RETURN sum(a.x, a.y)'     # aggregate arity
reject 'MATCH (a:`ns:f`) RETURN a.x LIMIT a.x'     # non-integer LIMIT
reject 'MATCH (a:`ns:f`) RETURN a.x ORDER BY zzz'  # ORDER BY unknown name

# Undirected is only supported as a single, sole hop for now.
reject 'MATCH (a:`ns:f`)-[:calls]->(b:`ns2:g`)-[:wraps]-(c:`ns:f`) RETURN a.id'  # undirected in a chain

# WHERE resolution/scope errors.
reject 'MATCH (a:`ns:f`) WHERE a.nope = 1 RETURN a.id'         # unknown column
reject 'MATCH (a:`ns:f`) WHERE a.y STARTS WITH a.x RETURN a.y' # needs a literal
reject 'MATCH (a:`ns:f`) WHERE count(a.x) > 1 RETURN a.x'      # aggregate not valid in WHERE

# Resolution errors.
reject 'MATCH (a:`ns:f`) RETURN a.nope'      # unknown column
reject 'MATCH (a:`nope:x`) RETURN a.id'      # unknown table
reject 'MATCH (a) RETURN a.x'                # node has no label

# Not-yet-supported constructs must be rejected, not mistranslated.
reject 'MATCH (a)-[:calls*1..3]->(b) RETURN b.id'    # var-length (M8)
reject 'MATCH (a:`ns:f`) RETURN a'           # whole-node RETURN (M9)

rm -f "$db"
exit $fail
