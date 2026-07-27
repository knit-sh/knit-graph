#!/bin/sh
# catalog.sh -- exercise schema introspection against a fixture database.
#
# Checks that --catalog lists tables/columns correctly and that lookups of a
# known table/column succeed while unknown ones fail with a non-zero exit.

: "${KG:?KG must point at the knit-graph binary}"
: "${srcdir:?srcdir must be set}"

. "$srcdir/fixture.sh"

db="cat_fixture.db"
if ! make_fixture "$db"; then
	echo "sqlite3 CLI unavailable; skipping catalog test"
	exit 77
fi

fail=0

# Full dump must match the golden file exactly (tables sorted, columns in order).
if ! "$KG" --catalog "$db" > cat_dump.out 2>&1; then
	echo "FAIL: catalog dump exited non-zero"; fail=1
fi
if ! diff -u "$srcdir/golden/catalog.expected" cat_dump.out; then
	echo "FAIL: catalog dump did not match golden"; fail=1
fi

# Known references succeed.
for ref in "ns:f" "ns2:g" "ns:f.x" "__provenance__.alias"; do
	if ! "$KG" --catalog "$db" "$ref" > /dev/null 2>&1; then
		echo "FAIL: known reference rejected: $ref"; fail=1
	fi
done

# Unknown references must fail. "kv"/"counter" exist in the database but have no
# TEXT "id" key, so the catalog excludes them and they read as unknown tables.
for ref in "nope:missing" "ns:f.nope" "__provenance__.bogus" "kv" "counter"; do
	if "$KG" --catalog "$db" "$ref" > /dev/null 2>&1; then
		echo "FAIL: unknown reference accepted: $ref"; fail=1
	fi
done

# The excluded tables must not appear anywhere in the full dump.
if grep -Eq '(^| )(kv|counter)$' cat_dump.out; then
	echo "FAIL: a non-graph table leaked into the catalog dump"; fail=1
fi

rm -f "$db" cat_dump.out
exit $fail
