/*
 * ast.h -- abstract syntax tree for the read-only Cypher subset knit-graph
 * understands. The grammar (cypher_parser.y) builds these nodes; the
 * transformer (M3+) consumes them.
 */

#ifndef KNIT_GRAPH_AST_H
#define KNIT_GRAPH_AST_H

#include <stdio.h>

/* ---------------------------------------------------------------- lists --- */

/* A singly-linked list of strings (labels, relationship types). */
typedef struct StrList {
	char *s;
	struct StrList *next;
} StrList;

/* Forward declarations. */
typedef struct Expr Expr;

/* A property map entry: key: value. */
typedef struct PropList {
	char *key;
	Expr *value;
	struct PropList *next;
} PropList;

/* A list of expressions (function arguments, IN right-hand side). */
typedef struct ExprList {
	Expr *expr;
	struct ExprList *next;
} ExprList;

/* -------------------------------------------------------- expressions --- */

typedef enum {
	EXPR_OR,
	EXPR_AND,
	EXPR_NOT,
	EXPR_CMP,          /* left <op> right */
	EXPR_IN,           /* left IN (args...) */
	EXPR_IS_NULL,      /* left IS NULL */
	EXPR_IS_NOT_NULL,  /* left IS NOT NULL */
	EXPR_STARTS_WITH,  /* left STARTS WITH right */
	EXPR_ENDS_WITH,    /* left ENDS WITH right */
	EXPR_CONTAINS,     /* left CONTAINS right */
	EXPR_PROPERTY,     /* var.key */
	EXPR_VARIABLE,     /* var */
	EXPR_FUNC,         /* func(args) */
	EXPR_LITERAL
} ExprKind;

typedef enum { CMP_EQ, CMP_NE, CMP_LT, CMP_GT, CMP_LE, CMP_GE } CmpOp;

typedef enum { LIT_INT, LIT_REAL, LIT_STRING, LIT_BOOL, LIT_NULL } LitKind;

struct Expr {
	ExprKind kind;

	Expr *left;         /* operands for boolean/comparison/string/IN/IS */
	Expr *right;
	CmpOp cmp;          /* EXPR_CMP */

	char *var;          /* EXPR_VARIABLE / EXPR_PROPERTY */
	char *key;          /* EXPR_PROPERTY */

	char *func;         /* EXPR_FUNC name */
	int func_distinct;  /* count(DISTINCT ...) */
	int func_star;      /* count(*) */
	ExprList *args;      /* EXPR_FUNC args, or EXPR_IN list elements */

	LitKind lit_kind;   /* EXPR_LITERAL */
	char *lit_text;     /* decoded string value / numeric text */
	int lit_bool;       /* LIT_BOOL */
};

/* ------------------------------------------------------------ patterns --- */

typedef struct NodePat {
	char *var;          /* may be NULL (anonymous) */
	StrList *labels;    /* may be NULL */
	PropList *props;    /* may be NULL */
} NodePat;

typedef enum { REL_RIGHT, REL_LEFT, REL_UNDIR } RelDir;

/* Variable-length specifier: -[:t*min..max]->  */
typedef struct VarLen {
	int present;        /* a '*' was given */
	int has_min;
	int min;
	int has_max;
	int max;
} VarLen;

typedef struct RelPat {
	char *var;          /* may be NULL */
	StrList *types;     /* may be NULL */
	PropList *props;    /* may be NULL */
	RelDir dir;
	VarLen varlen;
} RelPat;

/* One hop of a path: a relationship followed by a node. */
typedef struct Segment {
	RelPat *rel;
	NodePat *node;
	struct Segment *next;
} Segment;

/* A path pattern: a starting node followed by zero or more hops. */
typedef struct Path {
	NodePat *start;
	Segment *segments;  /* may be NULL */
} Path;

typedef struct PathList {
	Path *path;
	struct PathList *next;
} PathList;

/* --------------------------------------------------------- top clauses --- */

typedef struct Match {
	PathList *patterns;
	Expr *where;        /* may be NULL */
	struct Match *next;
} Match;

typedef struct ReturnItem {
	Expr *expr;
	char *alias;        /* may be NULL */
	struct ReturnItem *next;
} ReturnItem;

typedef struct SortItem {
	Expr *expr;
	int desc;
	struct SortItem *next;
} SortItem;

typedef struct Return {
	int distinct;
	int star;           /* RETURN * */
	ReturnItem *items;  /* NULL when star */
	SortItem *order;    /* may be NULL */
	Expr *skip;         /* may be NULL */
	Expr *limit;        /* may be NULL */
} Return;

typedef struct Query {
	Match *matches;     /* one or more, in source order */
	Return *ret;
} Query;

/* ------------------------------------------------------- constructors --- */

StrList   *ast_strlist(char *s, StrList *next);
PropList  *ast_proplist(char *key, Expr *value, PropList *next);
ExprList  *ast_exprlist(Expr *e, ExprList *next);

Expr *ast_expr_bin(ExprKind kind, Expr *l, Expr *r);
Expr *ast_expr_cmp(CmpOp op, Expr *l, Expr *r);
Expr *ast_expr_not(Expr *e);
Expr *ast_expr_in(Expr *l, ExprList *list);
Expr *ast_expr_is_null(Expr *l, int negated);
Expr *ast_expr_property(char *var, char *key);
Expr *ast_expr_variable(char *var);
Expr *ast_expr_func(char *func, int distinct, int star, ExprList *args);
Expr *ast_expr_lit(LitKind kind, char *text, int boolval);

NodePat *ast_node(char *var, StrList *labels, PropList *props);
RelPat  *ast_rel(char *var, StrList *types, PropList *props, RelDir dir, VarLen vl);
Segment *ast_segment(RelPat *rel, NodePat *node, Segment *next);
Path    *ast_path(NodePat *start, Segment *segments);
PathList *ast_pathlist(Path *p, PathList *next);

Match      *ast_match(PathList *patterns, Expr *where, Match *next);
ReturnItem *ast_return_item(Expr *e, char *alias, ReturnItem *next);
SortItem   *ast_sort_item(Expr *e, int desc, SortItem *next);
Return     *ast_return(int distinct, int star, ReturnItem *items,
                       SortItem *order, Expr *skip, Expr *limit);
Query      *ast_query(Match *matches, Return *ret);

/* --------------------------------------------------------------- misc --- */

void ast_free_query(Query *q);

/* Pretty-print the AST as an indented tree (used by --ast and tests). */
void ast_dump(FILE *out, const Query *q);

/*
 * Parse a Cypher string into a Query.  Returns 0 on success and stores the
 * tree in *out.  On failure returns non-zero and, if errmsg is non-NULL,
 * stores a malloc'd error message in *errmsg (caller frees).
 */
int cypher_parse(const char *text, Query **out, char **errmsg);

#endif /* KNIT_GRAPH_AST_H */
