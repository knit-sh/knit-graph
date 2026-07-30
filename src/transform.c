/*
 * transform.c -- AST -> SQL for the Cypher pattern shapes knit-graph handles.
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
 * the function table only when that node's own columns are used. `->`/`<-` fix
 * which endpoint is the source and which the target.
 *
 * The engine is general over patterns (M6): a MATCH may hold several
 * comma-separated paths, each a chain of any length.  Every relationship is one
 * __provenance__ instance; the first is the FROM, the rest are JOINed.  Each
 * node is a "slot", identified by its variable (anonymous nodes are per
 * occurrence).  A slot's identity is anchored to the first edge/side it appears
 * on; a later edge touching the same slot is tied to that anchor with
 * `edge.<side>_id = anchor.<side>_id AND edge.<side>_name = anchor.<side>_name`
 * (this is how `(a)-[]->(b)-[]->(c)` chains, and how a shared variable across
 * comma patterns joins).  A node whose columns are used is JOINed to its
 * function table; a node standing alone in a path (no relationship) becomes its
 * own FROM/cross-joined table.
 *
 * An undirected hop `(a)-[r]-(b)` is translated separately as an OR over both
 * orientations; it is supported only as a single, sole hop for now.
 *
 * A WHERE clause (M5) is translated to a boolean SQL expression -- comparisons
 * (= <> < > <= >=), AND/OR/NOT, IN (...), IS [NOT] NULL, STARTS WITH / ENDS
 * WITH / CONTAINS (-> LIKE), and literals (safely quoted) -- and ANDed onto the
 * pattern predicates.  Property references in WHERE resolve exactly like those
 * in RETURN and cause the owning node's table to be joined in.
 *
 * RETURN also carries the M7 projection tail: aggregate functions
 * (count/sum/avg/min/max, and collect -> json_group_array) with an implicit
 * GROUP BY on the non-aggregated items, DISTINCT, ORDER BY ... [ASC|DESC],
 * SKIP (-> OFFSET) and LIMIT.
 *
 * A variable-length hop `(a)-[:t*m..n]->(b)` (M8) is compiled to a recursive
 * CTE that walks __provenance__ from each edge outward, carrying the original
 * source, the current frontier, a depth, and the set of edges already used.
 * The edge-uniqueness guard (an edge's rowid may appear once per path) both
 * matches Cypher's relationship-uniqueness and guarantees termination on a
 * cyclic graph, so unbounded `*` and `*m..` are safe. The walk exposes the same
 * source_/target_ column pairs as the edge table, so the endpoint joins, label
 * predicates and the whole RETURN tail above are reused unchanged. It is
 * supported as a single directed hop (like the undirected case).
 *
 * A whole entity in RETURN (M9) -- `RETURN a` for a node or `RETURN r` for a
 * relationship -- expands to a json_object() over that entity's catalog columns,
 * in schema order, aliased to the variable name. A node's table is joined in
 * (like any column reference); a relationship maps to the edge table already in
 * the FROM. It works uniformly across the general, undirected and (for nodes)
 * variable-length engines, since all share the SELECT/RETURN machinery.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "transform.h"
#include "names.h"
#include "sqlbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

/* --------------------------------------------------------------- model --- */

/*
 * One node the pattern refers to. A node is identified across the whole query
 * by its variable name; anonymous nodes are identified per occurrence. `label`
 * is its function table and `name` its edge-filter value (both from its single
 * label, resolved through the name map); `alias` is the SQL alias used for that
 * table. A node is anchored to the first edge/side it appears on
 * (anchor_edge == -1 means it never touches an edge -- a lone node with its own
 * table). `referenced` records that a column of the node is used, so its table
 * must be joined in.
 */
typedef struct {
	const char    *var;           /* NULL if anonymous */
	const NodePat *np;            /* first occurrence, for anonymous identity */
	const char    *label;         /* resolved table, or NULL */
	const char    *name;          /* resolved *_name value, or NULL */
	const char    *alias;         /* SQL alias for the table */
	int            referenced;
	int            is_lone;       /* never an edge endpoint -> own table */
	int            anchor_edge;   /* index into edges[]; -1 if lone */
	int            anchor_target; /* anchored on the target side? */
} Slot;

/* One relationship: a row of __provenance__ tying a source slot to a target. */
typedef struct {
	const char     *var;   /* NULL if anonymous */
	const char     *alias; /* SQL alias for the edge table */
	const char     *type;  /* edge_type literal, or NULL */
	const PropList *props; /* inline property map, or NULL */
	int             src;   /* source slot index */
	int             tgt;   /* target slot index */
} Edge;

/*
 * A name the RETURN list or WHERE clause may refer to: a node slot (is_node,
 * slot indexes into slots[]) or the edge table (is_node == 0). `table` drives
 * column validation; it is NULL for a node without a label.
 */
typedef struct {
	const char *var;
	const char *table;
	int         is_node;
	int         slot;
} Bind;

static Bind *find_bind(Bind *binds, int n, const char *var)
{
	for (int i = 0; i < n; i++)
		if (binds[i].var && strcmp(binds[i].var, var) == 0)
			return &binds[i];
	return NULL;
}

/*
 * Resolve a property reference (var.key) against the binds and catalog, emit
 * `var."key"`, and (for a node) mark its slot referenced so the translator
 * joins that node's table in. Shared by RETURN and WHERE.
 */
static int emit_property(SqlBuf *b, const char *var, const char *key,
                        const Catalog *cat, Bind *binds, int nb, Slot *slots,
                        char **errmsg)
{
	Bind *bd = find_bind(binds, nb, var);
	if (!bd)
		return fail(errmsg, "unknown variable: %s", var);
	if (!bd->table)
		return fail(errmsg,
			"cannot resolve columns of '%s': it has no label", var);

	const CatalogTable *t = catalog_find_table(cat, bd->table);
	if (!t)
		return fail(errmsg, "unknown table: %s", bd->table);
	if (!catalog_table_has_column(t, key))
		return fail(errmsg, "unknown column: %s.%s", bd->table, key);

	if (bd->is_node)
		slots[bd->slot].referenced = 1;
	append_column(b, var, key);
	return 0;
}

static int emit_expr(SqlBuf *b, const Expr *e, const Catalog *cat,
                    Bind *binds, int nb, Slot *slots, char **errmsg);

/*
 * Map a Cypher aggregate name (case-insensitively) to its SQL spelling, or
 * NULL if it is not one of the aggregates knit-graph supports. `collect`
 * becomes SQLite's json_group_array; the rest keep their name.
 */
static const char *agg_sql_name(const char *fn)
{
	if (strcasecmp(fn, "count") == 0)   return "count";
	if (strcasecmp(fn, "sum") == 0)     return "sum";
	if (strcasecmp(fn, "avg") == 0)     return "avg";
	if (strcasecmp(fn, "min") == 0)     return "min";
	if (strcasecmp(fn, "max") == 0)     return "max";
	if (strcasecmp(fn, "collect") == 0) return "json_group_array";
	return NULL;
}

