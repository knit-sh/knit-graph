#!/bin/sh
# memcheck.sh -- run the parse and catalog paths under valgrind.
#
# A definite/indirect leak or any memory error makes valgrind exit with the
# distinctive code 99, which we treat as a failure. Ordinary non-zero exits
# from knit-graph itself (e.g. exit 1 on a rejected query) pass straight
# through valgrind, so the error path is checked for leaks without its exit
# code being mistaken for one. Skipped if valgrind is not installed.

: "${KG:?KG must point at the knit-graph binary}"
: "${srcdir:?srcdir must be set}"

if ! command -v valgrind >/dev/null 2>&1; then
	echo "valgrind unavailable; skipping memory check"
	exit 77
fi

. "$srcdir/fixture.sh"

VG="valgrind -q --leak-check=full --errors-for-leak-kinds=definite,indirect --error-exitcode=99"

fail=0
run() {
	"$@"
	if [ $? -eq 99 ]; then
		echo "FAIL: valgrind reported a leak/error: $*"
		fail=1
	fi
}

# Successful parses.
run $VG "$KG" --ast "MATCH (a:Foo) RETURN a"
run $VG "$KG" --ast "MATCH (a:\`ns:f\`)-[r:calls]->(b:\`ns2:g\`) RETURN b.id, count(*) AS n ORDER BY n DESC LIMIT 5"
run $VG "$KG" --ast "MATCH (a)-[:calls*1..3]-(b) WHERE a.x > 1 AND b.name STARTS WITH 'abc' RETURN DISTINCT a"

# Rejected parses -- these exercise the error-path cleanup (the M1 leak).
run $VG "$KG" --ast ""
run $VG "$KG" --ast "MATCH (a"
run $VG "$KG" --ast "CREATE (a) RETURN a"
run $VG "$KG" --ast "MATCH (a) RETURN"
run $VG "$KG" --ast "MATCH (a)-[r:calls]-> RETURN a"

# Catalog paths, if a fixture can be built.
db="vg_fixture.db"
if make_fixture "$db"; then
	run $VG "$KG" --catalog "$db"
	run $VG "$KG" --catalog "$db" "ns:f"
	run $VG "$KG" --catalog "$db" "ns:f.x"
	run $VG "$KG" --catalog "$db" "nope"
	run $VG "$KG" --catalog "$db" "ns:f.nope"

	# --explain: successful translations and rejected/erroring ones, so the
	# transformer's cleanup is exercised on both paths.
	run $VG "$KG" --explain "$db" "MATCH (a:\`ns:f\`) RETURN a.x"
	run $VG "$KG" --explain "$db" "MATCH (a:\`ns:f\`)-[r:calls]->(b:\`ns2:g\`) RETURN r.alias, b.id"
	run $VG "$KG" --explain "$db" "MATCH (a:\`ns:f\`)<-[r:calls]-(b:\`ns2:g\`) RETURN b.id"
	run $VG "$KG" --explain "$db" "MATCH (a:\`ns:f\`) RETURN a.nope"
	run $VG "$KG" --explain "$db" "MATCH (a) WHERE a.x = 1 RETURN a.x"

	# Full execution path (parse -> transform -> exec -> output): a node query,
	# a relationship query (with a NULL field), and an erroring translation.
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) RETURN a.x AS ex, a.y"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`)-[r:calls]->(b:\`ns2:g\`) RETURN r.alias, b.id"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) RETURN a.nope"

	# WHERE path (M5): a compound node filter, a rel filter joining b in via
	# WHERE, a LIKE, and error paths (unknown column and a non-literal LIKE).
	run $VG "$KG" --explain "$db" "MATCH (a:\`ns:f\`) WHERE a.x >= 1 AND a.y <> 'z' OR NOT a.x IN [3] RETURN a.x"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) WHERE a.y STARTS WITH 'a' RETURN a.id"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`)-[r:calls]->(b:\`ns2:g\`) WHERE b.z > 1.0 AND r.alias IS NULL RETURN a.id"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) WHERE a.nope = 1 RETURN a.id"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) WHERE a.y STARTS WITH a.x RETURN a.y"

	# Pattern engine (M6): a two-hop chain, an undirected hop, a multi-pattern
	# match, and error paths (undirected in a chain, conflicting labels).
	run $VG "$KG" --explain "$db" "MATCH (a:\`ns:f\`)-[r1:calls]->(b:\`ns2:g\`)-[r2:wraps]->(c:\`ns:f\`) WHERE a.x = 1 RETURN a.id, c.id"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`)-[:calls]->(b:\`ns2:g\`)-[:wraps]->(c:\`ns:f\`) RETURN a.id, c.id"
	run $VG "$KG" "$db" "MATCH (a:\`ns2:g\`)-[r:calls]-(b:\`ns:f\`) RETURN a.id, b.id"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`), (b:\`ns2:g\`) RETURN a.id, b.id"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`)-[:calls]->(b)-[:wraps]-(c) RETURN a.id"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`)-[:calls]->(a:\`ns2:g\`) RETURN a.id"

	# Aggregation/DISTINCT/ORDER BY/SKIP/LIMIT (M7): grouped aggregate, collect,
	# DISTINCT, ordered+paged, and error paths (bad function, bad arity,
	# non-integer LIMIT, unknown ORDER BY name).
	run $VG "$KG" --explain "$db" "MATCH (a:\`ns:f\`)-[r:calls]->(b:\`ns2:g\`) RETURN b.id, count(*) AS n"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) RETURN a.y, count(a.x) AS n ORDER BY n DESC LIMIT 5"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) RETURN collect(a.id) AS ids"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`), (b:\`ns2:g\`) RETURN DISTINCT b.id"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) RETURN a.x ORDER BY a.x SKIP 1 LIMIT 2"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) RETURN foo(a.x)"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) RETURN sum(a.x, a.y)"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) RETURN a.x LIMIT a.x"
	run $VG "$KG" "$db" "MATCH (a:\`ns:f\`) RETURN a.x ORDER BY zzz"

	rm -f "$db"
fi

exit $fail
