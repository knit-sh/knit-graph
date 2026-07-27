/*
 * cypher_scanner.l -- reentrant Flex scanner for the read-only Cypher subset.
 *
 * Emits tokens for the Bison grammar in cypher_parser.y.  Relationship arrows
 * ("->" and "<-") are tokenized here so the grammar stays unambiguous.
 */

%{
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "cypher_parser.h"

/* Decode a quoted string literal (text includes the surrounding quotes). */
static char *decode_string(const char *text, int len)
{
	char *out = malloc((size_t)len);   /* result is never longer than input-2+1 */
	char *w = out;
	for (int i = 1; i < len - 1; i++) {  /* skip surrounding quotes */
		char c = text[i];
		if (c == '\\' && i + 1 < len - 1) {
			char n = text[++i];
			switch (n) {
			case 'n': *w++ = '\n'; break;
			case 't': *w++ = '\t'; break;
			case 'r': *w++ = '\r'; break;
			default:  *w++ = n;    break;   /* \\ \' \" and anything else */
			}
		} else {
			*w++ = c;
		}
	}
	*w = '\0';
	return out;
}

/* Strip the surrounding backticks from a `quoted identifier`. */
static char *strip_backticks(const char *text, int len)
{
	char *out = malloc((size_t)len - 1);
	memcpy(out, text + 1, (size_t)len - 2);
	out[len - 2] = '\0';
	return out;
}
%}

%option reentrant bison-bridge
%option noyywrap nounput noinput
%option caseless

DIGIT   [0-9]
ALPHA   [A-Za-z_]
IDCHAR  [A-Za-z0-9_]

%%

[ \t\r\n]+          { /* whitespace */ }
"//"[^\n]*          { /* line comment */ }

"->"                { return ARROW_R; }
"<-"                { return ARROW_L; }
"<="                { return LE; }
">="                { return GE; }
"<>"                { return NE; }
".."                { return DOTDOT; }

"("                 { return '('; }
")"                 { return ')'; }
"["                 { return '['; }
"]"                 { return ']'; }
"{"                 { return '{'; }
"}"                 { return '}'; }
","                 { return ','; }
":"                 { return ':'; }
"."                 { return '.'; }
"="                 { return '='; }
"<"                 { return '<'; }
">"                 { return '>'; }
"-"                 { return '-'; }
"*"                 { return '*'; }
"|"                 { return '|'; }
";"                 { return ';'; }

MATCH               { return MATCH; }
WHERE               { return WHERE; }
RETURN              { return RETURN; }
AS                  { return AS; }
ORDER               { return ORDER; }
BY                  { return BY; }
SKIP                { return SKIP; }
LIMIT               { return LIMIT; }
DISTINCT            { return DISTINCT; }
AND                 { return AND; }
OR                  { return OR; }
NOT                 { return NOT; }
IN                  { return IN; }
IS                  { return IS; }
NULL                { return NULLTOK; }
TRUE                { return TRUETOK; }
FALSE               { return FALSETOK; }
STARTS              { return STARTS; }
ENDS                { return ENDS; }
WITH                { return WITH; }
CONTAINS            { return CONTAINS; }
ASC                 { return ASC; }
ASCENDING           { return ASC; }
DESC                { return DESC; }
DESCENDING          { return DESC; }

{DIGIT}+"."{DIGIT}+([eE][+-]?{DIGIT}+)?  { yylval->str = strdup(yytext); return REAL; }
{DIGIT}+                                 { yylval->str = strdup(yytext); return INTEGER; }

{ALPHA}{IDCHAR}*    { yylval->str = strdup(yytext); return IDENT; }

`[^`]*`             { yylval->str = strip_backticks(yytext, yyleng); return BQIDENT; }

'([^'\\]|\\.)*'     { yylval->str = decode_string(yytext, yyleng); return STRING; }
\"([^"\\]|\\.)*\"   { yylval->str = decode_string(yytext, yyleng); return STRING; }

.                   { return yytext[0]; }   /* unexpected char -> parser error */

%%
