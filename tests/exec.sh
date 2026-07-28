#!/bin/sh
# exec.sh -- end-to-end integration test (M4). Run the M3-level query shapes
# against the fixture database and diff the default "-list" output (pipe-
# separated fields, header row, NULL rendered as empty). Skipped if the
# sqlite3 CLI is unavailable.

: "${KG:?KG must point at the knit-graph binary}"
: "${srcdir:?srcdir must be set}"

. "$srcdir/fixture.sh"

db="exec_fixture.db"
if ! make_fixture "$db"; then
	echo "sqlite3 CLI unavailable; skipping exec test"
	exit 77
fi

fail=0

# expect QUERY EXPECTED: full stdout must match EXPECTED byte for byte. EXPECTED
# uses \n escapes (interpreted via printf %b) so trailing blank lines -- e.g. a
# NULL field's row -- are compared exactly, which command substitution would drop.
expect() {
	"$KG" "$db" "$1" > exec_got.out 2>&1
	printf '%b' "$2" > exec_exp.out
	if ! diff -u exec_exp.out exec_got.out; then
		echo "FAIL: $1"
		fail=1
	fi
}

# reject QUERY: execution must fail with a non-zero exit.
reject() {
	if "$KG" "$db" "$1" >/dev/null 2>&1; then
		echo "FAIL: should have been rejected: $1"
		fail=1
	fi
}

# Single node: header is the bare column name; two rows.
expect 'MATCH (a:`ns:f`) RETURN a.x' 'x\n1\n2\n'

# Aliases and a second column.
expect 'MATCH (a:`ns:f`) RETURN a.x AS ex, a.y' 'ex|y\n1|a\n2|b\n'

# Relationship: the single edge row joins both endpoints.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN a.id, b.id' 'id|id\nf1|g1\n'

# A NULL field (alias is NULL in the fixture) renders as an empty field.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN r.alias' 'alias\n\n'

# Reversed direction matches nothing: like the sqlite3 shell, an empty result
# prints nothing at all -- not even a header row.
expect 'MATCH (a:`ns:f`)<-[r:calls]-(b:`ns2:g`) RETURN b.id' ''

# WHERE (M5): filter the fixture rows.
expect 'MATCH (a:`ns:f`) WHERE a.x = 2 RETURN a.x, a.y' 'x|y\n2|b\n'
expect 'MATCH (a:`ns:f`) WHERE a.x > 1 RETURN a.id' 'id\nf2\n'
expect 'MATCH (a:`ns:f`) WHERE NOT a.x = 1 RETURN a.id' 'id\nf2\n'
expect 'MATCH (a:`ns:f`) WHERE a.x IN [1] RETURN a.id' 'id\nf1\n'
expect 'MATCH (a:`ns:f`) WHERE a.y STARTS WITH "a" RETURN a.id' 'id\nf1\n'
# A NULL relationship property tested with IS NULL still matches the edge.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) WHERE r.alias IS NULL RETURN a.id, b.id' 'id|id\nf1|g1\n'
# A WHERE reference to b joins it in; z=1.5 fails z>2.0, so nothing is emitted.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) WHERE b.z > 2.0 RETURN a.id' ''

# Chains, undirected & multi-pattern (M6).
# Two-hop chain f1 -calls-> g1 -wraps-> f2.
expect 'MATCH (a:`ns:f`)-[r1:calls]->(b:`ns2:g`)-[r2:wraps]->(c:`ns:f`) RETURN a.id, c.id' 'id|id\nf1|f2\n'
# Undirected finds the calls edge regardless of the query's direction: the edge
# is ns:f -> ns2:g, yet querying ns2:g--ns:f still matches it.
expect 'MATCH (a:`ns2:g`)-[r:calls]-(b:`ns:f`) RETURN a.id, b.id' 'id|id\ng1|f1\n'
# The directed form of the same query matches nothing, so emits nothing.
expect 'MATCH (a:`ns2:g`)-[r:calls]->(b:`ns:f`) RETURN a.id, b.id' ''
# Comma-separated patterns cross-join: two ns:f rows x one ns2:g row.
expect 'MATCH (a:`ns:f`), (b:`ns2:g`) RETURN a.id, b.id' 'id|id\nf1|g1\nf2|g1\n'

