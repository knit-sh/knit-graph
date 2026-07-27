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

# Reversed direction matches nothing here: header only, no data rows.
expect 'MATCH (a:`ns:f`)<-[r:calls]-(b:`ns2:g`) RETURN b.id' 'id\n'

# WHERE (M5): filter the fixture rows.
expect 'MATCH (a:`ns:f`) WHERE a.x = 2 RETURN a.x, a.y' 'x|y\n2|b\n'
expect 'MATCH (a:`ns:f`) WHERE a.x > 1 RETURN a.id' 'id\nf2\n'
expect 'MATCH (a:`ns:f`) WHERE NOT a.x = 1 RETURN a.id' 'id\nf2\n'
expect 'MATCH (a:`ns:f`) WHERE a.x IN [1] RETURN a.id' 'id\nf1\n'
expect 'MATCH (a:`ns:f`) WHERE a.y STARTS WITH "a" RETURN a.id' 'id\nf1\n'
# A NULL relationship property tested with IS NULL still matches the edge.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) WHERE r.alias IS NULL RETURN a.id, b.id' 'id|id\nf1|g1\n'
# A WHERE reference to b joins it in; z=1.5 fails z>2.0, so no data rows.
expect 'MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) WHERE b.z > 2.0 RETURN a.id' 'id\n'

# Errors still propagate on the execution path (unknown column, write clause,
# unknown column inside WHERE).
reject 'MATCH (a:`ns:f`) RETURN a.nope'
reject 'MATCH (a:`ns:f`) WHERE a.nope = 1 RETURN a.id'
reject 'CREATE (a:`ns:f`) RETURN a.x'

rm -f "$db" exec_got.out exec_exp.out
exit $fail
