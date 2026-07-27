## Builds the knit-graph CLI.

bin_PROGRAMS = knit-graph

knit_graph_SOURCES = \
	main.c \
	ast.c ast.h \
	catalog.c catalog.h \
	sqlbuf.c sqlbuf.h \
	transform.c transform.h \
	cypher_parser.y \
	cypher_scanner.l

# -d makes bison emit cypher_parser.h (tokens + YYSTYPE) for the scanner.
# -Wno-yacc silences POSIX-Yacc warnings (automake invokes "bison -y").
AM_YFLAGS = -d -Wno-yacc

# The generated header must exist before the scanner is compiled.
BUILT_SOURCES = cypher_parser.h

AM_CFLAGS = $(COVERAGE_CFLAGS)
knit_graph_LDFLAGS = $(COVERAGE_LDFLAGS)

CLEANFILES = cypher_parser.c cypher_parser.h cypher_scanner.c
