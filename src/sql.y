%{
#include "dbms.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int yylex(void);
void yyerror(const char *s);

void exec_create_db(const char *name);
void exec_drop_db(const char *name);
void exec_show_dbs(void);
void exec_use_db(const char *name);
void exec_create_table(CreateStmt *s);
void exec_drop_table(const char *name);
void exec_show_tables(void);
void exec_insert(InsertStmt *s);
void exec_select(SelectStmt *s);
void exec_delete(DeleteStmt *s);
void exec_update(UpdateStmt *s);
void exec_create_user(const char *name);
void exec_drop_user(const char *name);
void exec_login(const char *name);
void exec_grant(PrivType priv, const char *object, const char *user);
void exec_revoke(PrivType priv, const char *object, const char *user);
void exec_create_view(const char *view_name, SelectStmt *stmt);
void exec_drop_view(const char *view_name);
void exec_show_views(void);
void exec_create_index(const char *index_name, const char *table, const char *column);
void exec_drop_index(const char *index_name);
void exec_begin(void);
void exec_commit(void);
void exec_rollback(void);

static ConstraintDef *append_constraint(ConstraintDef *head, ConstraintDef *node) {
    ConstraintDef *p;

    if (!head) return node;
    p = head;
    while (p->next) p = p->next;
    p->next = node;
    return head;
}

static TableDefParts *make_table_parts(FieldDef *fields, ConstraintDef *constraints) {
    TableDefParts *parts = (TableDefParts *)malloc(sizeof(TableDefParts));
    parts->fields = fields;
    parts->constraints = constraints;
    return parts;
}

static void set_constraint_column_if_missing(ConstraintDef *constraints, const char *column) {
    ConstraintDef *c = constraints;
    while (c) {
        if (!c->column) {
            c->column = strdup(column);
        }
        c = c->next;
    }
}
%}

%code requires {
#include "dbms.h"
}

%union {
    int ival;
    char *sval;
    FieldDef *fdef;
    StrList *slist;
    ColRef *cref;
    TableRef *tref;
    Cond *cond;
    SetItem *sitem;
    ConstraintDef *cdef;
    TableDefParts *parts;
    CreateStmt *create_stmt;
    InsertStmt *insert_stmt;
    SelectStmt *select_stmt;
    DeleteStmt *delete_stmt;
    UpdateStmt *update_stmt;
}

%token <ival> INT_LIT
%token <sval> STR_LIT ID
%token CREATE DROP USE SHOW DATABASE DATABASES TABLE TABLES
%token USER VIEW VIEWS AS LOGIN GRANT REVOKE ON TO
%token INSERT INTO VALUES SELECT FROM WHERE DELETE UPDATE SET
%token PRIMARY KEY FOREIGN REFERENCES NULL_T UNIQUE INDEX
%token BEGIN_T COMMIT ROLLBACK
%token AND OR NOT INT_TYPE CHAR_TYPE EXIT
%token NEQ LE GE

%left OR
%left AND
%right NOT

%type <parts> table_element table_element_list column_def table_constraint
%type <cdef> column_constraint column_constraint_list column_constraint_list_opt
%type <slist> col_list val_list val
%type <cref> col_ref col_ref_list col_star
%type <tref> table_list
%type <cond> cond_expr compare
%type <sitem> set_item set_list
%type <create_stmt> create_tbl_stmt
%type <insert_stmt> insert_stmt
%type <select_stmt> select_stmt
%type <delete_stmt> delete_stmt
%type <update_stmt> update_stmt
%type <ival> comp_op privilege_type

%%

program
    : statement_list
    ;

statement_list
    : statement_list statement
    | statement
    ;

statement
    : create_db_stmt ';'
    | drop_db_stmt ';'
    | show_dbs_stmt ';'
    | use_db_stmt ';'
    | create_view_stmt ';'
    | drop_view_stmt ';'
    | show_views_stmt ';'
    | create_index_stmt ';'
    | drop_index_stmt ';'
    | create_user_stmt ';'
    | drop_user_stmt ';'
    | login_stmt ';'
    | grant_stmt ';'
    | revoke_stmt ';'
    | begin_stmt ';'
    | commit_stmt ';'
    | rollback_stmt ';'
    | create_tbl_stmt ';'   { exec_create_table($1); }
    | drop_tbl_stmt ';'
    | show_tbls_stmt ';'
    | insert_stmt ';'       { exec_insert($1); }
    | select_stmt ';'       { exec_select($1); }
    | delete_stmt ';'       { exec_delete($1); }
    | update_stmt ';'       { exec_update($1); }
    | exit_stmt ';'
    | error ';'             { yyerrok; }
    ;

create_db_stmt
    : CREATE DATABASE ID    { exec_create_db($3); free($3); }
    ;

drop_db_stmt
    : DROP DATABASE ID      { exec_drop_db($3); free($3); }
    ;

