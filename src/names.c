/*
 * names.c -- the command-name <-> table-name map (see names.h).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "names.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One (table, name) pairing. */
typedef struct {
	char *table;
	char *name;
} NameEntry;

struct NameMap {
	NameEntry *entries;
	int        n;
};

/* Duplicate a C string or abort; nothing sensible to do on OOM here. */
static char *xstrdup(const char *s)
{
	char *p = strdup(s ? s : "");
	if (!p) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	return p;
}

static int fail(char **errmsg, const char *fmt, ...)
{
	if (errmsg) {
		char buf[256];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
		*errmsg = strdup(buf);
	}
	return 1;
}

static NameMap *map_new(void)
{
	NameMap *m = calloc(1, sizeof *m);
	if (!m) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	return m;
}

/* Append a (table, name) entry, taking copies of both strings. */
static void map_add(NameMap *m, const char *table, const char *name)
{
	NameEntry *grown =
		realloc(m->entries, (m->n + 1) * sizeof *grown);
	if (!grown) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	m->entries = grown;
	m->entries[m->n].table = xstrdup(table);
	m->entries[m->n].name = xstrdup(name);
	m->n++;
}

/* Non-zero if the map already has an entry whose table is NAME. */
static int map_has_table(const NameMap *m, const char *name)
{
	for (int i = 0; i < m->n; i++)
		if (strcmp(m->entries[i].table, name) == 0)
			return 1;
	return 0;
}

/* Trim leading and trailing ASCII spaces/tabs in place; returns s. */
static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t'
			|| end[-1] == '\r' || end[-1] == '\n'))
		*--end = '\0';
	return s;
}

/*
 * Parse one `table=name` entry (already isolated between separators) into the
 * map. Blank entries are ignored. A missing '=' or an empty side is an error.
 */
static int parse_entry(NameMap *m, char *entry, char **errmsg)
{
	entry = trim(entry);
	if (*entry == '\0')
		return 0;

	char *eq = strchr(entry, '=');
	if (!eq)
		return fail(errmsg, "invalid name map entry (expected "
			"table=name): %s", entry);
	*eq = '\0';
	char *table = trim(entry);
	char *name = trim(eq + 1);
	if (*table == '\0' || *name == '\0')
		return fail(errmsg, "invalid name map entry (empty table or "
			"name): %s=%s", table, name);
	map_add(m, table, name);
	return 0;
}

int names_parse(const char *spec, NameMap **out, char **errmsg)
{
	*out = NULL;
	if (errmsg)
		*errmsg = NULL;

	NameMap *m = map_new();
	if (!spec) {
		*out = m;
		return 0;
	}

	/* Split on newlines and ';' into individual entries; parse each. */
	char *buf = xstrdup(spec);
	char *p = buf;
	int rc = 0;
	while (*p) {
		char *sep = p + strcspn(p, "\n;");
		int done = (*sep == '\0');
		*sep = '\0';
		if (parse_entry(m, p, errmsg)) {
			rc = 1;
			break;
		}
		if (done)
			break;
		p = sep + 1;
	}
	free(buf);

	if (rc) {
		names_free(m);
		return 1;
	}
	*out = m;
	return 0;
}

int names_parse_file(const char *path, NameMap **out, char **errmsg)
{
	*out = NULL;
	if (errmsg)
		*errmsg = NULL;

	FILE *f = fopen(path, "rb");
	if (!f)
		return fail(errmsg, "cannot open name map file: %s", path);

	/* Slurp the whole file. */
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	size_t got;
	while ((got = fread(buf + len, 1, cap - len, f)) > 0) {
		len += got;
		if (len == cap) {
			cap *= 2;
			char *grown = realloc(buf, cap);
			if (!grown) {
				fprintf(stderr, "knit-graph: out of memory\n");
				exit(1);
			}
			buf = grown;
		}
	}
	int ferr = ferror(f);
	fclose(f);
	if (ferr) {
		free(buf);
		return fail(errmsg, "cannot read name map file: %s", path);
	}
	buf[len] = '\0';

	int rc = names_parse(buf, out, errmsg);
	free(buf);
	return rc;
}

