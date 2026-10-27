/*
 * parser.y — grammar of the Hola agent language (Go-like, with agents).
 * Copyright (C) 2026 Siraj Razick <siraj@cloudgpu.io>
 * GPL-3.0-or-later.
 */
%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "strbuf.h"

int yylex(void);
void yyerror(const char *msg);
extern FILE *yyin;
typedef struct yy_buffer_state *YY_BUFFER_STATE;
YY_BUFFER_STATE yy_scan_string(const char *s);
void yy_delete_buffer(YY_BUFFER_STATE b);
void yyrestart(FILE *f);
void lexer_reset(void);

static nodelist *g_out;
static int g_errors;

#define N(k) node_new((k), g_cur_line)

static node *mk_tname(char *name) { node *n = N(N_TNAME); n->name = name; return n; }
static node *mk_bin(int op, node *a, node *b) { node *n = N(N_BINARY); n->ival = op; n->a = a; n->b = b; return n; }
static node *mk_list(nkind k) { return N(k); }
%}

%union {
    char *s;
    int64_t i;
    double f;
    struct node *n;
}

%token <s> IDENT IDENT_LBRACE STRING FUNC AGENT SKILL TOOL
%token <i> INT
%token <f> FLOAT
%token PACKAGE IMPORT TYPE STRUCT ABSTRACT EXTENDS OVERRIDE VAR IF ELSE FOR RANGE
%token RETURN BREAK CONTINUE TRUE FALSE
%token DEFINE EQ NE LE GE AND OR INC DEC PLUSEQ MINUSEQ MULEQ DIVEQ ERROR_TOKEN

%type <n> toplevel funcdecl params paramlist param type type_lb ret_lb block stmtlist stmt
%type <n> simple_stmt opt_simple opt_expr ifstmt forstmt expr unary primary args arglist
%type <n> kvlist kvs elems elemlist fieldseq field agentdecl aitems aitem aval strlist parents tooldecl
%type <s> opt_str name_lb
%type <i> opt_abstract

%left OR
%left AND
%left EQ NE '<' LE '>' GE
%left '+' '-'
%left '*' '/' '%'

%start program

%%

program
    : semis toplevels
    ;

semis
    : %empty
    | semis ';'
    ;

toplevels
    : %empty
    | toplevels toplevel semis   { if ($2) nodelist_push(g_out, $2); }
    ;

toplevel
    : PACKAGE IDENT ';'          { free($2); $$ = NULL; }
    | IMPORT STRING ';'          { $$ = N(N_IMPORT); $$->text = $2; }
    | funcdecl ';'               { $$ = $1; }
    | TYPE IDENT STRUCT '{' fieldseq '}' ';'
                                 { $$ = $5; $$->kind = N_STRUCT; $$->name = $2; }
    | agentdecl ';'              { $$ = $1; }
    | tooldecl ';'               { $$ = $1; }
    | SKILL name_lb aitems '}' ';'
                                 { $$ = $3; $$->kind = N_AGENT; $$->name = $2; $$->flag = 2; $$->text = $1; }
    | error ';'                  { $$ = NULL; yyerrok; }
    ;

/* ---- functions ------------------------------------------------- */

funcdecl
    : FUNC IDENT '(' params ')' ret_lb stmtlist '}'
        { $$ = N(N_FUNC); $$->text = $1; $$->name = $2; $$->list = $4->list;
          $$->type = $6; $$->a = $7; $$->a->kind = N_BLOCK; }
    | FUNC IDENT '(' params ')' ret_lb stmtlist stmt '}'
        { $$ = N(N_FUNC); $$->text = $1; $$->name = $2; $$->list = $4->list;
          $$->type = $6; nodelist_push(&$7->list, $8); $$->a = $7; $$->a->kind = N_BLOCK; }
    ;

/* The return type and the body's '{', which may be glued: `) int{`. */
ret_lb
    : '{'                        { $$ = NULL; }
    | type_lb                    { $$ = $1; }
    ;

params
    : %empty                     { $$ = mk_list(N_BLOCK); }
    | paramlist                  { $$ = $1; }
    ;

