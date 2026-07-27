/*
 * ast.c -- AST constructors, destructor, and pretty-printer.
 */

#include "ast.h"

#include <stdlib.h>
#include <string.h>

/* Allocate zeroed memory or abort; the CLI has nothing sensible to do on OOM. */
static void *xcalloc(size_t n)
{
	void *p = calloc(1, n);
	if (!p) {
		fprintf(stderr, "knit-graph: out of memory\n");
		exit(1);
	}
	return p;
}

/* ------------------------------------------------------------ lists --- */

StrList *ast_strlist(char *s, StrList *next)
{
	StrList *l = xcalloc(sizeof(*l));
	l->s = s;
	l->next = next;
	return l;
}

PropList *ast_proplist(char *key, Expr *value, PropList *next)
{
	PropList *l = xcalloc(sizeof(*l));
	l->key = key;
	l->value = value;
	l->next = next;
	return l;
}

ExprList *ast_exprlist(Expr *e, ExprList *next)
{
	ExprList *l = xcalloc(sizeof(*l));
	l->expr = e;
	l->next = next;
	return l;
}

/* ------------------------------------------------------ expressions --- */

Expr *ast_expr_bin(ExprKind kind, Expr *l, Expr *r)
{
	Expr *e = xcalloc(sizeof(*e));
	e->kind = kind;
	e->left = l;
	e->right = r;
	return e;
}

Expr *ast_expr_cmp(CmpOp op, Expr *l, Expr *r)
{
	Expr *e = ast_expr_bin(EXPR_CMP, l, r);
	e->cmp = op;
	return e;
}

Expr *ast_expr_not(Expr *e)
{
	return ast_expr_bin(EXPR_NOT, e, NULL);
}

Expr *ast_expr_in(Expr *l, ExprList *list)
{
	Expr *e = ast_expr_bin(EXPR_IN, l, NULL);
	e->args = list;
	return e;
}

Expr *ast_expr_is_null(Expr *l, int negated)
{
	return ast_expr_bin(negated ? EXPR_IS_NOT_NULL : EXPR_IS_NULL, l, NULL);
}

Expr *ast_expr_property(char *var, char *key)
{
	Expr *e = xcalloc(sizeof(*e));
	e->kind = EXPR_PROPERTY;
	e->var = var;
	e->key = key;
	return e;
}

Expr *ast_expr_variable(char *var)
{
	Expr *e = xcalloc(sizeof(*e));
	e->kind = EXPR_VARIABLE;
	e->var = var;
	return e;
}

Expr *ast_expr_func(char *func, int distinct, int star, ExprList *args)
{
	Expr *e = xcalloc(sizeof(*e));
	e->kind = EXPR_FUNC;
	e->func = func;
	e->func_distinct = distinct;
	e->func_star = star;
	e->args = args;
	return e;
}

Expr *ast_expr_lit(LitKind kind, char *text, int boolval)
{
	Expr *e = xcalloc(sizeof(*e));
	e->kind = EXPR_LITERAL;
	e->lit_kind = kind;
	e->lit_text = text;
	e->lit_bool = boolval;
	return e;
}

/* --------------------------------------------------------- patterns --- */

NodePat *ast_node(char *var, StrList *labels, PropList *props)
{
	NodePat *n = xcalloc(sizeof(*n));
	n->var = var;
	n->labels = labels;
	n->props = props;
	return n;
}

RelPat *ast_rel(char *var, StrList *types, PropList *props, RelDir dir, VarLen vl)
{
	RelPat *r = xcalloc(sizeof(*r));
	r->var = var;
	r->types = types;
	r->props = props;
	r->dir = dir;
	r->varlen = vl;
	return r;
}

Segment *ast_segment(RelPat *rel, NodePat *node, Segment *next)
{
	Segment *s = xcalloc(sizeof(*s));
	s->rel = rel;
	s->node = node;
	s->next = next;
	return s;
}

Path *ast_path(NodePat *start, Segment *segments)
{
	Path *p = xcalloc(sizeof(*p));
	p->start = start;
	p->segments = segments;
	return p;
}

PathList *ast_pathlist(Path *p, PathList *next)
{
	PathList *l = xcalloc(sizeof(*l));
	l->path = p;
	l->next = next;
	return l;
}

/* ------------------------------------------------------ top clauses --- */

Match *ast_match(PathList *patterns, Expr *where, Match *next)
{
	Match *m = xcalloc(sizeof(*m));
	m->patterns = patterns;
	m->where = where;
	m->next = next;
	return m;
}

ReturnItem *ast_return_item(Expr *e, char *alias, ReturnItem *next)
{
	ReturnItem *r = xcalloc(sizeof(*r));
	r->expr = e;
	r->alias = alias;
	r->next = next;
	return r;
}

SortItem *ast_sort_item(Expr *e, int desc, SortItem *next)
{
	SortItem *s = xcalloc(sizeof(*s));
	s->expr = e;
	s->desc = desc;
	s->next = next;
	return s;
}