/* Find the catalog table (other than the edge table) holding a row with ID. */
static int table_holding_id(sqlite3 *db, const Catalog *cat, const char *id,
                           const char **table, char **errmsg)
{
	*table = NULL;
	for (int i = 0; i < cat->ntables; i++) {
		const char *tname = cat->tables[i].name;
		if (strcmp(tname, KG_EDGE_TABLE) == 0)
			continue;

		char *sql = sqlite3_mprintf(
			"SELECT 1 FROM \"%w\" WHERE \"id\" = ?1 LIMIT 1", tname);
		if (!sql)
			return fail(errmsg, "out of memory");

		sqlite3_stmt *st = NULL;
		int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
		sqlite3_free(sql);
		if (rc != SQLITE_OK)
			return fail(errmsg, "%s", sqlite3_errmsg(db));

		sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(st);
		sqlite3_finalize(st);
		if (rc == SQLITE_ROW) {
			*table = tname;
			return 0;
		}
		if (rc != SQLITE_DONE)
			return fail(errmsg, "%s", sqlite3_errmsg(db));
	}
	return 0;
}

int names_derive(sqlite3 *db, const Catalog *cat, NameMap **out, char **errmsg)
{
	*out = NULL;
	if (errmsg)
		*errmsg = NULL;

	NameMap *m = map_new();

	/* Every distinct name in the edge table, with a representative id. */
	const char *q =
		"SELECT source_name, source_id FROM \"" KG_EDGE_TABLE "\" "
		"WHERE source_name IS NOT NULL "
		"UNION "
		"SELECT target_name, target_id FROM \"" KG_EDGE_TABLE "\" "
		"WHERE target_name IS NOT NULL";
	sqlite3_stmt *st = NULL;
	int rc = sqlite3_prepare_v2(db, q, -1, &st, NULL);
	if (rc != SQLITE_OK) {
		fail(errmsg, "%s", sqlite3_errmsg(db));
		names_free(m);
		return 1;
	}

	int failed = 0;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		const unsigned char *name = sqlite3_column_text(st, 0);
		const unsigned char *id = sqlite3_column_text(st, 1);
		if (!name || !id)
			continue;
		/* One entry per name suffices (a name maps to one table). */
		if (map_has_table(m, (const char *)name))
			continue;

		const char *table = NULL;
		if (table_holding_id(db, cat, (const char *)id, &table, errmsg)) {
			failed = 1;
			break;
		}
		if (table)
			map_add(m, table, (const char *)name);
	}
	sqlite3_finalize(st);

	if (!failed && rc != SQLITE_DONE && rc != SQLITE_ROW) {
		fail(errmsg, "%s", sqlite3_errmsg(db));
		failed = 1;
	}
	if (failed) {
		names_free(m);
		return 1;
	}
	*out = m;
	return 0;
}

int names_resolve(const NameMap *map, const char *label,
                  const char **table, const char **name, char **errmsg)
{
	*table = NULL;
	*name = NULL;
	if (!label)
		return 0;

	/* Match LABEL as a table name and (separately) as a recorded name. */
	const char *as_table_t = NULL, *as_table_n = NULL;
	const char *as_name_t = NULL, *as_name_n = NULL;
	if (map) {
		for (int i = 0; i < map->n; i++) {
			if (strcmp(map->entries[i].table, label) == 0) {
				as_table_t = map->entries[i].table;
				as_table_n = map->entries[i].name;
			}
			if (strcmp(map->entries[i].name, label) == 0) {
				as_name_t = map->entries[i].table;
				as_name_n = map->entries[i].name;
			}
		}
	}

	if (as_table_t && as_name_t) {
		/* Ambiguous only if the two readings disagree. */
		if (strcmp(as_table_t, as_name_t) != 0
				|| strcmp(as_table_n, as_name_n) != 0)
			return fail(errmsg,
				"ambiguous label '%s': it is both a table name "
				"and a command name", label);
		*table = as_table_t;
		*name = as_table_n;
	} else if (as_table_t) {
		*table = as_table_t;
		*name = as_table_n;
	} else if (as_name_t) {
		*table = as_name_t;
		*name = as_name_n;
	} else {
		*table = label;
		*name = label;
	}
	return 0;
}

void names_free(NameMap *map)
{
	if (!map)
		return;
	for (int i = 0; i < map->n; i++) {
		free(map->entries[i].table);
		free(map->entries[i].name);
	}
	free(map->entries);
	free(map);
}
