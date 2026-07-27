# fixture.sh -- build a small sample provenance database.
#
# Sourced by the catalog and memcheck tests (not a test itself). Defines
# make_fixture DBPATH, which creates the database with the sqlite3 CLI and
# returns non-zero if that CLI is unavailable, so callers can skip gracefully.

make_fixture() {
	db="$1"
	command -v sqlite3 >/dev/null 2>&1 || return 1
	rm -f "$db"
	sqlite3 "$db" <<'SQL'
CREATE TABLE "ns:f"  (id TEXT, x INTEGER, y TEXT);
CREATE TABLE "ns2:g" (id TEXT, z REAL);
-- Non-graph tables: no TEXT "id" key, so the catalog must skip them.
CREATE TABLE "kv"      (key TEXT, value TEXT);
CREATE TABLE "counter" (id INTEGER, n INTEGER);
CREATE TABLE "__provenance__" (
	source_id   TEXT,
	source_name TEXT,
	target_id   TEXT,
	target_name TEXT,
	edge_type   TEXT,
	start_time  REAL,
	end_time    REAL,
	alias       TEXT
);
INSERT INTO "ns:f"  (id, x, y) VALUES ('f1', 1, 'a'), ('f2', 2, 'b');
INSERT INTO "ns2:g" (id, z)    VALUES ('g1', 1.5);
INSERT INTO "__provenance__"
	(source_id, source_name, target_id, target_name, edge_type, start_time, end_time, alias)
VALUES ('f1', 'ns:f', 'g1', 'ns2:g', 'calls', 0.0, 1.0, NULL);
SQL
}
