#!/bin/sh
# Every statement here is valid read-only Cypher and must parse (exit 0).
KG=${KG:-./src/knit-graph}
fail=0

# Quoted heredoc: backticks and $ are literal, one query per line.
while IFS= read -r q; do
	[ -z "$q" ] && continue
	if "$KG" --ast "$q" >/dev/null 2>&1; then
		echo "PASS: $q"
	else
		echo "FAIL (should parse): $q"
		fail=1
	fi
done <<'QUERIES'
MATCH (n) RETURN n
MATCH (a:Foo) RETURN a.x
MATCH (a:`ns:f`) RETURN a
MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN b.id, count(*) AS n ORDER BY n DESC LIMIT 5
MATCH (a)-->(b) RETURN a
MATCH (a)<--(b) RETURN a
MATCH (a)--(b) RETURN a
MATCH (a)-[r:calls]->(b) RETURN r
MATCH (a)-[:calls|uses]->(b) RETURN b
MATCH (a)-[:calls*]->(b) RETURN b
MATCH (a)-[:calls*2]->(b) RETURN b
MATCH (a)-[:calls*1..]->(b) RETURN b
MATCH (a)-[:calls*..3]->(b) RETURN b
MATCH (a)-[:calls*1..3]->(b) RETURN b
MATCH (a {name: "x", k: 1}) RETURN a
MATCH (a)-[{alias: "g1"}]->(b) RETURN a
MATCH (a), (b) RETURN a, b
MATCH (a) WHERE a.x = 1 OR a.y <> 2 RETURN a
MATCH (a) WHERE NOT a.x IS NULL RETURN a
MATCH (a) WHERE a.x IS NOT NULL RETURN a
MATCH (a) WHERE a.x IN [1, 2, 3] RETURN a
MATCH (a) WHERE a.n CONTAINS 'z' RETURN a
MATCH (a) WHERE a.n STARTS WITH 'z' RETURN a
MATCH (a) WHERE a.n ENDS WITH 'z' RETURN a
MATCH (a) RETURN DISTINCT a.x AS x ORDER BY a.x ASC, a.y DESC SKIP 2 LIMIT 10
MATCH (a)-[r]->(b)-[s]->(c) RETURN a, b, c
MATCH (a) RETURN count(DISTINCT a.x)
MATCH (a) RETURN collect(a.id)
MATCH (a) RETURN *
MATCH (a) RETURN a;
MATCH (a) WHERE a.x >= 1 AND a.y <= 2 RETURN a
MATCH (a) WHERE type(a) = 'calls' RETURN a
MATCH (a) WHERE a.t = 1.5 RETURN a
QUERIES

exit $fail