/* Emit an aggregate call: count(*), count([DISTINCT] x), sum/avg/min/max(x),
 * collect(x) -> json_group_array(x). The lone argument may be any value
 * expression (in practice a property, which pulls in its node's join). */
static int emit_agg(SqlBuf *b, const Expr *e, const Catalog *cat,
                   Bind *binds, int nb, Slot *slots, char **errmsg)
{
	const char *fn = agg_sql_name(e->func);
	if (!fn)
		return fail(errmsg, "unsupported function in RETURN: %s()",
			e->func);

	if (e->func_star) {
		if (strcasecmp(e->func, "count") != 0)
			return fail(errmsg,
				"only count(*) takes '*'; %s() requires an argument",
				e->func);
		sqlbuf_append(b, "count(*)");
		return 0;
	}
	if (!e->args || e->args->next)
		return fail(errmsg, "%s() takes exactly one argument", e->func);

	sqlbuf_append(b, fn);
	sqlbuf_append_char(b, '(');
	if (e->func_distinct)
		sqlbuf_append(b, "DISTINCT ");
	if (emit_expr(b, e->args->expr, cat, binds, nb, slots, errmsg))
		return 1;
	sqlbuf_append_char(b, ')');
	return 0;
}

/*
 * Emit a whole-entity reference (RETURN a / RETURN r) as a json_object over the
 * entity's catalog columns in schema order: json_object('c', var."c", ...). A
 * node needs a label to know its table and is marked referenced so its table is
 * joined in; a relationship maps to the edge table, already in the FROM. The
 * variable is also the SQL alias (a whole entity always has a variable).
 */
static int emit_entity(SqlBuf *b, const char *var, const Catalog *cat,
                      Bind *binds, int nb, Slot *slots, char **errmsg)
{
	Bind *bd = find_bind(binds, nb, var);
	if (!bd)
		return fail(errmsg, "unknown variable: %s", var);
	if (!bd->table)
		return fail(errmsg,
			"cannot resolve columns of '%s': it has no label", var);

	const CatalogTable *t = catalog_find_table(cat, bd->table);
	if (!t)
		return fail(errmsg, "unknown table: %s", bd->table);

	if (bd->is_node)
		slots[bd->slot].referenced = 1;

	sqlbuf_append(b, "json_object(");
	for (int i = 0; i < t->ncolumns; i++) {
		if (i)
			sqlbuf_append(b, ", ");
		append_string(b, t->columns[i]);
		sqlbuf_append(b, ", ");
		append_column(b, var, t->columns[i]);
	}
	sqlbuf_append_char(b, ')');
	return 0;
}

/* A RETURN item is an aggregate when its expression is an aggregate call. */
static int item_is_aggregate(const ReturnItem *it)
{
	return it->expr->kind == EXPR_FUNC && agg_sql_name(it->expr->func);
}

/* Emit one RETURN/GROUP BY expression: a property or an aggregate call. */
static int emit_return_expr(SqlBuf *b, const Expr *e, const Catalog *cat,
                           Bind *binds, int nb, Slot *slots, char **errmsg)
{
	if (e->kind == EXPR_PROPERTY)
		return emit_property(b, e->var, e->key, cat, binds, nb, slots,
			errmsg);
	if (e->kind == EXPR_VARIABLE)
		return emit_entity(b, e->var, cat, binds, nb, slots, errmsg);
	if (e->kind == EXPR_FUNC)
		return emit_agg(b, e, cat, binds, nb, slots, errmsg);
	return fail(errmsg,
		"only property references (e.g. a.x), whole entities (e.g. a) "
		"and aggregate functions are supported in RETURN yet");
}

/* Does the RETURN list define `alias` (for an ORDER BY reference to it)? */
static int return_has_alias(const Return *r, const char *alias)
{
	for (ReturnItem *it = r->items; it; it = it->next)
		if (it->alias && strcmp(it->alias, alias) == 0)
			return 1;
	return 0;
}

/* Reject the RETURN constructs a later milestone will add, and validate the
 * projection tail's shape (SKIP/LIMIT must be integer literals). */
