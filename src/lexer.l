/*
 * lexer.l — tokens of the Hola agent language.
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 *
 * Newlines end statements the way Go's do: a newline after an identifier,
 * literal, return/break/continue, ')' ']' '}' '++' '--' becomes ';'.
 * An identifier written right before '{' (Foo{) is one token, IDENT_LBRACE,
 * which is how a composite literal is told apart from a block (`if x {`).
 */
%option noyywrap nounput noinput yylineno

%{
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "strbuf.h"
#include "parser.h"

const char *g_cur_file = "<input>";
int g_cur_line = 1;

static int last_tok = 0;     /* last token returned, for ';' insertion */
static strbuf doc;           /* pending // doc comment lines */
static int doc_line = -1;    /* line of the last doc comment */

static int needs_semi(int t) {
    switch (t) {
    case IDENT: case INT: case FLOAT: case STRING: case TRUE: case FALSE:
    case RETURN: case BREAK: case CONTINUE: case INC: case DEC:
    case ')': case ']': case '}':
        return 1;
    }
    return 0;
}

#define TOK(t) do { last_tok = (t); g_cur_line = yylineno; return (t); } while (0)

static int keyword(const char *s) {
    static const struct { const char *w; int t; } kw[] = {
        {"package", PACKAGE}, {"import", IMPORT}, {"func", FUNC}, {"type", TYPE},
        {"struct", STRUCT}, {"agent", AGENT}, {"skill", SKILL}, {"tool", TOOL}, {"abstract", ABSTRACT},
        {"extends", EXTENDS}, {"override", OVERRIDE}, {"var", VAR}, {"if", IF},
        {"else", ELSE}, {"for", FOR}, {"range", RANGE}, {"return", RETURN},
        {"break", BREAK}, {"continue", CONTINUE}, {"true", TRUE}, {"false", FALSE},
    };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
        if (strcmp(kw[i].w, s) == 0) return kw[i].t;
    return 0;
}

/* FUNC, AGENT and SKILL carry the doc comment written right above them. */
static char *take_doc(void) {
    char *d = NULL;
    if (doc.len && doc_line >= yylineno - 1) d = strdup(doc.data);
    sb_clear(&doc);
    return d;
}

static int ident_or_keyword(const char *text) {
    int k = keyword(text);
    if (k == FUNC || k == AGENT || k == SKILL || k == TOOL) { yylval.s = take_doc(); TOK(k); }
    if (k) TOK(k);
    yylval.s = strdup(text);
    TOK(IDENT);
}

static char *unescape(const char *s, size_t n) {
    strbuf b = {0};
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            char e = s[++i];
            switch (e) {
            case 'n': sb_putc(&b, '\n'); break;
            case 't': sb_putc(&b, '\t'); break;
            case 'r': sb_putc(&b, '\r'); break;
            case '0': sb_putc(&b, '\0'); break;
            case '\\': sb_putc(&b, '\\'); break;
            case '"': sb_putc(&b, '"'); break;
            default: sb_putc(&b, '\\'); sb_putc(&b, e); break;
            }
        } else {
            sb_putc(&b, c);
        }
    }
    if (!b.data) sb_puts(&b, "");
    return b.data;
}

%}

ID      [A-Za-z_][A-Za-z0-9_]*
DIGIT   [0-9]

%%

"//"[^\n]*      {
                    const char *t = yytext + 2;
                    if (*t == ' ') t++;
                    if (doc_line != yylineno - 1) sb_clear(&doc);
                    if (doc.len) sb_putc(&doc, '\n');
                    sb_puts(&doc, t);
                    doc_line = yylineno;
                }
"/*"([^*]|\*+[^*/])*\*+"/"   { /* block comment */ }
\n              {
                    if (needs_semi(last_tok)) { last_tok = ';'; g_cur_line = yylineno - 1; return ';'; }
                }
[ \t\r]+        { }

{ID}"{"         {
                    /* Foo{ is a composite literal; struct{ / else{ are keyword + '{'. */
                    yytext[yyleng - 1] = '\0';
                    if (keyword(yytext)) { yyless(yyleng - 1); return ident_or_keyword(yytext); }
                    yylval.s = strdup(yytext);
                    TOK(IDENT_LBRACE);
                }
{ID}            { return ident_or_keyword(yytext); }

{DIGIT}+"."{DIGIT}+([eE][-+]?{DIGIT}+)?  { yylval.f = strtod(yytext, NULL); TOK(FLOAT); }
{DIGIT}+[eE][-+]?{DIGIT}+                { yylval.f = strtod(yytext, NULL); TOK(FLOAT); }
"0x"[0-9a-fA-F]+                         { yylval.i = strtoll(yytext, NULL, 16); TOK(INT); }
{DIGIT}+                                 { yylval.i = strtoll(yytext, NULL, 10); TOK(INT); }

\"([^"\\\n]|\\.)*\"   { yylval.s = unescape(yytext + 1, yyleng - 2); TOK(STRING); }
`[^`]*`               { yylval.s = strndup(yytext + 1, yyleng - 2); TOK(STRING); }

":="            { TOK(DEFINE); }
"=="            { TOK(EQ); }
"!="            { TOK(NE); }
"<="            { TOK(LE); }
">="            { TOK(GE); }
"&&"            { TOK(AND); }
"||"            { TOK(OR); }
"++"            { TOK(INC); }
"--"            { TOK(DEC); }
"+="            { TOK(PLUSEQ); }
"-="            { TOK(MINUSEQ); }
"*="            { TOK(MULEQ); }
"/="            { TOK(DIVEQ); }
[-+*/%<>=!(){}\[\],.;:]  { TOK(yytext[0]); }

.               { fprintf(stderr, "%s:%d: unexpected character '%s'\n", g_cur_file, yylineno, yytext); TOK(ERROR_TOKEN); }

<<EOF>>         {
                    if (needs_semi(last_tok)) { last_tok = ';'; return ';'; }
                    return 0;
                }

%%

void lexer_reset(void) {
    last_tok = 0;
    sb_clear(&doc);
    doc_line = -1;
    yylineno = 1;
    g_cur_line = 1;
}
