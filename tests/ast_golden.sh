#!/bin/sh
# Compare the --ast dump of each golden query against its recorded output.
KG=${KG:-./src/knit-graph}
srcdir=${srcdir:-.}
fail=0

for cy in "$srcdir"/golden/*.cypher; do
	name=$(basename "$cy" .cypher)
	exp="$srcdir/golden/$name.expected"
	q=$(cat "$cy")
	got=$("$KG" --ast "$q" 2>&1)
	if [ "$got" = "$(cat "$exp")" ]; then
		echo "PASS: $name"
	else
		echo "FAIL: $name"
		echo "--- expected ---"
		cat "$exp"
		echo "--- got ---"
		printf '%s\n' "$got"
		fail=1
	fi
done

exit $fail
