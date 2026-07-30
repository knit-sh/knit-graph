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

# expectm MAP QUERY EXPECTED-SQL: like expect, but the query is translated with
# the name map MAP (--names). rejectm MAP QUERY: like reject, with a map.
expectm() {
	got=$("$KG" --explain --names "$1" "$db" "$2" 2>&1)
	if [ "$got" != "$3" ]; then
		echo "FAIL: $2"
		echo "  expected: $3"
		echo "  got:      $got"
		fail=1
	fi
}
rejectm() {
	if "$KG" --explain --names "$1" "$db" "$2" >/dev/null 2>&1; then
		echo "FAIL: should have been rejected: $2"
		fail=1
	fi
}

# expecth QUERY <<'EOF' ... EOF : like expect, but the expected SQL is read from
# a quoted heredoc, sparing the variable-length CTE's many single quotes from the
# '"'"' escaping dance. Command substitution trims the heredoc's trailing
# newline, matching the trim applied to the captured output.
expecth() {
	exp=$(cat)
	got=$("$KG" --explain "$db" "$1" 2>&1)
	if [ "$got" != "$exp" ]; then
		echo "FAIL: $1"
		echo "  expected: $exp"
		echo "  got:      $got"
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

# Variable-length paths -> recursive CTE (M8). The walk carries the fixed
# source, the current frontier, a depth and the set of edges used (edge rowids,
# for relationship-uniqueness / termination). The outer query treats the walk
# like one edge instance, so endpoint joins, label predicates and the RETURN
# tail are the same as elsewhere.
# Bounded *1..3: the upper bound is a `depth <` guard inside the recursion.
expecth 'MATCH (a:`ns:f`)-[:calls*1..3]->(b:`ns2:g`) RETURN a.id, b.id' <<'EOF'
WITH RECURSIVE "walk"(source_id, source_name, target_id, target_name, depth, path) AS (SELECT e."source_id", e."source_name", e."target_id", e."target_name", 1, '/' || e.rowid || '/' FROM "__provenance__" e WHERE e."edge_type" = 'calls' UNION ALL SELECT w."source_id", w."source_name", e."target_id", e."target_name", w."depth" + 1, w."path" || e.rowid || '/' FROM "walk" w JOIN "__provenance__" e ON e."source_id" = w."target_id" AND e."source_name" = w."target_name" WHERE e."edge_type" = 'calls' AND w."depth" < 3 AND w."path" NOT LIKE '%/' || e.rowid || '/%') SELECT a."id", b."id" FROM "walk" r JOIN "ns:f" a ON a."id" = r."source_id" JOIN "ns2:g" b ON b."id" = r."target_id" WHERE r."source_name" = 'ns:f' AND r."target_name" = 'ns2:g'
EOF
# Unbounded *: no depth guard; termination rests on edge-uniqueness alone.
expecth 'MATCH (a:`ns:f`)-[:calls*]->(b:`ns2:g`) RETURN b.id' <<'EOF'
WITH RECURSIVE "walk"(source_id, source_name, target_id, target_name, depth, path) AS (SELECT e."source_id", e."source_name", e."target_id", e."target_name", 1, '/' || e.rowid || '/' FROM "__provenance__" e WHERE e."edge_type" = 'calls' UNION ALL SELECT w."source_id", w."source_name", e."target_id", e."target_name", w."depth" + 1, w."path" || e.rowid || '/' FROM "walk" w JOIN "__provenance__" e ON e."source_id" = w."target_id" AND e."source_name" = w."target_name" WHERE e."edge_type" = 'calls' AND w."path" NOT LIKE '%/' || e.rowid || '/%') SELECT b."id" FROM "walk" r JOIN "ns2:g" b ON b."id" = r."target_id" WHERE r."source_name" = 'ns:f' AND r."target_name" = 'ns2:g'
EOF
# Reversed '<-' with a lower bound *2..4: the endpoints map to the opposite
# edge sides, and the lower bound becomes a `depth >=` filter on the outer query.
expecth 'MATCH (a:`ns:f`)<-[:calls*2..4]-(b:`ns2:g`) RETURN a.id' <<'EOF'
WITH RECURSIVE "walk"(source_id, source_name, target_id, target_name, depth, path) AS (SELECT e."source_id", e."source_name", e."target_id", e."target_name", 1, '/' || e.rowid || '/' FROM "__provenance__" e WHERE e."edge_type" = 'calls' UNION ALL SELECT w."source_id", w."source_name", e."target_id", e."target_name", w."depth" + 1, w."path" || e.rowid || '/' FROM "walk" w JOIN "__provenance__" e ON e."source_id" = w."target_id" AND e."source_name" = w."target_name" WHERE e."edge_type" = 'calls' AND w."depth" < 4 AND w."path" NOT LIKE '%/' || e.rowid || '/%') SELECT a."id" FROM "walk" r JOIN "ns:f" a ON a."id" = r."target_id" WHERE r."source_name" = 'ns2:g' AND r."target_name" = 'ns:f' AND r."depth" >= 2
EOF
# The RETURN tail (WHERE + ORDER BY) composes with a var-length hop unchanged.
expecth 'MATCH (a:`ns:f`)-[:chain*..2]->(b:`ns:f`) WHERE a.id = "f1" RETURN b.id ORDER BY b.id' <<'EOF'
WITH RECURSIVE "walk"(source_id, source_name, target_id, target_name, depth, path) AS (SELECT e."source_id", e."source_name", e."target_id", e."target_name", 1, '/' || e.rowid || '/' FROM "__provenance__" e WHERE e."edge_type" = 'chain' UNION ALL SELECT w."source_id", w."source_name", e."target_id", e."target_name", w."depth" + 1, w."path" || e.rowid || '/' FROM "walk" w JOIN "__provenance__" e ON e."source_id" = w."target_id" AND e."source_name" = w."target_name" WHERE e."edge_type" = 'chain' AND w."depth" < 2 AND w."path" NOT LIKE '%/' || e.rowid || '/%') SELECT b."id" FROM "walk" r JOIN "ns:f" a ON a."id" = r."source_id" JOIN "ns:f" b ON b."id" = r."target_id" WHERE r."source_name" = 'ns:f' AND r."target_name" = 'ns:f' AND a."id" = 'f1' ORDER BY b."id"
EOF

# Whole node / relationship in RETURN (M9): a bare variable expands to a
# json_object over the entity's catalog columns (schema order), aliased to the
# variable name. A node's table is joined in; a relationship is the edge already
# in the FROM.
expecth 'MATCH (a:`ns:f`) RETURN a' <<'EOF'
SELECT json_object('id', a."id", 'x', a."x", 'y', a."y") AS "a" FROM "ns:f" a
EOF
expecth 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN r' <<'EOF'
SELECT json_object('source_id', r."source_id", 'source_name', r."source_name", 'target_id', r."target_id", 'target_name', r."target_name", 'edge_type', r."edge_type", 'start_time', r."start_time", 'end_time', r."end_time", 'alias', r."alias") AS "r" FROM "__provenance__" r WHERE r."edge_type" = 'calls' AND r."source_name" = 'ns:f' AND r."target_name" = 'ns2:g'
EOF
# A whole node alongside a property: the entity pulls its table in like any ref.
expecth 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN a.id, b' <<'EOF'
SELECT a."id", json_object('id', b."id", 'z', b."z") AS "b" FROM "__provenance__" r JOIN "ns:f" a ON a."id" = r."source_id" JOIN "ns2:g" b ON b."id" = r."target_id" WHERE r."edge_type" = 'calls' AND r."source_name" = 'ns:f' AND r."target_name" = 'ns2:g'
EOF
# An explicit alias overrides the variable-name default.
expecth 'MATCH (a:`ns:f`) RETURN a AS node' <<'EOF'
SELECT json_object('id', a."id", 'x', a."x", 'y', a."y") AS "node" FROM "ns:f" a
EOF

# M9 errors.
reject 'MATCH (a) RETURN a'                    # whole node without a label
reject 'MATCH (a:`ns:f`) RETURN b'             # unknown variable

# M8 errors.
reject 'MATCH (a:`ns:f`)-[:calls*0..2]->(b:`ns:f`) RETURN b.id'          # lower bound < 1
reject 'MATCH (a:`ns:f`)-[:calls*3..2]->(b:`ns:f`) RETURN b.id'          # lower > upper
reject 'MATCH (a:`ns:f`)-[r:calls*1..2]->(b:`ns:f`) RETURN b.id'         # bound rel variable
reject 'MATCH (a:`ns:f`)-[:calls*1..2]-(b:`ns:f`) RETURN b.id'           # undirected var-length
reject 'MATCH (a:`ns:f`)-[:calls*1..2]->(b:`ns2:g`)-[:wraps]->(c) RETURN b.id'  # var-length not sole hop

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

# Name map: a label may be written as either a table name or the command name
# its edges record. The map here gives table "ns:f" the recorded name "fnode".
# The command-name spelling joins the table but filters on the recorded name...
expectm 'ns:f=fnode' 'MATCH (a:fnode)-[:calls]->(b:`ns2:g`) RETURN b.id' \
	'SELECT b."id" FROM "__provenance__" r JOIN "ns2:g" b ON b."id" = r."target_id" WHERE r."edge_type" = '"'"'calls'"'"' AND r."source_name" = '"'"'fnode'"'"' AND r."target_name" = '"'"'ns2:g'"'"''
# ...and the table-name spelling resolves to the very same SQL.
expectm 'ns:f=fnode' 'MATCH (a:`ns:f`)-[:calls]->(b:`ns2:g`) RETURN b.id' \
	'SELECT b."id" FROM "__provenance__" r JOIN "ns2:g" b ON b."id" = r."target_id" WHERE r."edge_type" = '"'"'calls'"'"' AND r."source_name" = '"'"'fnode'"'"' AND r."target_name" = '"'"'ns2:g'"'"''
# A label absent from the map resolves to itself (no map at all is the same).
expectm 'ns:f=fnode' 'MATCH (a:`ns2:g`) RETURN a.z' \
	'SELECT a."z" FROM "ns2:g" a'
# A label that is a table name in one entry and a command name in another is
# genuinely ambiguous and must be rejected, not guessed.
rejectm 'ns:f=fnode
x=ns:f' 'MATCH (a:`ns:f`) RETURN a.x'

# Inline relationship property maps lower to the same edge predicate the WHERE
# form (e.alias = 'fast') produces -- ANDed onto the pattern predicates.
expect 'MATCH (a:`ns:f`)-[{alias:"fast"}]->(b:`ns2:g`) RETURN b.id' \
	'SELECT b."id" FROM "__provenance__" r JOIN "ns2:g" b ON b."id" = r."target_id" WHERE r."source_name" = '"'"'ns:f'"'"' AND r."target_name" = '"'"'ns2:g'"'"' AND r."alias" = '"'"'fast'"'"''
# Alongside an edge type.
expect 'MATCH (a:`ns:f`)-[:calls {alias:"fast"}]->(b:`ns2:g`) RETURN b.id' \
	'SELECT b."id" FROM "__provenance__" r JOIN "ns2:g" b ON b."id" = r."target_id" WHERE r."edge_type" = '"'"'calls'"'"' AND r."source_name" = '"'"'ns:f'"'"' AND r."target_name" = '"'"'ns2:g'"'"' AND r."alias" = '"'"'fast'"'"''
# Undirected hop: the alias predicate applies to the edge row regardless of the
# orientation, so it is ANDed after the OR over both orientations.
expect 'MATCH (a:`ns:f`)-[{alias:"fast"}]-(b:`ns2:g`) RETURN b.id' \
	'SELECT b."id" FROM "__provenance__" r JOIN "ns2:g" b ON 1 = 1 WHERE (r."source_name" = '"'"'ns:f'"'"' AND r."target_id" = b."id" AND r."target_name" = '"'"'ns2:g'"'"' OR r."target_name" = '"'"'ns:f'"'"' AND r."source_id" = b."id" AND r."source_name" = '"'"'ns2:g'"'"') AND r."alias" = '"'"'fast'"'"''

# Inline property errors.
reject 'MATCH (a:`ns:f`)-[{nope:"x"}]->(b:`ns2:g`) RETURN b.id'       # unknown edge column
reject 'MATCH (a:`ns:f`)-[{alias:a.x}]->(b:`ns2:g`) RETURN b.id'      # non-literal value
reject 'MATCH (a:`ns:f`)-[:chain*1..2 {alias:"x"}]->(b:`ns:f`) RETURN b.id'  # inline on var-length

rm -f "$db"
exit $fail
