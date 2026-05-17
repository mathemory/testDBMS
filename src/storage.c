#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FieldDef* load_schema(const char *dbname, const char *tablename) {
    FILE *fp;
    FieldDef *head = NULL;
    FieldDef *tail = NULL;
    char path[1024];
    char line[512];

    if (!dbname || !tablename) return NULL;

    snprintf(path, sizeof(path), "%s/%s/sys.dat", data_dir, dbname);
    fp = fopen(path, "r");
    if (!fp) return NULL;

    while (fgets(line, sizeof(line), fp)) {
        char tname[256];
        char colname[256];
        char type[16];
        int colindex;
        int length;

        if (sscanf(line, "%255s %d %255s %15s %d",
                   tname, &colindex, colname, type, &length) != 5) {
            continue;
        }

        if (strcmp(tname, tablename) == 0) {
            FieldDef *node = (FieldDef *)malloc(sizeof(FieldDef));
            node->name = strdup(colname);
            node->type = (strcmp(type, "INT") == 0) ? TYPE_INT : TYPE_CHAR;
            node->length = length;
            node->next = NULL;

            if (!head) {
                head = node;
                tail = node;
            } else {
                tail->next = node;
                tail = node;
            }
        }
    }

    fclose(fp);
    return head;
}

ConstraintDef* load_constraints(const char *dbname, const char *tablename) {
    FILE *fp;
    ConstraintDef *head = NULL;
    ConstraintDef *tail = NULL;
    char path[1024];
    char line[512];

    if (!dbname || !tablename) return NULL;

    snprintf(path, sizeof(path), "%s/%s/constraints.meta", data_dir, dbname);
    fp = fopen(path, "r");
    if (!fp) return NULL;

    while (fgets(line, sizeof(line), fp)) {
        char table[256];
        char type[64];
        char column[256] = "";
        char ref_table[256] = "";
        char ref_column[256] = "";
        int n;

        line[strcspn(line, "\r\n")] = '\0';
        n = sscanf(line, "%255[^|]|%63[^|]|%255[^|]|%255[^|]|%255[^|]",
                   table, type, column, ref_table, ref_column);
        if (n < 3 || strcmp(table, tablename) != 0) {
            continue;
        }

        {
            ConstraintDef *node = (ConstraintDef *)malloc(sizeof(ConstraintDef));
            node->table = strdup(table);
            node->column = strdup(column);
            node->ref_table = NULL;
            node->ref_column = NULL;
            node->next = NULL;

            if (strcmp(type, "PK") == 0) {
                node->type = CONS_PRIMARY_KEY;
            } else if (strcmp(type, "NOT_NULL") == 0) {
                node->type = CONS_NOT_NULL;
            } else if (strcmp(type, "UNIQUE") == 0) {
                node->type = CONS_UNIQUE;
            } else if (strcmp(type, "FK") == 0) {
                node->type = CONS_FOREIGN_KEY;
                if (n >= 5) {
                    node->ref_table = strdup(ref_table);
                    node->ref_column = strdup(ref_column);
                }
            } else {
                free(node->table);
                free(node->column);
                free(node);
                continue;
            }

            if (!head) {
                head = node;
                tail = node;
            } else {
                tail->next = node;
                tail = node;
            }
        }
    }

    fclose(fp);
    return head;
}

int schema_col_count(FieldDef *schema) {
    int count = 0;

    while (schema) {
        count++;
        schema = schema->next;
    }

    return count;
}

int find_col_index(FieldDef *schema, const char *colname) {
    int index = 0;

    while (schema) {
        if (strcmp(schema->name, colname) == 0) {
            return index;
        }
        index++;
        schema = schema->next;
    }

    return -1;
}

char*** load_table(const char *dbname, const char *tablename,
                   FieldDef *schema, int *row_count) {
    FILE *fp;
    char path[1024];
    char line[4096];
    char ***rows = NULL;
    int col_count;

    if (row_count) *row_count = 0;
    if (!dbname || !tablename || !schema || !row_count) return NULL;

    snprintf(path, sizeof(path), "%s/%s/%s.dat", data_dir, dbname, tablename);
    fp = fopen(path, "r");
    if (!fp) return NULL;

    col_count = schema_col_count(schema);

    while (fgets(line, sizeof(line), fp)) {
        char **row = (char **)malloc(col_count * sizeof(char *));
        char *start = line;
        int i;

        line[strcspn(line, "\n")] = '\0';

        for (i = 0; i < col_count; i++) {
            char *tab = strchr(start, '\t');

            if (tab) {
                *tab = '\0';
                row[i] = strdup(start);
                start = tab + 1;
            } else {
                row[i] = strdup(start);
                start += strlen(start);
            }
        }

        rows = (char ***)realloc(rows, (*row_count + 1) * sizeof(char **));
        rows[*row_count] = row;
        (*row_count)++;
    }

    fclose(fp);
    return rows;
}