show_dbs_stmt
    : SHOW DATABASES        { exec_show_dbs(); }
    ;

use_db_stmt
    : USE ID                { exec_use_db($2); free($2); }
    | USE DATABASE ID       { exec_use_db($3); free($3); }
    ;

create_view_stmt
    : CREATE VIEW ID AS select_stmt
      {
          exec_create_view($3, $5);
          free($3);
      }
    ;

drop_view_stmt
    : DROP VIEW ID
      {
          exec_drop_view($3);
          free($3);
      }
    ;

show_views_stmt
    : SHOW VIEWS            { exec_show_views(); }
    ;

create_index_stmt
    : CREATE INDEX ID ON ID '(' ID ')'
      {
          exec_create_index($3, $5, $7);
          free($3);
          free($5);
          free($7);
      }
    ;

drop_index_stmt
    : DROP INDEX ID
      {
          exec_drop_index($3);
          free($3);
      }
    ;

create_user_stmt
    : CREATE USER ID        { exec_create_user($3); free($3); }
    ;

drop_user_stmt
    : DROP USER ID          { exec_drop_user($3); free($3); }
    ;

login_stmt
    : LOGIN ID              { exec_login($2); free($2); }
    ;

grant_stmt
    : GRANT privilege_type ON ID TO ID
      {
          exec_grant((PrivType)$2, $4, $6);
          free($4);
          free($6);
      }
    ;

revoke_stmt
    : REVOKE privilege_type ON ID FROM ID
      {
          exec_revoke((PrivType)$2, $4, $6);
          free($4);
          free($6);
      }
    ;

begin_stmt
    : BEGIN_T              { exec_begin(); }
    ;

commit_stmt
    : COMMIT               { exec_commit(); }
    ;

rollback_stmt
    : ROLLBACK             { exec_rollback(); }
    ;

privilege_type
    : SELECT                { $$ = PRIV_SELECT; }
    | INSERT                { $$ = PRIV_INSERT; }
    | UPDATE                { $$ = PRIV_UPDATE; }
    | DELETE                { $$ = PRIV_DELETE; }
    ;

create_tbl_stmt
    : CREATE TABLE ID '(' table_element_list ')'
      {
          CreateStmt *s = malloc(sizeof(CreateStmt));
          ConstraintDef *c;

          s->table = $3;
          s->fields = $5->fields;
          s->constraints = $5->constraints;

          c = s->constraints;
          while (c) {
              if (!c->table) {
                  c->table = strdup($3);
              }
              c = c->next;
          }

          $$ = s;
          free($5);
      }
    ;

table_element_list
    : table_element
      {
          $$ = $1;
      }
    | table_element_list ',' table_element
      {
          FieldDef *pf;
          $$ = $1;

          if (!$$->fields) {
              $$->fields = $3->fields;
          } else if ($3->fields) {
              pf = $$->fields;
              while (pf->next) pf = pf->next;
              pf->next = $3->fields;
          }

          $$->constraints = append_constraint($$->constraints, $3->constraints);
          free($3);
      }
    ;

table_element
    : column_def
      {
          $$ = $1;
      }
    | table_constraint
      {
          $$ = $1;
      }
    ;

column_def
    : ID INT_TYPE column_constraint_list_opt
      {
          FieldDef *f = malloc(sizeof(FieldDef));
          f->name = $1;
          f->type = TYPE_INT;
          f->length = 4;
          f->next = NULL;

          set_constraint_column_if_missing($3, $1);
          $$ = make_table_parts(f, $3);
      }
    | ID CHAR_TYPE '(' INT_LIT ')' column_constraint_list_opt
      {
          FieldDef *f = malloc(sizeof(FieldDef));
          f->name = $1;
          f->type = TYPE_CHAR;
          f->length = $4;
          f->next = NULL;

          set_constraint_column_if_missing($6, $1);
          $$ = make_table_parts(f, $6);
      }
    ;

column_constraint_list_opt
    :
      {
          $$ = NULL;
      }
    | column_constraint_list
      {
          $$ = $1;
      }
    ;

column_constraint_list
    : column_constraint
      {
          $$ = $1;
      }
    | column_constraint_list column_constraint
      {
          $$ = append_constraint($1, $2);
      }
    ;

column_constraint
    : PRIMARY KEY
      {
          ConstraintDef *c = malloc(sizeof(ConstraintDef));
          c->type = CONS_PRIMARY_KEY;
          c->table = NULL;
          c->column = NULL;
          c->ref_table = NULL;
          c->ref_column = NULL;
          c->next = NULL;
          $$ = c;
      }
    | NOT NULL_T
      {
          ConstraintDef *c = malloc(sizeof(ConstraintDef));
          c->type = CONS_NOT_NULL;
          c->table = NULL;
          c->column = NULL;
          c->ref_table = NULL;
          c->ref_column = NULL;
          c->next = NULL;
          $$ = c;
      }
    | UNIQUE
      {
          ConstraintDef *c = malloc(sizeof(ConstraintDef));
          c->type = CONS_UNIQUE;
          c->table = NULL;
          c->column = NULL;
          c->ref_table = NULL;
          c->ref_column = NULL;
          c->next = NULL;
          $$ = c;
      }
    ;

