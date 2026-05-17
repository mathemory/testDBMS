#ifndef STORAGE_H
#define STORAGE_H

#include "dbms.h"

FieldDef* load_schema(const char *dbname, const char *tablename);
ConstraintDef* load_constraints(const char *dbname, const char *tablename);
int schema_col_count(FieldDef *schema);
int find_col_index(FieldDef *schema, const char *colname);

char*** load_table(const char *dbname, const char *tablename,
                   FieldDef *schema, int *row_count);
void save_table(const char *dbname, const char *tablename,
                FieldDef *schema, char ***rows, int row_count);
void free_table(char ***rows, int row_count, int col_count);
void free_schema(FieldDef *schema);
void free_constraints(ConstraintDef *constraints);
void free_cond(Cond *c);
int eval_cond(Cond *cond, FieldDef *schema, char **row);

#endif