void save_table(const char *dbname, const char *tablename,
                FieldDef *schema, char ***rows, int row_count) {
    FILE *fp;
    char path[1024];
    int col_count;
    int i;
    int j;

    if (!dbname || !tablename) return;

    snprintf(path, sizeof(path), "%s/%s/%s.dat", data_dir, dbname, tablename);
    fp = fopen(path, "w");
    if (!fp) return;

    col_count = schema_col_count(schema);

    for (i = 0; i < row_count; i++) {
        for (j = 0; j < col_count; j++) {
            fprintf(fp, "%s", rows[i][j] ? rows[i][j] : "");
            if (j < col_count - 1) {
                fprintf(fp, "\t");
            }
        }
        fprintf(fp, "\n");
    }

    fclose(fp);
}

void free_table(char ***rows, int row_count, int col_count) {
    int i;
    int j;

    if (!rows) return;

    for (i = 0; i < row_count; i++) {
        if (!rows[i]) continue;
        for (j = 0; j < col_count; j++) {
            free(rows[i][j]);
        }
        free(rows[i]);
    }

    free(rows);
}

void free_schema(FieldDef *schema) {
    while (schema) {
        FieldDef *next = schema->next;
        free(schema->name);
        free(schema);
        schema = next;
    }
}

void free_constraints(ConstraintDef *constraints) {
    while (constraints) {
        ConstraintDef *next = constraints->next;
        free(constraints->table);
        free(constraints->column);
        free(constraints->ref_table);
        free(constraints->ref_column);
        free(constraints);
        constraints = next;
    }
}

void free_cond(Cond *c) {
    if (!c) return;

    free_cond(c->left);
    free_cond(c->right);
    free(c->left_table);
    free(c->left_col);
    free(c->right_str);
    free(c->right_table);
    free(c);
}

int eval_cond(Cond *cond, FieldDef *schema, char **row) {
    int left_index;
    char *left_val;

    if (!cond) return 1;

    if (cond->op == OP_AND) {
        return eval_cond(cond->left, schema, row) &&
               eval_cond(cond->right, schema, row);
    }
    if (cond->op == OP_OR) {
        return eval_cond(cond->left, schema, row) ||
               eval_cond(cond->right, schema, row);
    }
    if (cond->op == OP_NOT) {
        return !eval_cond(cond->left, schema, row);
    }

    left_index = find_col_index(schema, cond->left_col);
    if (left_index < 0) return 0;

    left_val = row[left_index] ? row[left_index] : "";

    if (cond->right_kind == VAL_INT) {
        int left_int = atoi(left_val);
        int right_int = cond->right_int;

        switch (cond->op) {
            case OP_EQ:  return left_int == right_int;
            case OP_NEQ: return left_int != right_int;
            case OP_LT:  return left_int < right_int;
            case OP_GT:  return left_int > right_int;
            case OP_LE:  return left_int <= right_int;
            case OP_GE:  return left_int >= right_int;
            default:     return 0;
        }
    }

    if (cond->right_kind == VAL_STR) {
        const char *right_str = cond->right_str ? cond->right_str : "";
        int cmp = strcmp(left_val, right_str);

        switch (cond->op) {
            case OP_EQ:  return cmp == 0;
            case OP_NEQ: return cmp != 0;
            case OP_LT:  return cmp < 0;
            case OP_GT:  return cmp > 0;
            case OP_LE:  return cmp <= 0;
            case OP_GE:  return cmp >= 0;
            default:     return 0;
        }
    }

    if (cond->right_kind == VAL_COL) {
        int right_index = find_col_index(schema, cond->right_str);
        char *right_val;
        int cmp;

        if (right_index < 0) return 0;

        right_val = row[right_index] ? row[right_index] : "";
        cmp = strcmp(left_val, right_val);

        switch (cond->op) {
            case OP_EQ:  return cmp == 0;
            case OP_NEQ: return cmp != 0;
            case OP_LT:  return cmp < 0;
            case OP_GT:  return cmp > 0;
            case OP_LE:  return cmp <= 0;
            case OP_GE:  return cmp >= 0;
            default:     return 0;
        }
    }

    return 0;
}