table_constraint
    : PRIMARY KEY '(' ID ')'
      {
          ConstraintDef *c = malloc(sizeof(ConstraintDef));
          c->type = CONS_PRIMARY_KEY;
          c->table = NULL;
          c->column = $4;
          c->ref_table = NULL;
          c->ref_column = NULL;
          c->next = NULL;
          $$ = make_table_parts(NULL, c);
      }
    | FOREIGN KEY '(' ID ')' REFERENCES ID '(' ID ')'
      {
          ConstraintDef *c = malloc(sizeof(ConstraintDef));
          c->type = CONS_FOREIGN_KEY;
          c->table = NULL;
          c->column = $4;
          c->ref_table = $7;
          c->ref_column = $9;
          c->next = NULL;
          $$ = make_table_parts(NULL, c);
      }
    ;

drop_tbl_stmt
    : DROP TABLE ID         { exec_drop_table($3); free($3); }
    ;

show_tbls_stmt
    : SHOW TABLES           { exec_show_tables(); }
    ;

insert_stmt
    : INSERT INTO ID '(' col_list ')' VALUES '(' val_list ')'
      {
          InsertStmt *s = malloc(sizeof(InsertStmt));
          s->table = $3;
          s->cols = $5;
          s->vals = $9;
          $$ = s;
      }
    | INSERT INTO ID VALUES '(' val_list ')'
      {
          InsertStmt *s = malloc(sizeof(InsertStmt));
          s->table = $3;
          s->cols = NULL;
          s->vals = $6;
          $$ = s;
      }
    ;

col_list
    : ID
      {
          StrList *s = malloc(sizeof(StrList));
          s->val = $1;
          s->next = NULL;
          $$ = s;
      }
    | col_list ',' ID
      {
          StrList *p = $1;
          while (p->next) p = p->next;
          p->next = malloc(sizeof(StrList));
          p->next->val = $3;
          p->next->next = NULL;
          $$ = $1;
      }
    ;

val_list
    : val
      {
          $$ = $1;
      }
    | val_list ',' val
      {
          StrList *p = $1;
          while (p->next) p = p->next;
          p->next = $3;
          $$ = $1;
      }
    ;

val
    : INT_LIT
      {
          StrList *s = malloc(sizeof(StrList));
          char buf[32];
          sprintf(buf, "%d", $1);
          s->val = strdup(buf);
          s->next = NULL;
          $$ = s;
      }
    | STR_LIT
      {
          StrList *s = malloc(sizeof(StrList));
          s->val = $1;
          s->next = NULL;
          $$ = s;
      }
    ;

select_stmt
    : SELECT col_star FROM table_list
      {
          SelectStmt *s = malloc(sizeof(SelectStmt));
          s->cols = $2;
          s->tables = $4;
          s->cond = NULL;
          $$ = s;
      }
    | SELECT col_star FROM table_list WHERE cond_expr
      {
          SelectStmt *s = malloc(sizeof(SelectStmt));
          s->cols = $2;
          s->tables = $4;
          s->cond = $6;
          $$ = s;
      }
    ;

col_star
    : '*'
      {
          $$ = NULL;
      }
    | col_ref_list
      {
          $$ = $1;
      }
    ;

col_ref_list
    : col_ref
      {
          $$ = $1;
      }
    | col_ref_list ',' col_ref
      {
          ColRef *p = $1;
          while (p->next) p = p->next;
          p->next = $3;
          $$ = $1;
      }
    ;

col_ref
    : ID
      {
          ColRef *c = malloc(sizeof(ColRef));
          c->table = NULL;
          c->col = $1;
          c->next = NULL;
          $$ = c;
      }
    | ID '.' ID
      {
          ColRef *c = malloc(sizeof(ColRef));
          c->table = $1;
          c->col = $3;
          c->next = NULL;
          $$ = c;
      }
    ;

table_list
    : ID
      {
          TableRef *t = malloc(sizeof(TableRef));
          t->table = $1;
          t->next = NULL;
          $$ = t;
      }
    | table_list ',' ID
      {
          TableRef *p = $1;
          while (p->next) p = p->next;
          p->next = malloc(sizeof(TableRef));
          p->next->table = $3;
          p->next->next = NULL;
          $$ = $1;
      }
    ;

