#!/bin/sh
# Every statement here is invalid or a write clause and must be rejected
# (non-zero exit). Write clauses are rejected because the grammar omits them.
KG=${KG:-./src/knit-graph}
fail=0

check_rejected() {
	desc="$1"
	q="$2"
	if "$KG" --ast "$q" >/dev/null 2>&1; then
		echo "FAIL (should be rejected): $desc"
		fail=1
	else
		echo "PASS (rejected): $desc"
	fi
}

# Empty input.
check_rejected "empty statement" ""

while IFS= read -r q; do
	[ -z "$q" ] && continue
	check_rejected "$q" "$q"
done <<'QUERIES'
CREATE (a) RETURN a
MATCH (a) CREATE (b) RETURN a
MATCH (a) SET a.x = 1 RETURN a
MATCH (a) DELETE a
MERGE (a) RETURN a
MATCH (a) REMOVE a.x RETURN a
MATCH (a)
RETURN 1
MATCH (a RETURN a
MATCH (a) RETURN
MATCH (a) WHERE RETURN a
MATCH (a) RETURN a ORDER a
MATCH (a)-[:calls*1..2..3]->(b) RETURN b
foobar
MATCH (a) RETURN a LIMIT
QUERIES

exit $fail
