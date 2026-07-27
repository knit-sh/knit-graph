/*
 * transform.c -- AST -> SQL for the two simplest Cypher shapes (M3):
 *
 *   MATCH (a:`ns:f`) RETURN a.x
 *       -> SELECT a."x" FROM "ns:f" a
 *
 *   MATCH (a:`ns:f`)-[r:calls]->(b:`ns2:g`) RETURN b.id
 *       -> SELECT b."id" FROM "__provenance__" r
 *          JOIN "ns2:g" b ON b."id" = r."target_id"
 *          WHERE r."edge_type" = 'calls'
 *            AND r."source_name" = 'ns:f' AND r."target_name" = 'ns2:g'
 *
 * A relationship maps to a row of the __provenance__ edge table; each labelled
 * endpoint contributes a source_name/target_name predicate, and a JOIN back to
 * the function table only when that node's own columns are returned. `<-`
 * swaps the source and target roles.
 *
 * Anything beyond a single node or single hop -- WHERE, chains, undirected or
 * variable-length relationships, multiple labels/types, inline property maps,
 * aggregation, DISTINCT, ORDER BY/SKIP/LIMIT, RETURN of a whole entity -- is
 * rejected here; those arrive in later milestones.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "transform.h"
#include "sqlbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Set *errmsg (if requested) to a formatted, malloc'd message; return 1. */
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

/* Append a double-quoted SQL identifier, doubling any embedded quotes. */
static void append_ident(SqlBuf *b, const char *id)
{
	sqlbuf_append_char(b, '"');
	for (const char *p = id; *p; p++) {
		if (*p == '"')
			sqlbuf_append(b, "\"\"");
		else
			sqlbuf_append_char(b, *p);
	}
	sqlbuf_append_char(b, '"');
}

/* Append a single-quoted SQL string literal, doubling any embedded quotes. */
static void append_string(SqlBuf *b, const char *s)
{
	sqlbuf_append_char(b, '\'');
	for (const char *p = s; *p; p++) {
		if (*p == '\'')
			sqlbuf_append(b, "''");
		else
			sqlbuf_append_char(b, *p);
	}
	sqlbuf_append_char(b, '\'');
}

/* Append `alias."column"`. */
static void append_column(SqlBuf *b, const char *alias, const char *column)
{
	sqlbuf_append(b, alias);
	sqlbuf_append_char(b, '.');
	append_ident(b, column);
}

/* --------------------------------------------------------------- bindings --- */

/*
 * One entity the RETURN list may refer to. Nodes carry the function table they
 * resolve to (from their single label) and whether they sit on the source or
 * target side of the edge; the edge itself resolves to __provenance__.
 */
typedef struct {
	const char *var;    /* user variable name */
	const char *table;  /* resolved table, for column validation */
	int is_node;
	int is_target;      /* nodes: 0 = source side, 1 = target side */
	int referenced;     /* a property of this entity appears in RETURN */
} Binding;

static Binding *find_binding(Binding *binds, int n, const char *var)
{
	for (int i = 0; i < n; i++)
		if (binds[i].var && strcmp(binds[i].var, var) == 0)
			return &binds[i];
	return NULL;
}

/* Reject the constructs a single-node/single-rel translator cannot yet emit. */
static int check_return_shape(const Return *r, char **errmsg)
{
	if (r->star)
		return fail(errmsg, "RETURN * is not supported yet");
	if (r->distinct)
		return fail(errmsg, "RETURN DISTINCT is not supported yet");
	if (r->order || r->skip || r->limit)
		return fail(errmsg, "ORDER BY/SKIP/LIMIT are not supported yet");

	for (ReturnItem *it = r->items; it; it = it->next) {
		if (it->expr->kind != EXPR_PROPERTY)
			return fail(errmsg,
				"only property references (e.g. a.x) are supported "
				"in RETURN yet");
	}
	return 0;
}

/* A node the transformer understands: at most one label, no inline props. */
static int check_node(const NodePat *n, char **errmsg)
{
	if (n->props)
		return fail(errmsg, "inline property maps are not supported yet");
	if (n->labels && n->labels->next)
		return fail(errmsg, "multiple node labels are not supported yet");
	return 0;
}