paramlist
    : param                      { $$ = mk_list(N_BLOCK); nodelist_push(&$$->list, $1); }
    | paramlist ',' param        { $$ = $1; nodelist_push(&$$->list, $3); }
    ;

param
    : IDENT type                 { $$ = N(N_PARAM); $$->name = $1; $$->type = $2; }
    ;

type
    : IDENT                      { $$ = mk_tname($1); }
    | IDENT '.' IDENT            { $$ = mk_tname(xasprintf("%s.%s", $1, $3)); free($1); free($3); }
    | '[' ']' type               { $$ = N(N_TARRAY); $$->type = $3; }
    ;

/* A type immediately followed by '{'. */
type_lb
    : IDENT '{'                  { $$ = mk_tname($1); }
    | IDENT_LBRACE               { $$ = mk_tname($1); }
    | IDENT '.' IDENT '{'        { $$ = mk_tname(xasprintf("%s.%s", $1, $3)); free($1); free($3); }
    | IDENT '.' IDENT_LBRACE     { $$ = mk_tname(xasprintf("%s.%s", $1, $3)); free($1); free($3); }
    | '[' ']' type_lb            { $$ = N(N_TARRAY); $$->type = $3; }
    ;

/* ---- statements ------------------------------------------------- */

block
    : '{' stmtlist '}'           { $$ = $2; $$->kind = N_BLOCK; }
    | '{' stmtlist stmt '}'      { $$ = $2; $$->kind = N_BLOCK; nodelist_push(&$$->list, $3); }
    ;

stmtlist
    : %empty                     { $$ = mk_list(N_BLOCK); }
    | stmtlist ';'               { $$ = $1; }
    | stmtlist stmt ';'          { $$ = $1; nodelist_push(&$$->list, $2); }
    | stmtlist error ';'         { $$ = $1; yyerrok; }
    ;

stmt
    : simple_stmt                { $$ = $1; }
    | VAR IDENT type             { $$ = N(N_VAR); $$->name = $2; $$->type = $3; }
    | VAR IDENT type '=' expr    { $$ = N(N_VAR); $$->name = $2; $$->type = $3; $$->a = $5; }
    | VAR IDENT '=' expr         { $$ = N(N_VAR); $$->name = $2; $$->a = $4; }
    | ifstmt                     { $$ = $1; }
    | forstmt                    { $$ = $1; }
    | RETURN                     { $$ = N(N_RETURN); }
    | RETURN expr                { $$ = N(N_RETURN); $$->a = $2; }
    | BREAK                      { $$ = N(N_BREAK); }
    | CONTINUE                   { $$ = N(N_CONTINUE); }
    | block                      { $$ = $1; }
    ;

simple_stmt
    : expr                       { $$ = N(N_EXPRSTMT); $$->a = $1; }
    | IDENT DEFINE expr          { $$ = N(N_DEFINE); $$->name = $1; $$->a = $3; }
    | expr '=' expr              { $$ = N(N_ASSIGN); $$->ival = '='; $$->a = $1; $$->b = $3; }
    | expr PLUSEQ expr           { $$ = N(N_ASSIGN); $$->ival = '+'; $$->a = $1; $$->b = $3; }
    | expr MINUSEQ expr          { $$ = N(N_ASSIGN); $$->ival = '-'; $$->a = $1; $$->b = $3; }
    | expr MULEQ expr            { $$ = N(N_ASSIGN); $$->ival = '*'; $$->a = $1; $$->b = $3; }
    | expr DIVEQ expr            { $$ = N(N_ASSIGN); $$->ival = '/'; $$->a = $1; $$->b = $3; }
    | expr INC                   { $$ = N(N_INCDEC); $$->ival = 1; $$->a = $1; }
    | expr DEC                   { $$ = N(N_INCDEC); $$->ival = -1; $$->a = $1; }
    ;