# Aggregation, DISTINCT, ORDER BY, SKIP, LIMIT (M7).
# count(*) over the two ns:f rows.
expect 'MATCH (a:`ns:f`) RETURN count(*) AS n' 'n\n2\n'
# Implicit GROUP BY a.y: one row per distinct y.
expect 'MATCH (a:`ns:f`) RETURN a.y, count(*) AS n ORDER BY a.y' 'y|n\na|1\nb|1\n'
# collect -> a JSON array of the ids.
expect 'MATCH (a:`ns:f`) RETURN collect(a.id) AS ids' 'ids\n["f1","f2"]\n'
# sum/min/max/avg over a.x (1 and 2).
expect 'MATCH (a:`ns:f`) RETURN sum(a.x) AS s, min(a.x) AS mn, max(a.x) AS mx, avg(a.x) AS av' 's|mn|mx|av\n3|1|2|1.5\n'
# ORDER BY ... DESC + LIMIT keeps only the largest.
expect 'MATCH (a:`ns:f`) RETURN a.x ORDER BY a.x DESC LIMIT 1' 'x\n2\n'
# SKIP drops the first ordered row.
expect 'MATCH (a:`ns:f`) RETURN a.x ORDER BY a.x SKIP 1' 'x\n2\n'
# DISTINCT collapses the cross-join's two g1 rows into one.
expect 'MATCH (a:`ns:f`), (b:`ns2:g`) RETURN DISTINCT b.id' 'id\ng1\n'

# Variable-length paths -> recursive CTE (M8). The fixture's 'chain' edges form
# a 2-cycle f1 -> f2 -> f1; walks start at f1 (pinned via WHERE) over ns:f.
# *1..1: only the depth-1 neighbour.
expect 'MATCH (a:`ns:f`)-[:chain*1..1]->(b:`ns:f`) WHERE a.id = "f1" RETURN b.id' 'id\nf2\n'
# *1..2: f2 at depth 1 and f1 at depth 2 (the cycle closes).
expect 'MATCH (a:`ns:f`)-[:chain*1..2]->(b:`ns:f`) WHERE a.id = "f1" RETURN b.id ORDER BY b.id' 'id\nf1\nf2\n'
# *2..2: the lower bound drops the depth-1 row, leaving only f1.
expect 'MATCH (a:`ns:f`)-[:chain*2..2]->(b:`ns:f`) WHERE a.id = "f1" RETURN b.id' 'id\nf1\n'
# Unbounded *: edge-uniqueness terminates the walk on the cycle (f2, then f1).
expect 'MATCH (a:`ns:f`)-[:chain*]->(b:`ns:f`) WHERE a.id = "f1" RETURN b.id ORDER BY b.id' 'id\nf1\nf2\n'
# A var-length hop composes with aggregation: two nodes reachable within 2 hops.
expect 'MATCH (a:`ns:f`)-[:chain*1..2]->(b:`ns:f`) WHERE a.id = "f1" RETURN count(*) AS reachable' 'reachable\n2\n'

# Whole node / relationship in RETURN (M9): a bare variable renders as a JSON
# object of the entity's columns, keyed by column name, in schema order.
expect 'MATCH (a:`ns:f`) RETURN a' \
	'a\n{"id":"f1","x":1,"y":"a"}\n{"id":"f2","x":2,"y":"b"}\n'
# A whole relationship: the edge row, with the NULL alias rendered as JSON null.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN r' \
	'r\n{"source_id":"f1","source_name":"ns:f","target_id":"g1","target_name":"ns2:g","edge_type":"calls","start_time":0.0,"end_time":1.0,"alias":null}\n'
# A whole node alongside a scalar: b expands to JSON, a.id stays a plain field.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN a.id, b' \
	'id|b\nf1|{"id":"g1","z":1.5}\n'

# Errors still propagate on the execution path (unknown column, write clause,
# unknown column inside WHERE).
reject 'MATCH (a:`ns:f`) RETURN a.nope'
reject 'MATCH (a:`ns:f`) WHERE a.nope = 1 RETURN a.id'
reject 'CREATE (a:`ns:f`) RETURN a.x'

rm -f "$db" exec_got.out exec_exp.out
exit $fail