Return *ast_return(int distinct, int star, ReturnItem *items,
                   SortItem *order, Expr *skip, Expr *limit)
{
	Return *r = xcalloc(sizeof(*r));
	r->distinct = distinct;
	r->star = star;
	r->items = items;
	r->order = order;
	r->skip = skip;
	r->limit = limit;
	return r;
}

Query *ast_query(Match *matches, Return *ret)
{
	Query *q = xcalloc(sizeof(*q));
	q->matches = matches;
	q->ret = ret;
	return q;
}

/* --------------------------------------------------------------- free --- */

static void free_strlist(StrList *l)
{
	while (l) {
		StrList *n = l->next;
		free(l->s);
		free(l);
		l = n;
	}
}

static void free_expr(Expr *e);

static void free_exprlist(ExprList *l)
{
	while (l) {
		ExprList *n = l->next;
		free_expr(l->expr);
		free(l);
		l = n;
	}
}

static void free_proplist(PropList *l)
{
	while (l) {
		PropList *n = l->next;
		free(l->key);
		free_expr(l->value);
		free(l);
		l = n;
	}
}

static void free_expr(Expr *e)
{
	if (!e)
		return;
	free_expr(e->left);
	free_expr(e->right);
	free_exprlist(e->args);
	free(e->var);
	free(e->key);
	free(e->func);
	free(e->lit_text);
	free(e);
}

static void free_node(NodePat *n)
{
	if (!n)
		return;
	free(n->var);
	free_strlist(n->labels);
	free_proplist(n->props);
	free(n);
}

static void free_rel(RelPat *r)
{
	if (!r)
		return;
	free(r->var);
	free_strlist(r->types);
	free_proplist(r->props);
	free(r);
}

static void free_path(Path *p)
{
	if (!p)
		return;
	free_node(p->start);
	Segment *s = p->segments;
	while (s) {
		Segment *n = s->next;
		free_rel(s->rel);
		free_node(s->node);
		free(s);
		s = n;
	}
	free(p);
}

void ast_free_query(Query *q)
{
	if (!q)
		return;

	Match *m = q->matches;
	while (m) {
		Match *mn = m->next;
		PathList *pl = m->patterns;
		while (pl) {
			PathList *pn = pl->next;
			free_path(pl->path);
			free(pl);
			pl = pn;
		}
		free_expr(m->where);
		free(m);
		m = mn;
	}

	if (q->ret) {
		ReturnItem *ri = q->ret->items;
		while (ri) {
			ReturnItem *rn = ri->next;
			free_expr(ri->expr);
			free(ri->alias);
			free(ri);
			ri = rn;
		}
		SortItem *si = q->ret->order;
		while (si) {
			SortItem *sn = si->next;
			free_expr(si->expr);
			free(si);
			si = sn;
		}
		free_expr(q->ret->skip);
		free_expr(q->ret->limit);
		free(q->ret);
	}

	free(q);
}

/* --------------------------------------------------------------- dump --- */

static void indent(FILE *out, int depth)
{
	for (int i = 0; i < depth; i++)
		fputs("  ", out);
}

static const char *cmp_name(CmpOp op)
{
	switch (op) {
	case CMP_EQ: return "=";
	case CMP_NE: return "<>";
	case CMP_LT: return "<";
	case CMP_GT: return ">";
	case CMP_LE: return "<=";
	case CMP_GE: return ">=";
	}
	return "?";
}

static void dump_expr(FILE *out, const Expr *e, int depth);

static void dump_exprlist(FILE *out, const ExprList *l, int depth)
{
	for (; l; l = l->next)
		dump_expr(out, l->expr, depth);
}