ifstmt
    : IF expr block              { $$ = N(N_IF); $$->a = $2; $$->b = $3; }
    | IF expr block ELSE block   { $$ = N(N_IF); $$->a = $2; $$->b = $3; $$->c = $5; }
    | IF expr block ELSE ifstmt  { $$ = N(N_IF); $$->a = $2; $$->b = $3; $$->c = $5; }
    ;

forstmt
    : FOR block                  { $$ = N(N_FOR); $$->d = $2; }
    | FOR expr block             { $$ = N(N_FOR); $$->b = $2; $$->d = $3; }
    | FOR opt_simple ';' opt_expr ';' opt_simple block
                                 { $$ = N(N_FOR); $$->a = $2; $$->b = $4; $$->c = $6; $$->d = $7; }
    | FOR IDENT DEFINE RANGE expr block
                                 { $$ = N(N_RANGE); $$->name = $2; $$->a = $5; $$->d = $6; }
    | FOR IDENT ',' IDENT DEFINE RANGE expr block
                                 { $$ = N(N_RANGE); $$->name = $2; $$->text = $4; $$->a = $7; $$->d = $8; }
    ;

opt_simple
    : %empty                     { $$ = NULL; }
    | simple_stmt                { $$ = $1; }
    ;

opt_expr
    : %empty                     { $$ = NULL; }
    | expr                       { $$ = $1; }
    ;

/* ---- expressions ------------------------------------------------ */

expr
    : unary                      { $$ = $1; }
    | expr OR expr               { $$ = mk_bin(OP_OR, $1, $3); }
    | expr AND expr              { $$ = mk_bin(OP_AND, $1, $3); }
    | expr EQ expr               { $$ = mk_bin(OP_EQ, $1, $3); }
    | expr NE expr               { $$ = mk_bin(OP_NE, $1, $3); }
    | expr '<' expr              { $$ = mk_bin('<', $1, $3); }
    | expr LE expr               { $$ = mk_bin(OP_LE, $1, $3); }
    | expr '>' expr              { $$ = mk_bin('>', $1, $3); }
    | expr GE expr               { $$ = mk_bin(OP_GE, $1, $3); }
    | expr '+' expr              { $$ = mk_bin('+', $1, $3); }
    | expr '-' expr              { $$ = mk_bin('-', $1, $3); }
    | expr '*' expr              { $$ = mk_bin('*', $1, $3); }
    | expr '/' expr              { $$ = mk_bin('/', $1, $3); }
    | expr '%' expr              { $$ = mk_bin('%', $1, $3); }
    ;

unary
    : primary                    { $$ = $1; }
    | '-' unary                  { $$ = N(N_UNARY); $$->ival = '-'; $$->a = $2; }
    | '!' unary                  { $$ = N(N_UNARY); $$->ival = '!'; $$->a = $2; }
    ;

primary
    : INT                        { $$ = N(N_INT); $$->ival = $1; }
    | FLOAT                      { $$ = N(N_FLOAT); $$->fval = $1; }
    | STRING                     { $$ = N(N_STR); $$->text = $1; }
    | TRUE                       { $$ = N(N_BOOL); $$->ival = 1; }
    | FALSE                      { $$ = N(N_BOOL); $$->ival = 0; }
    | IDENT                      { $$ = N(N_IDENT); $$->name = $1; }
    | '(' expr ')'               { $$ = $2; }
    | primary '(' args ')'       { $$ = N(N_CALL); $$->a = $1; $$->list = $3->list; }
    | primary '.' IDENT          { $$ = N(N_FIELD); $$->a = $1; $$->name = $3; }
    | primary '[' expr ']'       { $$ = N(N_INDEX); $$->a = $1; $$->b = $3; }
    | IDENT_LBRACE kvlist '}'    { $$ = N(N_COMPOSITE); $$->type = mk_tname($1); $$->list = $2->list; }
    | primary '.' IDENT_LBRACE kvlist '}'
                                 { if ($1->kind != N_IDENT) yyerror("a composite literal needs a type name before '{'");
                                   $$ = N(N_COMPOSITE); $$->type = mk_tname(xasprintf("%s.%s", $1->name, $3));
                                   free($3); $$->list = $4->list; }
    | '[' ']' type_lb elems '}'  { $$ = N(N_ARRAYLIT); $$->type = $3; $$->list = $4->list; }
    ;

