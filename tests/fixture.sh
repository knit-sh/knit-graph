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
VALUES
	-- The 'calls' edge f1 -> g1 drives the single-hop tests.
	('f1', 'ns:f', 'g1', 'ns2:g', 'calls', 0.0, 1.0, NULL),
	-- A second edge of a *different* type, g1 -> f2, so f1 -calls-> g1 -wraps-> f2
	-- is a two-hop chain. Its distinct edge_type keeps the 'calls'-only queries
	-- (and the reversed-direction test) unaffected.
	('g1', 'ns2:g', 'f2', 'ns:f', 'wraps', 0.0, 1.0, NULL),
	-- A 'chain' edge_type forming a 2-cycle within ns:f (f1 -> f2 -> f1), used by
	-- the variable-length tests (M8). Being its own edge_type over the existing
	-- node ids, it is invisible to every 'calls'/'wraps' test, and the cycle
	-- exercises the recursion's edge-uniqueness termination guard on unbounded
	-- walks. Reachable from f1: f2 at depth 1, f1 at depth 2, then no new edges.
	('f1', 'ns:f', 'f2', 'ns:f', 'chain', 0.0, 1.0, NULL),
	('f2', 'ns:f', 'f1', 'ns:f', 'chain', 0.0, 1.0, NULL);
SQL
}