static void dump_expr(FILE *out, const Expr *e, int depth)
{
	if (!e) {
		indent(out, depth);
		fputs("(null)\n", out);
		return;
	}
	indent(out, depth);
	switch (e->kind) {
	case EXPR_OR:
		fputs("OR\n", out);
		dump_expr(out, e->left, depth + 1);
		dump_expr(out, e->right, depth + 1);
		break;
	case EXPR_AND:
		fputs("AND\n", out);
		dump_expr(out, e->left, depth + 1);
		dump_expr(out, e->right, depth + 1);
		break;
	case EXPR_NOT:
		fputs("NOT\n", out);
		dump_expr(out, e->left, depth + 1);
		break;
	case EXPR_CMP:
		fprintf(out, "CMP %s\n", cmp_name(e->cmp));
		dump_expr(out, e->left, depth + 1);
		dump_expr(out, e->right, depth + 1);
		break;
	case EXPR_IN:
		fputs("IN\n", out);
		dump_expr(out, e->left, depth + 1);
		indent(out, depth + 1);
		fputs("list\n", out);
		dump_exprlist(out, e->args, depth + 2);
		break;
	case EXPR_IS_NULL:
		fputs("IS NULL\n", out);
		dump_expr(out, e->left, depth + 1);
		break;
	case EXPR_IS_NOT_NULL:
		fputs("IS NOT NULL\n", out);
		dump_expr(out, e->left, depth + 1);
		break;
	case EXPR_STARTS_WITH:
		fputs("STARTS WITH\n", out);
		dump_expr(out, e->left, depth + 1);
		dump_expr(out, e->right, depth + 1);
		break;
	case EXPR_ENDS_WITH:
		fputs("ENDS WITH\n", out);
		dump_expr(out, e->left, depth + 1);
		dump_expr(out, e->right, depth + 1);
		break;
	case EXPR_CONTAINS:
		fputs("CONTAINS\n", out);
		dump_expr(out, e->left, depth + 1);
		dump_expr(out, e->right, depth + 1);
		break;
	case EXPR_PROPERTY:
		fprintf(out, "property %s.%s\n", e->var, e->key);
		break;
	case EXPR_VARIABLE:
		fprintf(out, "variable %s\n", e->var);
		break;
	case EXPR_FUNC:
		fprintf(out, "func %s%s%s\n", e->func,
			e->func_distinct ? " DISTINCT" : "",
			e->func_star ? " *" : "");
		dump_exprlist(out, e->args, depth + 1);
		break;
	case EXPR_LITERAL:
		switch (e->lit_kind) {
		case LIT_INT:    fprintf(out, "int %s\n", e->lit_text); break;
		case LIT_REAL:   fprintf(out, "real %s\n", e->lit_text); break;
		case LIT_STRING: fprintf(out, "string '%s'\n", e->lit_text); break;
		case LIT_BOOL:   fprintf(out, "bool %s\n", e->lit_bool ? "true" : "false"); break;
		case LIT_NULL:   fputs("null\n", out); break;
		}
		break;
	}
}

static void dump_props(FILE *out, const PropList *p, int depth)
{
	for (; p; p = p->next) {
		indent(out, depth);
		fprintf(out, "prop %s\n", p->key);
		dump_expr(out, p->value, depth + 1);
	}
}

static void dump_strlist(FILE *out, const char *tag, const StrList *l, int depth)
{
	for (; l; l = l->next) {
		indent(out, depth);
		fprintf(out, "%s %s\n", tag, l->s);
	}
}

static void dump_node(FILE *out, const NodePat *n, int depth)
{
	indent(out, depth);
	fprintf(out, "node %s\n", n->var ? n->var : "(anon)");
	dump_strlist(out, "label", n->labels, depth + 1);
	dump_props(out, n->props, depth + 1);
}

static void dump_rel(FILE *out, const RelPat *r, int depth)
{
	const char *dir = r->dir == REL_RIGHT ? "->" :
	                  r->dir == REL_LEFT ? "<-" : "--";
	indent(out, depth);
	fprintf(out, "rel %s dir %s", r->var ? r->var : "(anon)", dir);
	if (r->varlen.present) {
		fputs(" varlen *", out);
		if (r->varlen.has_min)
			fprintf(out, "%d", r->varlen.min);
		fputs("..", out);
		if (r->varlen.has_max)
			fprintf(out, "%d", r->varlen.max);
	}
	fputc('\n', out);
	dump_strlist(out, "type", r->types, depth + 1);
	dump_props(out, r->props, depth + 1);
}

static void dump_path(FILE *out, const Path *p, int depth)
{
	indent(out, depth);
	fputs("path\n", out);
	dump_node(out, p->start, depth + 1);
	for (Segment *s = p->segments; s; s = s->next) {
		dump_rel(out, s->rel, depth + 1);
		dump_node(out, s->node, depth + 1);
	}
}

void ast_dump(FILE *out, const Query *q)
{
	fputs("query\n", out);
	for (Match *m = q->matches; m; m = m->next) {
		indent(out, 1);
		fputs("match\n", out);
		for (PathList *pl = m->patterns; pl; pl = pl->next)
			dump_path(out, pl->path, 2);
		if (m->where) {
			indent(out, 2);
			fputs("where\n", out);
			dump_expr(out, m->where, 3);
		}
	}

	Return *r = q->ret;
	indent(out, 1);
	fprintf(out, "return%s%s\n", r->distinct ? " DISTINCT" : "",
		r->star ? " *" : "");
	for (ReturnItem *ri = r->items; ri; ri = ri->next) {
		indent(out, 2);
		fprintf(out, "item%s%s\n", ri->alias ? " AS " : "",
			ri->alias ? ri->alias : "");
		dump_expr(out, ri->expr, 3);
	}
	if (r->order) {
		indent(out, 2);
		fputs("order\n", out);
		for (SortItem *si = r->order; si; si = si->next) {
			indent(out, 3);
			fprintf(out, "sort %s\n", si->desc ? "DESC" : "ASC");
			dump_expr(out, si->expr, 4);
		}
	}
	if (r->skip) {
		indent(out, 2);
		fputs("skip\n", out);
		dump_expr(out, r->skip, 3);
	}
	if (r->limit) {
		indent(out, 2);
		fputs("limit\n", out);
		dump_expr(out, r->limit, 3);
	}
}