args
    : %empty                     { $$ = mk_list(N_BLOCK); }
    | arglist                    { $$ = $1; }
    | arglist ','                { $$ = $1; }
    ;

arglist
    : expr                       { $$ = mk_list(N_BLOCK); nodelist_push(&$$->list, $1); }
    | arglist ',' expr           { $$ = $1; nodelist_push(&$$->list, $3); }
    ;

kvlist
    : %empty                     { $$ = mk_list(N_BLOCK); }
    | kvs                        { $$ = $1; }
    | kvs ','                    { $$ = $1; }
    | kvs ';'                    { $$ = $1; }
    | kvs ',' ';'                { $$ = $1; }
    ;

kvs
    : IDENT ':' expr             { $$ = mk_list(N_BLOCK); node *kv = N(N_KV); kv->name = $1; kv->a = $3;
                                   nodelist_push(&$$->list, kv); }
    | kvs ',' IDENT ':' expr     { $$ = $1; node *kv = N(N_KV); kv->name = $3; kv->a = $5;
                                   nodelist_push(&$$->list, kv); }
    | kvs ',' ';' IDENT ':' expr { $$ = $1; node *kv = N(N_KV); kv->name = $4; kv->a = $6;
                                   nodelist_push(&$$->list, kv); }
    ;

elems
    : %empty                     { $$ = mk_list(N_BLOCK); }
    | elemlist                   { $$ = $1; }
    | elemlist ','               { $$ = $1; }
    | elemlist ';'               { $$ = $1; }
    | elemlist ',' ';'           { $$ = $1; }
    ;

elemlist
    : expr                       { $$ = mk_list(N_BLOCK); nodelist_push(&$$->list, $1); }
    | elemlist ',' expr          { $$ = $1; nodelist_push(&$$->list, $3); }
    | elemlist ',' ';' expr      { $$ = $1; nodelist_push(&$$->list, $4); }
    ;

/* ---- struct fields and agent input/output blocks ---------------- */

fieldseq
    : %empty                     { $$ = mk_list(N_FIELDS); }
    | fieldseq ';'               { $$ = $1; }
    | fieldseq field             { $$ = $1; nodelist_push(&$$->list, $2); }
    ;

field
    : IDENT type opt_str         { $$ = N(N_PARAM); $$->name = $1; $$->type = $2; $$->text = $3; }
    ;

opt_str
    : %empty                     { $$ = NULL; }
    | STRING                     { $$ = $1; }
    ;

/* ---- agents ----------------------------------------------------- */

agentdecl
    : opt_abstract AGENT name_lb aitems '}'
        { $$ = $4; $$->kind = N_AGENT; $$->name = $3; $$->flag = (int)$1; $$->text = $2; }
    | opt_abstract AGENT IDENT EXTENDS parents name_lb aitems '}'
        { $$ = $7; $$->kind = N_AGENT; $$->name = $3; $$->flag = (int)$1; $$->text = $2;
          $$->list2 = $5->list; nodelist_push(&$$->list2, mk_tname($6)); }
    ;

/* A declarative tool: what it does is bound to an action, not programmed.
 *   tool name(params) = sh "template" | http "url" | print "text" | read "path" | write "path" "text"
 *   tool name = agent Other          (the other agent's input is the tool's parameters) */
tooldecl
    : TOOL IDENT '(' params ')' '=' IDENT STRING
        { $$ = N(N_TOOLBIND); $$->text = $1; $$->name = $2; $$->list = $4->list;
          $$->a = N(N_IDENT); $$->a->name = $7; $$->b = N(N_STR); $$->b->text = $8; }
    | TOOL IDENT '(' params ')' '=' IDENT STRING STRING
        { $$ = N(N_TOOLBIND); $$->text = $1; $$->name = $2; $$->list = $4->list;
          $$->a = N(N_IDENT); $$->a->name = $7; $$->b = N(N_STR); $$->b->text = $8;
          $$->c = N(N_STR); $$->c->text = $9; }
    | TOOL IDENT '=' AGENT IDENT
        { $$ = N(N_TOOLBIND); $$->text = $1; $$->name = $2; free($4);
          $$->a = N(N_IDENT); $$->a->name = xstrdup("agent"); $$->d = N(N_IDENT); $$->d->name = $5; }
    ;