/*
 * Resolve every RETURN property against the bindings and emit the SELECT list.
 * Marks referenced nodes so the caller knows which joins to emit.
 */
static int emit_select(SqlBuf *sql, const Return *r, const Catalog *cat,
                      Binding *binds, int nbinds, char **errmsg)
{
	sqlbuf_append(sql, "SELECT ");
	int first = 1;
	for (ReturnItem *it = r->items; it; it = it->next) {
		const char *var = it->expr->var;
		const char *key = it->expr->key;

		Binding *bd = find_binding(binds, nbinds, var);
		if (!bd)
			return fail(errmsg, "unknown variable: %s", var);
		if (!bd->table)
			return fail(errmsg,
				"cannot resolve columns of '%s': it has no label",
				var);

		const CatalogTable *t = catalog_find_table(cat, bd->table);
		if (!t)
			return fail(errmsg, "unknown table: %s", bd->table);
		if (!catalog_table_has_column(t, key))
			return fail(errmsg, "unknown column: %s.%s", bd->table, key);
		bd->referenced = 1;

		if (!first)
			sqlbuf_append(sql, ", ");
		first = 0;
		append_column(sql, var, key);
		if (it->alias) {
			sqlbuf_append(sql, " AS ");
			append_ident(sql, it->alias);
		}
	}
	return 0;
}

/* ------------------------------------------------------------ single node --- */

static int transform_single_node(const NodePat *n, const Return *r,
                                 const Catalog *cat, char **sql, char **errmsg)
{
	if (check_node(n, errmsg))
		return 1;
	if (!n->labels)
		return fail(errmsg,
			"node has no label; cannot determine which table to query");

	const char *table = n->labels->s;
	if (!catalog_find_table(cat, table))
		return fail(errmsg, "unknown table: %s", table);

	const char *alias = n->var ? n->var : "n";
	Binding b = { alias, table, 1, 0, 0 };

	SqlBuf buf;
	sqlbuf_init(&buf);
	if (emit_select(&buf, r, cat, &b, 1, errmsg)) {
		sqlbuf_free(&buf);
		return 1;
	}
	sqlbuf_append(&buf, " FROM ");
	append_ident(&buf, table);
	sqlbuf_append_char(&buf, ' ');
	sqlbuf_append(&buf, alias);

	*sql = sqlbuf_detach(&buf);
	return 0;
}

/* ------------------------------------------------------ single relationship --- */

/* Build the label predicate for one endpoint, if it has a label. */
static void emit_endpoint_predicate(SqlBuf *w, int *first, const char *edge,
                                   const char *role, const char *label)
{
	if (!label)
		return;
	if (!*first)
		sqlbuf_append(w, " AND ");
	*first = 0;
	append_column(w, edge, role);
	sqlbuf_append(w, " = ");
	append_string(w, label);
}

/* Emit the JOIN back to a node's function table, if that node is returned. */
static void emit_node_join(SqlBuf *f, const Binding *b, const char *edge)
{
	if (!b->is_node || !b->referenced || !b->table)
		return;
	sqlbuf_append(f, " JOIN ");
	append_ident(f, b->table);
	sqlbuf_append_char(f, ' ');
	sqlbuf_append(f, b->var);
	sqlbuf_append(f, " ON ");
	append_column(f, b->var, "id");
	sqlbuf_append(f, " = ");
	append_column(f, edge, b->is_target ? "target_id" : "source_id");
}