static int check_return_shape(const Return *r, char **errmsg)
{
	if (r->star)
		return fail(errmsg, "RETURN * is not supported yet");

	for (ReturnItem *it = r->items; it; it = it->next) {
		if (it->expr->kind != EXPR_PROPERTY
				&& it->expr->kind != EXPR_VARIABLE
				&& it->expr->kind != EXPR_FUNC)
			return fail(errmsg,
				"only property references (e.g. a.x), whole entities "
				"(e.g. a) and aggregate functions are supported in "
				"RETURN yet");
	}
	if (r->skip && !(r->skip->kind == EXPR_LITERAL
			&& r->skip->lit_kind == LIT_INT))
		return fail(errmsg, "SKIP requires an integer literal");
	if (r->limit && !(r->limit->kind == EXPR_LITERAL
			&& r->limit->lit_kind == LIT_INT))
		return fail(errmsg, "LIMIT requires an integer literal");
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
 * Resolve every RETURN property against the binds and emit the SELECT list.
 * Marks referenced nodes so the caller knows which joins to emit.
 */
static int emit_select(SqlBuf *sql, const Return *r, const Catalog *cat,
                      Bind *binds, int nb, Slot *slots, char **errmsg)
{
	sqlbuf_append(sql, "SELECT ");
	if (r->distinct)
		sqlbuf_append(sql, "DISTINCT ");
	int first = 1;
	for (ReturnItem *it = r->items; it; it = it->next) {
		if (!first)
			sqlbuf_append(sql, ", ");
		first = 0;

		if (emit_return_expr(sql, it->expr, cat, binds, nb, slots,
				errmsg))
			return 1;

		/* A whole entity is aliased to its variable name (so the JSON
		 * column's header is `a`, not the json_object(...) text) unless
		 * the query gives an explicit alias. */
		const char *alias = it->alias;
		if (!alias && it->expr->kind == EXPR_VARIABLE)
			alias = it->expr->var;
		if (alias) {
			sqlbuf_append(sql, " AS ");
			append_ident(sql, alias);
		}
	}
	return 0;
}

/*
 * Implicit GROUP BY: if any RETURN item aggregates, group by every
 * non-aggregated item. Emits nothing (leaving grp empty) when there is no
 * aggregate, or when every item aggregates (a single group over all rows).
 */
static int emit_group_by(SqlBuf *grp, const Return *r, const Catalog *cat,
                        Bind *binds, int nb, Slot *slots, char **errmsg)
{
	int has_agg = 0;
	for (ReturnItem *it = r->items; it; it = it->next)
		if (item_is_aggregate(it))
			has_agg = 1;
	if (!has_agg)
		return 0;

	int first = 1;
	for (ReturnItem *it = r->items; it; it = it->next) {
		if (item_is_aggregate(it))
			continue;
		sqlbuf_append(grp, first ? " GROUP BY " : ", ");
		first = 0;
		if (emit_return_expr(grp, it->expr, cat, binds, nb, slots,
				errmsg))
			return 1;
	}
	return 0;
}

/*
 * ORDER BY: each item is either a bare RETURN alias (emitted as a quoted
 * identifier SQLite resolves to the output column) or a property reference
 * (which pulls in its node's join, exactly as in SELECT/WHERE).
 */
static int emit_order_by(SqlBuf *ord, const Return *r, const Catalog *cat,
                        Bind *binds, int nb, Slot *slots, char **errmsg)
{
	if (!r->order)
		return 0;

	int first = 1;
	for (SortItem *si = r->order; si; si = si->next) {
		sqlbuf_append(ord, first ? " ORDER BY " : ", ");
		first = 0;
		if (si->expr->kind == EXPR_VARIABLE) {
			if (!return_has_alias(r, si->expr->var))
				return fail(errmsg,
					"ORDER BY refers to unknown name: %s",
					si->expr->var);
			append_ident(ord, si->expr->var);
		} else if (si->expr->kind == EXPR_PROPERTY) {
			if (emit_property(ord, si->expr->var, si->expr->key, cat,
					binds, nb, slots, errmsg))
				return 1;
		} else {
			return fail(errmsg, "ORDER BY supports property "
				"references and RETURN aliases only");
		}
		if (si->desc)
			sqlbuf_append(ord, " DESC");
	}
	return 0;
}

/* SKIP/LIMIT -> LIMIT/OFFSET. SKIP without LIMIT uses LIMIT -1 (unbounded). */
static void emit_limit_offset(SqlBuf *out, const Return *r)
{
	if (!r->skip && !r->limit)
		return;
	if (r->limit) {
		sqlbuf_append(out, " LIMIT ");
		sqlbuf_append(out, r->limit->lit_text);
	} else {
		sqlbuf_append(out, " LIMIT -1");
	}
	if (r->skip) {
		sqlbuf_append(out, " OFFSET ");
		sqlbuf_append(out, r->skip->lit_text);
	}
}

/* --------------------------------------------------------------- WHERE --- */

static const char *cmp_op_sql(CmpOp op)
{
	switch (op) {
	case CMP_EQ: return "=";
	case CMP_NE: return "<>";
	case CMP_LT: return "<";
	case CMP_GT: return ">";
	case CMP_LE: return "<=";
	case CMP_GE: return ">=";
	}
	return "=";
}

/* Emit a literal value: numbers verbatim, strings quoted, bool as 1/0, NULL. */
static void emit_literal(SqlBuf *b, const Expr *e)
{
	switch (e->lit_kind) {
	case LIT_INT:
	case LIT_REAL:
		sqlbuf_append(b, e->lit_text);
		break;
	case LIT_STRING:
		append_string(b, e->lit_text);
		break;
	case LIT_BOOL:
		sqlbuf_append(b, e->lit_bool ? "1" : "0");
		break;
	case LIT_NULL:
		sqlbuf_append(b, "NULL");
		break;
	}
}

/*
 * Emit a LIKE pattern literal built from a plain string. `pre`/`post` are the
 * surrounding wildcards ("" or "%"). The string's own LIKE metacharacters
 * (% _ \) are backslash-escaped so only pre/post act as wildcards; the caller
 * pairs this with `ESCAPE '\'`.
 */
static void append_like_pattern(SqlBuf *b, const char *pre, const char *s,
                               const char *post)
{
	sqlbuf_append_char(b, '\'');
	sqlbuf_append(b, pre);
	for (const char *p = s; *p; p++) {
		if (*p == '%' || *p == '_' || *p == '\\')
			sqlbuf_append_char(b, '\\');
		if (*p == '\'')
			sqlbuf_append(b, "''");
		else
			sqlbuf_append_char(b, *p);
	}
	sqlbuf_append(b, post);
	sqlbuf_append_char(b, '\'');
}

/* left STARTS WITH/ENDS WITH/CONTAINS 'lit' -> left LIKE 'pattern' ESCAPE '\'. */
static int emit_like(SqlBuf *b, const Expr *e, const char *pre, const char *post,
                    const Catalog *cat, Bind *binds, int nb, Slot *slots,
                    char **errmsg)
{
	if (e->right->kind != EXPR_LITERAL || e->right->lit_kind != LIT_STRING)
		return fail(errmsg,
			"STARTS WITH / ENDS WITH / CONTAINS require a string literal");

	if (emit_expr(b, e->left, cat, binds, nb, slots, errmsg))
		return 1;
	sqlbuf_append(b, " LIKE ");
	append_like_pattern(b, pre, e->right->lit_text, post);
	sqlbuf_append(b, " ESCAPE '\\'");
	return 0;
}

/* Recursively emit a boolean/value expression as SQL. */
static int emit_expr(SqlBuf *b, const Expr *e, const Catalog *cat,
                    Bind *binds, int nb, Slot *slots, char **errmsg)
{
	switch (e->kind) {
	case EXPR_OR:
	case EXPR_AND: {
		const char *op = (e->kind == EXPR_OR) ? " OR " : " AND ";
		sqlbuf_append_char(b, '(');
		if (emit_expr(b, e->left, cat, binds, nb, slots, errmsg))
			return 1;
		sqlbuf_append(b, op);
		if (emit_expr(b, e->right, cat, binds, nb, slots, errmsg))
			return 1;
		sqlbuf_append_char(b, ')');
		return 0;
	}
	case EXPR_NOT:
		sqlbuf_append(b, "(NOT ");
		if (emit_expr(b, e->left, cat, binds, nb, slots, errmsg))
			return 1;
		sqlbuf_append_char(b, ')');
		return 0;
	case EXPR_CMP:
		if (emit_expr(b, e->left, cat, binds, nb, slots, errmsg))
			return 1;
		sqlbuf_append_char(b, ' ');
		sqlbuf_append(b, cmp_op_sql(e->cmp));
		sqlbuf_append_char(b, ' ');
		return emit_expr(b, e->right, cat, binds, nb, slots, errmsg);
	case EXPR_IN: {
		if (emit_expr(b, e->left, cat, binds, nb, slots, errmsg))
			return 1;
		sqlbuf_append(b, " IN (");
		int first = 1;
		for (ExprList *l = e->args; l; l = l->next) {
			if (!first)
				sqlbuf_append(b, ", ");
			first = 0;
			if (emit_expr(b, l->expr, cat, binds, nb, slots, errmsg))
				return 1;
		}
		sqlbuf_append_char(b, ')');
		return 0;
	}
	case EXPR_IS_NULL:
		if (emit_expr(b, e->left, cat, binds, nb, slots, errmsg))
			return 1;
		sqlbuf_append(b, " IS NULL");
		return 0;
	case EXPR_IS_NOT_NULL:
		if (emit_expr(b, e->left, cat, binds, nb, slots, errmsg))
			return 1;
		sqlbuf_append(b, " IS NOT NULL");
		return 0;
	case EXPR_STARTS_WITH:
		return emit_like(b, e, "", "%", cat, binds, nb, slots, errmsg);
	case EXPR_ENDS_WITH:
		return emit_like(b, e, "%", "", cat, binds, nb, slots, errmsg);
	case EXPR_CONTAINS:
		return emit_like(b, e, "%", "%", cat, binds, nb, slots, errmsg);
	case EXPR_PROPERTY:
		return emit_property(b, e->var, e->key, cat, binds, nb, slots,
			errmsg);
	case EXPR_LITERAL:
		emit_literal(b, e);
		return 0;
	case EXPR_VARIABLE:
		return fail(errmsg,
			"bare variable '%s' is not supported in WHERE yet", e->var);
	case EXPR_FUNC:
		return fail(errmsg, "functions are not supported in WHERE yet");
	}
	return fail(errmsg, "unsupported expression in WHERE");
}

/* ---------------------------------------------------- general engine --- */

/* Allocate a small owned identifier (a generated alias). Aborts on OOM. */
static char *own_alias(char **owned, int *nowned, const char *fmt, int n)
{
	char buf[32];
	snprintf(buf, sizeof buf, fmt, n);
	char *s = strdup(buf);
	if (!s) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	owned[(*nowned)++] = s;
	return s;
}

/*
 * Find or create the slot for a node occurrence. Nodes are matched by variable
 * name (so a variable shared across hops/patterns is one slot); anonymous nodes
 * match by their AST pointer. On a repeat occurrence the label is merged, and a
 * conflicting label is an error (returns -1).
 */
static int slot_for(Slot *slots, int *nslots, const NodePat *np, char **errmsg)
{
	for (int i = 0; i < *nslots; i++) {
		int same = (np->var && slots[i].var
				&& strcmp(slots[i].var, np->var) == 0)
			|| (!np->var && !slots[i].var && slots[i].np == np);
		if (!same)
			continue;
		if (np->labels) {
			const char *lbl = np->labels->s;
			if (slots[i].label && strcmp(slots[i].label, lbl) != 0) {
				fail(errmsg, "conflicting labels for '%s'",
					np->var ? np->var : "(anonymous)");
				return -1;
			}
			slots[i].label = lbl;
		}
		return i;
	}

	int i = (*nslots)++;
	slots[i].var = np->var;
	slots[i].np = np;
	slots[i].label = np->labels ? np->labels->s : NULL;
	slots[i].alias = np->var;
	slots[i].referenced = 0;
	slots[i].is_lone = 0;
	slots[i].anchor_edge = -1;
	slots[i].anchor_target = 0;
	return i;
}

/*
 * If slot `sl` -- appearing on `side_target` of edge `ei` -- was anchored to an
 * earlier edge, emit the id/name equality tying this edge's side to that anchor.
 */
static void emit_tie(SqlBuf *b, int *first, const Edge *edges, int ei,
                    int sl, int side_target, const Slot *slots)
{
	if (slots[sl].anchor_edge == ei)
		return; /* anchored here: this edge defines the slot's identity */

	const Edge *ae = &edges[slots[sl].anchor_edge];
	const char *side_id   = side_target ? "target_id"   : "source_id";
	const char *side_name = side_target ? "target_name" : "source_name";
	const char *anc_id    = slots[sl].anchor_target ? "target_id"   : "source_id";
	const char *anc_name  = slots[sl].anchor_target ? "target_name" : "source_name";

	if (!*first)
		sqlbuf_append(b, " AND ");
	*first = 0;
	append_column(b, edges[ei].alias, side_id);
	sqlbuf_append(b, " = ");
	append_column(b, ae->alias, anc_id);
	sqlbuf_append(b, " AND ");
	append_column(b, edges[ei].alias, side_name);
	sqlbuf_append(b, " = ");
	append_column(b, ae->alias, anc_name);
}

/*
 * Lower an inline relationship property map -- `-[{alias:'fast'}]->` -- to the
 * same edge-column predicates the WHERE form (`e.alias = 'fast'`) produces:
 * each `key: value` becomes `<edge>."key" = <value>`, ANDed onto the pattern
 * predicates. The key must be a real column of the edge table and the value a
 * literal. `pfirst` tracks whether any predicate has been emitted yet.
 */
static int emit_rel_props(SqlBuf *pred, int *pfirst, const char *edge_alias,
                         const PropList *props, const Catalog *cat,
                         char **errmsg)
{
	if (!props)
		return 0;

	const CatalogTable *et = catalog_find_table(cat, KG_EDGE_TABLE);
	for (const PropList *p = props; p; p = p->next) {
		if (!et || !catalog_table_has_column(et, p->key))
			return fail(errmsg, "unknown column: %s.%s",
				KG_EDGE_TABLE, p->key);
		if (p->value->kind != EXPR_LITERAL)
			return fail(errmsg, "inline relationship property '%s' "
				"must be a literal value", p->key);
		if (!*pfirst)
			sqlbuf_append(pred, " AND ");
		*pfirst = 0;
		append_column(pred, edge_alias, p->key);
		sqlbuf_append(pred, " = ");
		emit_literal(pred, p->value);
	}
	return 0;
}

static int transform_general(const Match *m, const Return *r,
                            const Catalog *cat, const NameMap *map,
                            char **sql, char **errmsg)
{
	/* Upper bounds: one slot per node occurrence, one edge per segment. */
	int cap_nodes = 0, cap_edges = 0;
	for (PathList *pl = m->patterns; pl; pl = pl->next) {
		cap_nodes++;
		for (Segment *s = pl->path->segments; s; s = s->next) {
			cap_nodes++;
			cap_edges++;
		}
	}

	Slot *slots = calloc((size_t)cap_nodes + 1, sizeof *slots);
	Edge *edges = calloc((size_t)cap_edges + 1, sizeof *edges);
	Bind *binds = calloc((size_t)cap_nodes + cap_edges + 1, sizeof *binds);
	char **owned = calloc((size_t)cap_nodes + cap_edges + 1, sizeof *owned);
	if (!slots || !edges || !binds || !owned) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	int nslots = 0, nedges = 0, nb = 0, nowned = 0, rc = 1;

	SqlBuf sel, uw, from, pred, grp, ord, out;
	sqlbuf_init(&sel);
	sqlbuf_init(&uw);
	sqlbuf_init(&from);
	sqlbuf_init(&pred);
	sqlbuf_init(&grp);
	sqlbuf_init(&ord);
	sqlbuf_init(&out);

	/* Collect slots and edges, and anchor each slot to its first edge. */
	for (PathList *pl = m->patterns; pl; pl = pl->next) {
		Path *p = pl->path;
		if (check_node(p->start, errmsg))
			goto done;
		int prev = slot_for(slots, &nslots, p->start, errmsg);
		if (prev < 0)
			goto done;

		for (Segment *s = p->segments; s; s = s->next) {
			RelPat *rel = s->rel;
			if (check_node(s->node, errmsg))
				goto done;
			if (rel->types && rel->types->next) {
				fail(errmsg,
					"multiple relationship types are not supported yet");
				goto done;
			}

			int cur = slot_for(slots, &nslots, s->node, errmsg);
			if (cur < 0)
				goto done;

			int src, tgt;
			if (rel->dir == REL_LEFT) {
				src = cur;
				tgt = prev;
			} else { /* REL_RIGHT */
				src = prev;
				tgt = cur;
			}
			int ei = nedges++;
			edges[ei].var = rel->var;
			edges[ei].type = rel->types ? rel->types->s : NULL;
			edges[ei].props = rel->props;
			edges[ei].src = src;
			edges[ei].tgt = tgt;
			if (slots[src].anchor_edge == -1) {
				slots[src].anchor_edge = ei;
				slots[src].anchor_target = 0;
			}
			if (slots[tgt].anchor_edge == -1) {
				slots[tgt].anchor_edge = ei;
				slots[tgt].anchor_target = 1;
			}
			prev = cur;
		}
	}

	/* Resolve each labelled slot through the name map: `label` becomes the
	 * table to JOIN, `name` the *_name value its edge filter matches. */
	for (int i = 0; i < nslots; i++) {
		const char *table, *name;
		if (names_resolve(map, slots[i].label, &table, &name, errmsg))
			goto done;
		slots[i].label = table;
		slots[i].name = name;
	}

	/* Name each edge (variable, or a generated alias) . */
	for (int i = 0; i < nedges; i++) {
		if (edges[i].var)
			edges[i].alias = edges[i].var;
		else if (nedges == 1)
			edges[i].alias = "r";
		else
			edges[i].alias = own_alias(owned, &nowned, "_e%d", i);
	}

	/* Mark lone nodes, validate labels resolve, and give lone nodes aliases. */
	for (int i = 0; i < nslots; i++) {
		slots[i].is_lone = (slots[i].anchor_edge == -1);
		if (slots[i].label && !catalog_find_table(cat, slots[i].label)) {
			fail(errmsg, "unknown table: %s", slots[i].label);
			goto done;
		}
		if (slots[i].is_lone) {
			if (!slots[i].label) {
				fail(errmsg, "node has no label; cannot determine "
					"which table to query");
				goto done;
			}
			if (!slots[i].alias)
				slots[i].alias =
					own_alias(owned, &nowned, "_n%d", i);
		}
	}

	/* Binds for RETURN/WHERE resolution: node slots and edges by variable. */
	for (int i = 0; i < nslots; i++)
		if (slots[i].var)
			binds[nb++] = (Bind){ slots[i].var, slots[i].label, 1, i };
	for (int i = 0; i < nedges; i++)
		if (edges[i].var)
			binds[nb++] = (Bind){ edges[i].var, KG_EDGE_TABLE, 0, -1 };

	/* SELECT and the user WHERE, emitted before the joins so any node they
	 * reference is marked and gets its table joined in. */
	if (emit_select(&sel, r, cat, binds, nb, slots, errmsg))
		goto done;
	if (m->where && emit_expr(&uw, m->where, cat, binds, nb, slots, errmsg))
		goto done;
	if (emit_order_by(&ord, r, cat, binds, nb, slots, errmsg))
		goto done;
	if (emit_group_by(&grp, r, cat, binds, nb, slots, errmsg))
		goto done;

	/* FROM: the first edge (or, edgeless, the first lone node) is the base;
	 * further edges JOIN on their ties, further lone nodes cross-join. */
	int base_done = 0;
	if (nedges > 0) {
		sqlbuf_append(&from, " FROM ");
		append_ident(&from, KG_EDGE_TABLE);
		sqlbuf_append_char(&from, ' ');
		sqlbuf_append(&from, edges[0].alias);
		base_done = 1;
		for (int i = 1; i < nedges; i++) {
			sqlbuf_append(&from, " JOIN ");
			append_ident(&from, KG_EDGE_TABLE);
			sqlbuf_append_char(&from, ' ');
			sqlbuf_append(&from, edges[i].alias);
			sqlbuf_append(&from, " ON ");
			int tfirst = 1;
			emit_tie(&from, &tfirst, edges, i, edges[i].src, 0, slots);
			emit_tie(&from, &tfirst, edges, i, edges[i].tgt, 1, slots);
			if (tfirst)
				sqlbuf_append(&from, "1 = 1");
		}
	}
	for (int i = 0; i < nslots; i++) {
		if (!slots[i].is_lone)
			continue;
		if (!base_done) {
			sqlbuf_append(&from, " FROM ");
			append_ident(&from, slots[i].label);
			sqlbuf_append_char(&from, ' ');
			sqlbuf_append(&from, slots[i].alias);
			base_done = 1;
		} else {
			sqlbuf_append(&from, " JOIN ");
			append_ident(&from, slots[i].label);
			sqlbuf_append_char(&from, ' ');
			sqlbuf_append(&from, slots[i].alias);
			sqlbuf_append(&from, " ON 1 = 1");
		}
	}
	/* Referenced non-lone nodes: join their function table to their anchor. */
	for (int i = 0; i < nslots; i++) {
		if (slots[i].is_lone || !slots[i].referenced)
			continue;
		const Edge *ae = &edges[slots[i].anchor_edge];
		const char *idcol =
			slots[i].anchor_target ? "target_id" : "source_id";
		sqlbuf_append(&from, " JOIN ");
		append_ident(&from, slots[i].label);
		sqlbuf_append_char(&from, ' ');
		sqlbuf_append(&from, slots[i].alias);
		sqlbuf_append(&from, " ON ");
		append_column(&from, slots[i].alias, "id");
		sqlbuf_append(&from, " = ");
		append_column(&from, ae->alias, idcol);
	}

	/* Pattern predicates: per edge, its type and its labelled endpoints. */
	int pfirst = 1;
	for (int i = 0; i < nedges; i++) {
		if (edges[i].type) {
			if (!pfirst)
				sqlbuf_append(&pred, " AND ");
			pfirst = 0;
			append_column(&pred, edges[i].alias, "edge_type");
			sqlbuf_append(&pred, " = ");
			append_string(&pred, edges[i].type);
		}
		const char *sl = slots[edges[i].src].name;
		if (sl) {
			if (!pfirst)
				sqlbuf_append(&pred, " AND ");
			pfirst = 0;
			append_column(&pred, edges[i].alias, "source_name");
			sqlbuf_append(&pred, " = ");
			append_string(&pred, sl);
		}
		const char *tl = slots[edges[i].tgt].name;
		if (tl) {
			if (!pfirst)
				sqlbuf_append(&pred, " AND ");
			pfirst = 0;
			append_column(&pred, edges[i].alias, "target_name");
			sqlbuf_append(&pred, " = ");
			append_string(&pred, tl);
		}
		if (emit_rel_props(&pred, &pfirst, edges[i].alias,
				edges[i].props, cat, errmsg))
			goto done;
	}

	sqlbuf_append(&out, sel.data ? sel.data : "");
	sqlbuf_append(&out, from.data ? from.data : "");
	int have_pred = pred.len > 0;
	int have_user = uw.len > 0;
	if (have_pred || have_user) {
		sqlbuf_append(&out, " WHERE ");
		if (have_pred)
			sqlbuf_append(&out, pred.data);
		if (have_pred && have_user)
			sqlbuf_append(&out, " AND ");
		if (have_user)
			sqlbuf_append(&out, uw.data);
	}
	if (grp.len > 0)
		sqlbuf_append(&out, grp.data);
	if (ord.len > 0)
		sqlbuf_append(&out, ord.data);
	emit_limit_offset(&out, r);

	*sql = sqlbuf_detach(&out);
	rc = 0;

done:
	sqlbuf_free(&sel);
	sqlbuf_free(&uw);
	sqlbuf_free(&from);
	sqlbuf_free(&pred);
	sqlbuf_free(&grp);
	sqlbuf_free(&ord);
	sqlbuf_free(&out);
	for (int i = 0; i < nowned; i++)
		free(owned[i]);
	free(owned);
	free(slots);
	free(edges);
	free(binds);
	return rc;
}

/* ------------------------------------------------------ undirected hop --- */

/* Emit one endpoint's terms for a given edge side (id if joined, name if
 * labelled), separated from earlier terms by " AND ". */
static void orient_side(SqlBuf *o, int *first, const char *edge,
                       const char *side, const Slot *s)
{
	char idcol[16], namecol[16];
	snprintf(idcol, sizeof idcol, "%s_id", side);
	snprintf(namecol, sizeof namecol, "%s_name", side);

	if (s->referenced) {
		if (!*first)
			sqlbuf_append(o, " AND ");
		*first = 0;
		append_column(o, edge, idcol);
		sqlbuf_append(o, " = ");
		append_column(o, s->alias, "id");
	}
	if (s->name) {
		if (!*first)
			sqlbuf_append(o, " AND ");
		*first = 0;
		append_column(o, edge, namecol);
		sqlbuf_append(o, " = ");
		append_string(o, s->name);
	}
}

/* Build one orientation: node u on side `su`, node v on side `sv`. */
static void build_orient(SqlBuf *o, const char *edge, const char *su,
                        const char *sv, const Slot *u, const Slot *v)
{
	int first = 1;
	orient_side(o, &first, edge, su, u);
	orient_side(o, &first, edge, sv, v);
}

static int transform_undirected(const NodePat *left, const RelPat *rel,
                               const NodePat *right, const Return *r,
                               const Expr *where, const Catalog *cat,
                               const NameMap *map, char **sql, char **errmsg)
{
	if (check_node(left, errmsg) || check_node(right, errmsg))
		return 1;
	if (rel->types && rel->types->next)
		return fail(errmsg,
			"multiple relationship types are not supported yet");

	const char *u_label = left->labels ? left->labels->s : NULL;
	const char *v_label = right->labels ? right->labels->s : NULL;
	const char *u_table, *u_name, *v_table, *v_name;
	if (names_resolve(map, u_label, &u_table, &u_name, errmsg)
			|| names_resolve(map, v_label, &v_table, &v_name, errmsg))
		return 1;
	if (u_table && !catalog_find_table(cat, u_table))
		return fail(errmsg, "unknown table: %s", u_table);
	if (v_table && !catalog_find_table(cat, v_table))
		return fail(errmsg, "unknown table: %s", v_table);

	const char *edge = rel->var ? rel->var : "r";
	const char *type = rel->types ? rel->types->s : NULL;

	/* Slot 0 = left (u), slot 1 = right (v). */
	Slot slots[2];
	memset(slots, 0, sizeof slots);
	slots[0].var = left->var;
	slots[0].label = u_table;
	slots[0].name = u_name;
	slots[0].alias = left->var;
	slots[0].anchor_edge = -1;
	slots[1].var = right->var;
	slots[1].label = v_table;
	slots[1].name = v_name;
	slots[1].alias = right->var;
	slots[1].anchor_edge = -1;

	Bind binds[3];
	int nb = 0;
	if (left->var)
		binds[nb++] = (Bind){ left->var, u_table, 1, 0 };
	if (right->var)
		binds[nb++] = (Bind){ right->var, v_table, 1, 1 };
	if (rel->var)
		binds[nb++] = (Bind){ rel->var, KG_EDGE_TABLE, 0, -1 };

	SqlBuf sel, uw, pred, o1, o2, grp, ord, out;
	sqlbuf_init(&sel);
	sqlbuf_init(&uw);
	sqlbuf_init(&pred);
	sqlbuf_init(&o1);
	sqlbuf_init(&o2);
	sqlbuf_init(&grp);
	sqlbuf_init(&ord);
	sqlbuf_init(&out);
	int rc = 1;

	if (emit_select(&sel, r, cat, binds, nb, slots, errmsg))
		goto done;
	if (where && emit_expr(&uw, where, cat, binds, nb, slots, errmsg))
		goto done;
	if (emit_order_by(&ord, r, cat, binds, nb, slots, errmsg))
		goto done;
	if (emit_group_by(&grp, r, cat, binds, nb, slots, errmsg))
		goto done;

	/* A referenced endpoint needs a label to know which table to join. */
	for (int i = 0; i < 2; i++) {
		if (slots[i].referenced && !slots[i].label) {
			fail(errmsg,
				"cannot resolve columns of '%s': it has no label",
				slots[i].var);
			goto done;
		}
	}

	sqlbuf_append(&out, sel.data ? sel.data : "");
	sqlbuf_append(&out, " FROM ");
	append_ident(&out, KG_EDGE_TABLE);
	sqlbuf_append_char(&out, ' ');
	sqlbuf_append(&out, edge);
	for (int i = 0; i < 2; i++) {
		if (!slots[i].referenced)
			continue;
		sqlbuf_append(&out, " JOIN ");
		append_ident(&out, slots[i].label);
		sqlbuf_append_char(&out, ' ');
		sqlbuf_append(&out, slots[i].alias);
		sqlbuf_append(&out, " ON 1 = 1");
	}

	int first = 1;
	if (type) {
		append_column(&pred, edge, "edge_type");
		sqlbuf_append(&pred, " = ");
		append_string(&pred, type);
		first = 0;
	}
	/* OR over both orientations of the undirected hop. */
	build_orient(&o1, edge, "source", "target", &slots[0], &slots[1]);
	build_orient(&o2, edge, "target", "source", &slots[0], &slots[1]);
	if (o1.len > 0) {
		if (!first)
			sqlbuf_append(&pred, " AND ");
		first = 0;
		sqlbuf_append(&pred, "(");
		sqlbuf_append(&pred, o1.data);
		sqlbuf_append(&pred, " OR ");
		sqlbuf_append(&pred, o2.data);
		sqlbuf_append(&pred, ")");
	}
	if (emit_rel_props(&pred, &first, edge, rel->props, cat, errmsg))
		goto done;

	int have_pred = pred.len > 0;
	int have_user = uw.len > 0;
	if (have_pred || have_user) {
		sqlbuf_append(&out, " WHERE ");
		if (have_pred)
			sqlbuf_append(&out, pred.data);
		if (have_pred && have_user)
			sqlbuf_append(&out, " AND ");
		if (have_user)
			sqlbuf_append(&out, uw.data);
	}
	if (grp.len > 0)
		sqlbuf_append(&out, grp.data);
	if (ord.len > 0)
		sqlbuf_append(&out, ord.data);
	emit_limit_offset(&out, r);

	*sql = sqlbuf_detach(&out);
	rc = 0;

done:
	sqlbuf_free(&sel);
	sqlbuf_free(&uw);
	sqlbuf_free(&pred);
	sqlbuf_free(&o1);
	sqlbuf_free(&o2);
	sqlbuf_free(&grp);
	sqlbuf_free(&ord);
	sqlbuf_free(&out);
	return rc;
}

/* ------------------------------------------------- variable-length hop --- */

#define KG_WALK_CTE "walk"

/*
 * A variable-length hop `(a)-[:t*m..n]->(b)` compiled to a recursive CTE.
 *
 * The CTE `walk` carries, per path, the fixed original source
 * (source_id, source_name), the current frontier (target_id, target_name), the
 * hop count `depth`, and `path` -- the '/'-delimited list of edge rowids already
 * traversed. The base member is every type-matching edge (depth 1); the
 * recursive member extends a path by an edge whose source meets the current
 * frontier, provided that edge's rowid is not already in `path`. That edge
 * uniqueness is Cypher's relationship-uniqueness and, since the edge set is
 * finite, guarantees termination even when `n` is unbounded.
 *
 * The outer query then treats `walk` exactly like one edge instance: the
 * source-side and target-side nodes are joined to their function tables when
 * referenced and constrained by source_name/target_name when labelled, and
 * `depth >= m` enforces the lower bound (the upper bound is enforced inside the
 * recursion). Only a single directed hop is handled; a bound relationship
 * variable is rejected (its Cypher value is a list, unsupported here).
 */
static int transform_varlen(const NodePat *left, const RelPat *rel,
                           const NodePat *right, const Return *r,
                           const Expr *where, const Catalog *cat,
                           const NameMap *map, char **sql, char **errmsg)
{
	if (check_node(left, errmsg) || check_node(right, errmsg))
		return 1;
	if (rel->props)
		return fail(errmsg, "inline property maps are not supported on "
			"variable-length relationships");
	if (rel->types && rel->types->next)
		return fail(errmsg,
			"multiple relationship types are not supported yet");
	if (rel->dir == REL_UNDIR)
		return fail(errmsg, "undirected variable-length relationships "
			"are not supported yet");
	if (rel->var)
		return fail(errmsg, "a variable cannot be bound to a "
			"variable-length relationship");

	/* Bounds: Cypher's default lower bound is 1; the upper may be open. */
	const VarLen *vl = &rel->varlen;
	int min = vl->has_min ? vl->min : 1;
	if (min < 1)
		return fail(errmsg,
			"variable-length lower bound must be at least 1");
	if (vl->has_max && vl->max < 1)
		return fail(errmsg,
			"variable-length upper bound must be at least 1");
	if (vl->has_min && vl->has_max && vl->min > vl->max)
		return fail(errmsg,
			"variable-length lower bound exceeds upper bound");

	const char *u_label = left->labels ? left->labels->s : NULL;
	const char *v_label = right->labels ? right->labels->s : NULL;
	const char *u_table, *u_name, *v_table, *v_name;
	if (names_resolve(map, u_label, &u_table, &u_name, errmsg)
			|| names_resolve(map, v_label, &v_table, &v_name, errmsg))
		return 1;
	if (u_table && !catalog_find_table(cat, u_table))
		return fail(errmsg, "unknown table: %s", u_table);
	if (v_table && !catalog_find_table(cat, v_table))
		return fail(errmsg, "unknown table: %s", v_table);

	const char *type = rel->types ? rel->types->s : NULL;
	const char *edge = "r"; /* the walk CTE's alias in the outer query */

	/* Slot 0 = left node, slot 1 = right node. */
	Slot slots[2];
	memset(slots, 0, sizeof slots);
	slots[0].var = left->var;
	slots[0].label = u_table;
	slots[0].name = u_name;
	slots[0].alias = left->var;
	slots[0].anchor_edge = -1;
	slots[1].var = right->var;
	slots[1].label = v_table;
	slots[1].name = v_name;
	slots[1].alias = right->var;
	slots[1].anchor_edge = -1;

	Bind binds[2];
	int nb = 0;
	if (left->var)
		binds[nb++] = (Bind){ left->var, u_table, 1, 0 };
	if (right->var)
		binds[nb++] = (Bind){ right->var, v_table, 1, 1 };

	SqlBuf sel, uw, pred, grp, ord, out;
	sqlbuf_init(&sel);
	sqlbuf_init(&uw);
	sqlbuf_init(&pred);
	sqlbuf_init(&grp);
	sqlbuf_init(&ord);
	sqlbuf_init(&out);
	int rc = 1;

	if (emit_select(&sel, r, cat, binds, nb, slots, errmsg))
		goto done;
	if (where && emit_expr(&uw, where, cat, binds, nb, slots, errmsg))
		goto done;
	if (emit_order_by(&ord, r, cat, binds, nb, slots, errmsg))
		goto done;
	if (emit_group_by(&grp, r, cat, binds, nb, slots, errmsg))
		goto done;

	/* A referenced endpoint needs a label to know which table to join. */
	for (int i = 0; i < 2; i++) {
		if (slots[i].referenced && !slots[i].label) {
			fail(errmsg,
				"cannot resolve columns of '%s': it has no label",
				slots[i].var);
			goto done;
		}
	}

	/* Map the pattern's endpoints onto the edge's source/target side: `->`
	 * puts the left node on the source side, `<-` swaps them. */
	int src_i = (rel->dir == REL_LEFT) ? 1 : 0;
	int tgt_i = (rel->dir == REL_LEFT) ? 0 : 1;
	struct { int slot; const char *idcol; const char *namecol; } ends[2] = {
		{ src_i, "source_id", "source_name" },
		{ tgt_i, "target_id", "target_name" },
	};

	/* WITH RECURSIVE walk(...) AS ( base UNION ALL recursive ). */
	sqlbuf_append(&out, "WITH RECURSIVE ");
	append_ident(&out, KG_WALK_CTE);
	sqlbuf_append(&out, "(source_id, source_name, target_id, target_name, "
		"depth, path) AS (SELECT ");
	append_column(&out, "e", "source_id");
	sqlbuf_append(&out, ", ");
	append_column(&out, "e", "source_name");
	sqlbuf_append(&out, ", ");
	append_column(&out, "e", "target_id");
	sqlbuf_append(&out, ", ");
	append_column(&out, "e", "target_name");
	sqlbuf_append(&out, ", 1, '/' || e.rowid || '/' FROM ");
	append_ident(&out, KG_EDGE_TABLE);
	sqlbuf_append(&out, " e");
	if (type) {
		sqlbuf_append(&out, " WHERE ");
		append_column(&out, "e", "edge_type");
		sqlbuf_append(&out, " = ");
		append_string(&out, type);
	}
	sqlbuf_append(&out, " UNION ALL SELECT ");
	append_column(&out, "w", "source_id");
	sqlbuf_append(&out, ", ");
	append_column(&out, "w", "source_name");
	sqlbuf_append(&out, ", ");
	append_column(&out, "e", "target_id");
	sqlbuf_append(&out, ", ");
	append_column(&out, "e", "target_name");
	sqlbuf_append(&out, ", ");
	append_column(&out, "w", "depth");
	sqlbuf_append(&out, " + 1, ");
	append_column(&out, "w", "path");
	sqlbuf_append(&out, " || e.rowid || '/' FROM ");
	append_ident(&out, KG_WALK_CTE);
	sqlbuf_append(&out, " w JOIN ");
	append_ident(&out, KG_EDGE_TABLE);
	sqlbuf_append(&out, " e ON ");
	append_column(&out, "e", "source_id");
	sqlbuf_append(&out, " = ");
	append_column(&out, "w", "target_id");
	sqlbuf_append(&out, " AND ");
	append_column(&out, "e", "source_name");
	sqlbuf_append(&out, " = ");
	append_column(&out, "w", "target_name");
	sqlbuf_append(&out, " WHERE ");
	if (type) {
		append_column(&out, "e", "edge_type");
		sqlbuf_append(&out, " = ");
		append_string(&out, type);
		sqlbuf_append(&out, " AND ");
	}
	if (vl->has_max) {
		char buf[32];
		append_column(&out, "w", "depth");
		snprintf(buf, sizeof buf, " < %d AND ", vl->max);
		sqlbuf_append(&out, buf);
	}
	append_column(&out, "w", "path");
	sqlbuf_append(&out, " NOT LIKE '%/' || e.rowid || '/%') ");

	/* Outer query: the walk stands in for a single edge instance. */
	sqlbuf_append(&out, sel.data ? sel.data : "");
	sqlbuf_append(&out, " FROM ");
	append_ident(&out, KG_WALK_CTE);
	sqlbuf_append_char(&out, ' ');
	sqlbuf_append(&out, edge);
	for (int k = 0; k < 2; k++) {
		Slot *s = &slots[ends[k].slot];
		if (!s->referenced)
			continue;
		sqlbuf_append(&out, " JOIN ");
		append_ident(&out, s->label);
		sqlbuf_append_char(&out, ' ');
		sqlbuf_append(&out, s->alias);
		sqlbuf_append(&out, " ON ");
		append_column(&out, s->alias, "id");
		sqlbuf_append(&out, " = ");
		append_column(&out, edge, ends[k].idcol);
	}

	/* Endpoint label predicates and the lower-bound depth filter. */
	int pfirst = 1;
	for (int k = 0; k < 2; k++) {
		Slot *s = &slots[ends[k].slot];
		if (!s->name)
			continue;
		if (!pfirst)
			sqlbuf_append(&pred, " AND ");
		pfirst = 0;
		append_column(&pred, edge, ends[k].namecol);
		sqlbuf_append(&pred, " = ");
		append_string(&pred, s->name);
	}
	if (min > 1) {
		char buf[32];
		if (!pfirst)
			sqlbuf_append(&pred, " AND ");
		pfirst = 0;
		append_column(&pred, edge, "depth");
		snprintf(buf, sizeof buf, " >= %d", min);
		sqlbuf_append(&pred, buf);
	}

	int have_pred = pred.len > 0;
	int have_user = uw.len > 0;
	if (have_pred || have_user) {
		sqlbuf_append(&out, " WHERE ");
		if (have_pred)
			sqlbuf_append(&out, pred.data);
		if (have_pred && have_user)
			sqlbuf_append(&out, " AND ");
		if (have_user)
			sqlbuf_append(&out, uw.data);
	}
	if (grp.len > 0)
		sqlbuf_append(&out, grp.data);
	if (ord.len > 0)
		sqlbuf_append(&out, ord.data);
	emit_limit_offset(&out, r);

	*sql = sqlbuf_detach(&out);
	rc = 0;

done:
	sqlbuf_free(&sel);
	sqlbuf_free(&uw);
	sqlbuf_free(&pred);
	sqlbuf_free(&grp);
	sqlbuf_free(&ord);
	sqlbuf_free(&out);
	return rc;
}

/* ------------------------------------------------------------- entry point --- */

int transform_query(const Query *q, const Catalog *cat, const NameMap *map,
                    char **sql, char **errmsg)
{
	*sql = NULL;
	if (errmsg)
		*errmsg = NULL;

	if (q->matches->next)
		return fail(errmsg, "multiple MATCH clauses are not supported yet");

	const Match *m = q->matches;
	if (check_return_shape(q->ret, errmsg))
		return 1;

	/* Scan for the constructs handled by a dedicated engine: variable-length
	 * paths (M8) and undirected hops, each supported as a single sole hop. */
	int has_undirected = 0, has_varlen = 0, nseg = 0, npaths = 0;
	for (PathList *pl = m->patterns; pl; pl = pl->next) {
		npaths++;
		for (Segment *s = pl->path->segments; s; s = s->next) {
			nseg++;
			if (s->rel->varlen.present)
				has_varlen = 1;
			if (s->rel->dir == REL_UNDIR)
				has_undirected = 1;
		}
	}

	if (has_varlen) {
		if (npaths != 1 || nseg != 1)
			return fail(errmsg, "variable-length relationships are "
				"only supported as a single hop");
		const Path *p = m->patterns->path;
		return transform_varlen(p->start, p->segments->rel,
			p->segments->node, q->ret, m->where, cat, map, sql, errmsg);
	}

	if (has_undirected) {
		if (npaths != 1 || nseg != 1)
			return fail(errmsg, "undirected relationships are only "
				"supported as a single hop");
		const Path *p = m->patterns->path;
		return transform_undirected(p->start, p->segments->rel,
			p->segments->node, q->ret, m->where, cat, map, sql, errmsg);
	}

	return transform_general(m, q->ret, cat, map, sql, errmsg);
}