opt_abstract
    : %empty                     { $$ = 0; }
    | ABSTRACT                   { $$ = 1; }
    ;

/* "Name {" or "Name{" */
name_lb
    : IDENT '{'                  { $$ = $1; }
    | IDENT_LBRACE               { $$ = $1; }
    ;

parents
    : %empty                     { $$ = mk_list(N_BLOCK); }
    | parents IDENT ','          { $$ = $1; nodelist_push(&$$->list, mk_tname($2)); }
    ;

aitems
    : %empty                     { $$ = mk_list(N_AGENT); }
    | aitems ';'                 { $$ = $1; }
    | aitems aitem ';'           { $$ = $1; nodelist_push(&$$->list, $2); }
    | aitems error ';'           { $$ = $1; yyerrok; }
    ;

aitem
    : IDENT aval                 { $$ = N(N_AITEM); $$->name = $1; $$->a = $2; }
    | name_lb fieldseq '}'       { $$ = N(N_AITEM); $$->name = $1; $$->a = $2; }
    | funcdecl                   { $$ = N(N_AITEM); $$->name = xstrdup("func"); $$->a = $1; $$->a->kind = N_TOOLDEF; }
    | tooldecl                   { $$ = N(N_AITEM); $$->name = xstrdup("tool"); $$->a = $1; }
    | OVERRIDE aitem             { $$ = $2; $$->flag = 1; }
    ;

aval
    : STRING                     { $$ = N(N_STR); $$->text = $1; }
    | INT                        { $$ = N(N_INT); $$->ival = $1; }
    | FLOAT                      { $$ = N(N_FLOAT); $$->fval = $1; }
    | TRUE                       { $$ = N(N_BOOL); $$->ival = 1; }
    | FALSE                      { $$ = N(N_BOOL); $$->ival = 0; }
    | IDENT                      { $$ = N(N_IDENT); $$->name = $1; }
    | IDENT '(' STRING ')'       { $$ = N(N_CALL); $$->name = $1; $$->text = $3; }
    | '[' strlist ']'            { $$ = $2; }
    | '[' strlist ',' ']'        { $$ = $2; }
    ;

strlist
    : %empty                     { $$ = mk_list(N_STRLIST); }
    | STRING                     { $$ = mk_list(N_STRLIST); node *s = N(N_STR); s->text = $1; nodelist_push(&$$->list, s); }
    | strlist ',' STRING         { $$ = $1; node *s = N(N_STR); s->text = $3; nodelist_push(&$$->list, s); }
    | IDENT                      { $$ = mk_list(N_STRLIST); node *s = N(N_IDENT); s->name = $1; nodelist_push(&$$->list, s); }
    | strlist ',' IDENT          { $$ = $1; node *s = N(N_IDENT); s->name = $3; nodelist_push(&$$->list, s); }
    ;

%%

void yyerror(const char *msg) {
    fprintf(stderr, "%s:%d: %s\n", g_cur_file, g_cur_line, msg);
    g_errors++;
}

int parse_file(const char *path, nodelist *out) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "holacc: cannot open %s\n", path); return -1; }
    g_cur_file = xstrdup(path);
    g_out = out;
    g_errors = 0;
    lexer_reset();
    yyin = f;
    yyrestart(f);
    int rc = yyparse();
    fclose(f);
    return (rc || g_errors) ? -1 : 0;
}

int parse_string(const char *name, const char *src, nodelist *out) {
    g_cur_file = name;
    g_out = out;
    g_errors = 0;
    lexer_reset();
    YY_BUFFER_STATE b = yy_scan_string(src);
    int rc = yyparse();
    yy_delete_buffer(b);
    return (rc || g_errors) ? -1 : 0;
}