static int transform_single_rel(const NodePat *left, const RelPat *rel,
                               const NodePat *right, const Return *r,
                               const Catalog *cat, char **sql, char **errmsg)
{
	if (check_node(left, errmsg) || check_node(right, errmsg))
		return 1;
	if (rel->props)
		return fail(errmsg, "inline property maps are not supported yet");
	if (rel->varlen.present)
		return fail(errmsg,
			"variable-length relationships are not supported yet");
	if (rel->dir == REL_UNDIR)
		return fail(errmsg,
			"undirected relationships are not supported yet");
	if (rel->types && rel->types->next)
		return fail(errmsg,
			"multiple relationship types are not supported yet");

	/* `->` : left is source, right is target. `<-` swaps the two. */
	const NodePat *src = (rel->dir == REL_LEFT) ? right : left;
	const NodePat *tgt = (rel->dir == REL_LEFT) ? left : right;
	const char *src_label = src->labels ? src->labels->s : NULL;
	const char *tgt_label = tgt->labels ? tgt->labels->s : NULL;

	if (src_label && !catalog_find_table(cat, src_label))
		return fail(errmsg, "unknown table: %s", src_label);
	if (tgt_label && !catalog_find_table(cat, tgt_label))
		return fail(errmsg, "unknown table: %s", tgt_label);

	const char *edge = rel->var ? rel->var : "r";

	/* Bindings, in pattern order (left, then right), for RETURN resolution.
	 * The edge itself resolves to __provenance__ and needs no join. */
	Binding binds[3];
	int nb = 0;
	if (left->var)
		binds[nb++] = (Binding){ left->var,
			left->labels ? left->labels->s : NULL, 1,
			(rel->dir == REL_LEFT) ? 1 : 0, 0 };
	if (right->var)
		binds[nb++] = (Binding){ right->var,
			right->labels ? right->labels->s : NULL, 1,
			(rel->dir == REL_LEFT) ? 0 : 1, 0 };
	if (rel->var)
		binds[nb++] = (Binding){ rel->var, KG_EDGE_TABLE, 0, 0, 0 };

	SqlBuf sel;
	sqlbuf_init(&sel);
	if (emit_select(&sel, r, cat, binds, nb, errmsg)) {
		sqlbuf_free(&sel);
		return 1;
	}

	/* A returned node without a label has no table to join to. */
	for (int i = 0; i < nb; i++) {
		if (binds[i].referenced && !binds[i].table) {
			sqlbuf_free(&sel);
			return fail(errmsg,
				"cannot resolve columns of '%s': it has no label",
				binds[i].var);
		}
	}

	SqlBuf from;
	sqlbuf_init(&from);
	sqlbuf_append(&from, " FROM ");
	append_ident(&from, KG_EDGE_TABLE);
	sqlbuf_append_char(&from, ' ');
	sqlbuf_append(&from, edge);
	for (int i = 0; i < nb; i++)
		emit_node_join(&from, &binds[i], edge);

	SqlBuf where;
	sqlbuf_init(&where);
	int first = 1;
	if (rel->types) {
		append_column(&where, edge, "edge_type");
		sqlbuf_append(&where, " = ");
		append_string(&where, rel->types->s);
		first = 0;
	}
	emit_endpoint_predicate(&where, &first, edge, "source_name", src_label);
	emit_endpoint_predicate(&where, &first, edge, "target_name", tgt_label);

	SqlBuf out;
	sqlbuf_init(&out);
	sqlbuf_append(&out, sel.data ? sel.data : "");
	sqlbuf_append(&out, from.data ? from.data : "");
	if (where.len) {
		sqlbuf_append(&out, " WHERE ");
		sqlbuf_append(&out, where.data);
	}

	sqlbuf_free(&sel);
	sqlbuf_free(&from);
	sqlbuf_free(&where);
	*sql = sqlbuf_detach(&out);
	return 0;
}

/* ------------------------------------------------------------- entry point --- */

int transform_query(const Query *q, const Catalog *cat,
                    char **sql, char **errmsg)
{
	*sql = NULL;
	if (errmsg)
		*errmsg = NULL;

	if (q->matches->next)
		return fail(errmsg, "multiple MATCH clauses are not supported yet");

	const Match *m = q->matches;
	if (m->where)
		return fail(errmsg, "WHERE is not supported yet");
	if (m->patterns->next)
		return fail(errmsg,
			"multiple patterns in one MATCH are not supported yet");

	if (check_return_shape(q->ret, errmsg))
		return 1;

	const Path *p = m->patterns->path;
	if (p->segments == NULL)
		return transform_single_node(p->start, q->ret, cat, sql, errmsg);
	if (p->segments->next == NULL)
		return transform_single_rel(p->start, p->segments->rel,
			p->segments->node, q->ret, cat, sql, errmsg);

	return fail(errmsg, "multi-hop patterns are not supported yet");
}