cond_expr
    : cond_expr AND cond_expr
      {
          Cond *c = malloc(sizeof(Cond));
          c->op = OP_AND;
          c->left_table = NULL;
          c->left_col = NULL;
          c->right_kind = VAL_COL;
          c->right_str = NULL;
          c->right_int = 0;
          c->right_table = NULL;
          c->left = $1;
          c->right = $3;
          $$ = c;
      }
    | cond_expr OR cond_expr
      {
          Cond *c = malloc(sizeof(Cond));
          c->op = OP_OR;
          c->left_table = NULL;
          c->left_col = NULL;
          c->right_kind = VAL_COL;
          c->right_str = NULL;
          c->right_int = 0;
          c->right_table = NULL;
          c->left = $1;
          c->right = $3;
          $$ = c;
      }
    | NOT cond_expr
      {
          Cond *c = malloc(sizeof(Cond));
          c->op = OP_NOT;
          c->left_table = NULL;
          c->left_col = NULL;
          c->right_kind = VAL_COL;
          c->right_str = NULL;
          c->right_int = 0;
          c->right_table = NULL;
          c->left = $2;
          c->right = NULL;
          $$ = c;
      }
    | '(' cond_expr ')'
      {
          $$ = $2;
      }
    | compare
      {
          $$ = $1;
      }
    ;

compare
    : col_ref comp_op STR_LIT
      {
          Cond *c = malloc(sizeof(Cond));
          c->op = $2;
          c->left_table = $1->table;
          c->left_col = $1->col;
          c->right_kind = VAL_STR;
          c->right_str = $3;
          c->right_int = 0;
          c->right_table = NULL;
          c->left = NULL;
          c->right = NULL;
          free($1);
          $$ = c;
      }
    | col_ref comp_op INT_LIT
      {
          Cond *c = malloc(sizeof(Cond));
          c->op = $2;
          c->left_table = $1->table;
          c->left_col = $1->col;
          c->right_kind = VAL_INT;
          c->right_str = NULL;
          c->right_int = $3;
          c->right_table = NULL;
          c->left = NULL;
          c->right = NULL;
          free($1);
          $$ = c;
      }
    | col_ref comp_op col_ref
      {
          Cond *c = malloc(sizeof(Cond));
          c->op = $2;
          c->left_table = $1->table;
          c->left_col = $1->col;
          c->right_kind = VAL_COL;
          c->right_str = $3->col;
          c->right_int = 0;
          c->right_table = $3->table;
          c->left = NULL;
          c->right = NULL;
          free($1);
          free($3);
          $$ = c;
      }
    ;

comp_op
    : '='                    { $$ = OP_EQ; }
    | NEQ                    { $$ = OP_NEQ; }
    | '<'                    { $$ = OP_LT; }
    | '>'                    { $$ = OP_GT; }
    | LE                     { $$ = OP_LE; }
    | GE                     { $$ = OP_GE; }
    ;

delete_stmt
    : DELETE FROM ID
      {
          DeleteStmt *s = malloc(sizeof(DeleteStmt));
          s->table = $3;
          s->cond = NULL;
          $$ = s;
      }
    | DELETE FROM ID WHERE cond_expr
      {
          DeleteStmt *s = malloc(sizeof(DeleteStmt));
          s->table = $3;
          s->cond = $5;
          $$ = s;
      }
    ;

update_stmt
    : UPDATE ID SET set_list
      {
          UpdateStmt *s = malloc(sizeof(UpdateStmt));
          s->table = $2;
          s->sets = $4;
          s->cond = NULL;
          $$ = s;
      }
    | UPDATE ID SET set_list WHERE cond_expr
      {
          UpdateStmt *s = malloc(sizeof(UpdateStmt));
          s->table = $2;
          s->sets = $4;
          s->cond = $6;
          $$ = s;
      }
    ;

set_list
    : set_item
      {
          $$ = $1;
      }
    | set_list ',' set_item
      {
          SetItem *p = $1;
          while (p->next) p = p->next;
          p->next = $3;
          $$ = $1;
      }
    ;

set_item
    : ID '=' INT_LIT
      {
          SetItem *s = malloc(sizeof(SetItem));
          s->col = $1;
          s->kind = VAL_INT;
          s->str_val = NULL;
          s->int_val = $3;
          s->next = NULL;
          $$ = s;
      }
    | ID '=' STR_LIT
      {
          SetItem *s = malloc(sizeof(SetItem));
          s->col = $1;
          s->kind = VAL_STR;
          s->str_val = $3;
          s->int_val = 0;
          s->next = NULL;
          $$ = s;
      }
    ;

exit_stmt
    : EXIT                   { exit(0); }
    ;

%%

void yyerror(const char *s) {
    if (s && strcmp(s, "syntax error") == 0) {
        fprintf(stderr, "Syntax error near ';' (skipped).\n");
        return;
    }

    fprintf(stderr, "Syntax error: %s\n", s ? s : "unknown error");
}
