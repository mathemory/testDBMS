#include "dbms.h"
#include "storage.h"

#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *YY_BUFFER_STATE;
extern YY_BUFFER_STATE yy_scan_string(const char *str);
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern void yypush_buffer_state(YY_BUFFER_STATE new_buffer);
extern void yypop_buffer_state(void);
extern int yyparse(void);

char current_db[256] = "";
char data_dir[512] = "./data";
char current_user[256] = "ADMIN";
int auth_enabled = 1;
TxnState current_txn = {0, "", NULL};
int next_txn_id = 1;

typedef struct {
    int table_count;
    char **table_names;
    FieldDef **schemas;
    char ****tables;
    int *row_counts;
    int *col_counts;
    int *offsets;
    FieldDef *merged_schema;
    int *selected_indices;
    char **selected_headers;
    int selected_count;
    Cond *cond;
} SelectContext;

static void free_str_array(char **arr, int count);
static void print_selected_header(char **headers, int count);
static void print_selected_row(char **row, int *indices, int count);
static void free_row_values(char **row, int col_count);
static void rebuild_indexes_for_table(const char *table);

static void trim_newline(char *s) {
    if (s) {
        s[strcspn(s, "\r\n")] = '\0';
    }
}

static int is_safe_name(const char *name) {
    int i;

    if (!name || !name[0]) return 0;

    for (i = 0; name[i]; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return 0;
        }
    }

    return 1;
}

static int ensure_data_root(void) {
    struct stat st;
    char path[1024];
    FILE *fp;

    if (stat(data_dir, &st) != 0) {
        if (mkdir(data_dir, 0755) != 0) {
            perror("mkdir");
            return 0;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: %s is not a directory.\n", data_dir);
        return 0;
    }

    snprintf(path, sizeof(path), "%s/sys.dat", data_dir);
    fp = fopen(path, "a");
    if (!fp) {
        perror("fopen");
        return 0;
    }
    fclose(fp);
    return 1;
}

static int database_exists_in_sys(const char *name) {
    char path[1024];
    char line[256];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/sys.dat", data_dir);
    fp = fopen(path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        trim_newline(line);
        if (strcmp(line, name) == 0) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int table_exists_in_db(const char *dbname, const char *tablename) {
    char path[1024];
    char line[512];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s/sys.dat", data_dir, dbname);
    fp = fopen(path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        char tname[256];

        if (sscanf(line, "%255s", tname) == 1 && strcmp(tname, tablename) == 0) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int selected_db_ready(void) {
    if (current_db[0] == '\0') {
        fprintf(stderr, "Error: no database selected.\n");
        return 0;
    }
    return 1;
}

static const char *priv_type_name(PrivType priv) {
    switch (priv) {
        case PRIV_SELECT: return "SELECT";
        case PRIV_INSERT: return "INSERT";
        case PRIV_UPDATE: return "UPDATE";
        case PRIV_DELETE: return "DELETE";
        default: return "";
    }
}

static int ensure_security_meta(const char *dbname) {
    char users_path[1024];
    char grants_path[1024];
    FILE *fp;

    snprintf(users_path, sizeof(users_path), "%s/%s/users.meta", data_dir, dbname);
    fp = fopen(users_path, "a");
    if (!fp) {
        perror("fopen");
        return 0;
    }
    fclose(fp);

    snprintf(grants_path, sizeof(grants_path), "%s/%s/grants.meta", data_dir, dbname);
    fp = fopen(grants_path, "a");
    if (!fp) {
        perror("fopen");
        return 0;
    }
    fclose(fp);

    return 1;
}

static int ensure_views_meta(const char *dbname) {
    char views_path[1024];
    FILE *fp;

    snprintf(views_path, sizeof(views_path), "%s/%s/views.meta", data_dir, dbname);
    fp = fopen(views_path, "a");
    if (!fp) {
        perror("fopen");
        return 0;
    }
    fclose(fp);
    return 1;
}

static int ensure_constraints_meta(const char *dbname) {
    char path[1024];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s/constraints.meta", data_dir, dbname);
    fp = fopen(path, "a");
    if (!fp) {
        perror("fopen");
        return 0;
    }
    fclose(fp);
    return 1;
}

static int ensure_logs_dir(const char *dbname) {
    char dir_path[1024];
    char log_path[1024];
    struct stat st;
    FILE *fp;
    int path_len;

    snprintf(dir_path, sizeof(dir_path), "%s/%s/logs", data_dir, dbname);
    if (stat(dir_path, &st) != 0) {
        if (mkdir(dir_path, 0755) != 0) {
            perror("mkdir");
            return 0;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: %s is not a directory.\n", dir_path);
        return 0;
    }

    path_len = snprintf(log_path, sizeof(log_path), "%s/db.log", dir_path);
    if (path_len < 0 || (size_t)path_len >= sizeof(log_path)) {
        fprintf(stderr, "Error: log path too long.\n");
        return 0;
    }
    fp = fopen(log_path, "a");
    if (!fp) {
        perror("fopen");
        return 0;
    }
    fclose(fp);
    return 1;
}

static int ensure_indexes_dir(const char *dbname) {
    char path[1024];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s/indexes", data_dir, dbname);
    if (stat(path, &st) != 0) {
        if (mkdir(path, 0755) != 0) {
            perror("mkdir");
            return 0;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: %s is not a directory.\n", path);
        return 0;
    }
    return 1;
}

static void build_index_path(char *path, size_t size, const char *dbname, const char *table, const char *column) {
    snprintf(path, size, "%s/%s/indexes/%s__%s.idx", data_dir, dbname, table, column);
}

static void free_txn_logs(TxnLogRecord *logs) {
    while (logs) {
        TxnLogRecord *next = logs->next;
        free(logs->txn_id);
        free(logs->op_type);
        free(logs->table);
        free(logs->before_image);
        free(logs->after_image);
        free(logs);
        logs = next;
    }
}

static char *serialize_row(char **row, int col_count) {
    int i;
    size_t total = 1;
    char *buf;

    for (i = 0; i < col_count; i++) {
        total += strlen(row[i] ? row[i] : "");
        if (i < col_count - 1) total += 1;
    }

    buf = (char *)malloc(total);
    if (!buf) return NULL;
    buf[0] = '\0';

    for (i = 0; i < col_count; i++) {
        strcat(buf, row[i] ? row[i] : "");
        if (i < col_count - 1) {
            strcat(buf, "\t");
        }
    }

    return buf;
}

static char **deserialize_row(const char *image, int col_count) {
    char **row;
    char *copy;
    char *start;
    int i;

    row = (char **)malloc(col_count * sizeof(char *));
    if (!row) return NULL;
    for (i = 0; i < col_count; i++) row[i] = strdup("");

    copy = strdup(image ? image : "");
    if (!copy) {
        free_row_values(row, col_count);
        return NULL;
    }

    start = copy;
    for (i = 0; i < col_count; i++) {
        char *tab = strchr(start, '\t');
        free(row[i]);
        if (tab) {
            *tab = '\0';
            row[i] = strdup(start);
            start = tab + 1;
        } else {
            row[i] = strdup(start);
            start += strlen(start);
        }
    }

    free(copy);
    return row;
}

static int row_matches_image(char **row, int col_count, const char *image) {
    char *serialized;
    int matched;

    serialized = serialize_row(row, col_count);
    if (!serialized) return 0;
    matched = strcmp(serialized, image ? image : "") == 0;
    free(serialized);
    return matched;
}

static char ***load_table_with_txn(const char *dbname, const char *tablename,
                                   FieldDef *schema, int *row_count) {
    char ***rows;
    int count = 0;
    int col_count;
    TxnLogRecord *log;

    rows = load_table(dbname, tablename, schema, &count);
    if (!current_txn.active) {
        *row_count = count;
        return rows;
    }

    col_count = schema_col_count(schema);
    for (log = current_txn.logs; log; log = log->next) {
        int i;

        if (strcmp(log->table ? log->table : "", tablename) != 0) continue;

        if (strcmp(log->op_type ? log->op_type : "", "INSERT") == 0) {
            char **new_row = deserialize_row(log->after_image, col_count);
            if (!new_row) continue;
            rows = (char ***)realloc(rows, (count + 1) * sizeof(char **));
            rows[count++] = new_row;
        } else if (strcmp(log->op_type ? log->op_type : "", "DELETE") == 0) {
            for (i = 0; i < count; i++) {
                if (row_matches_image(rows[i], col_count, log->before_image)) {
                    free_row_values(rows[i], col_count);
                    for (; i < count - 1; i++) {
                        rows[i] = rows[i + 1];
                    }
                    count--;
                    if (count == 0) {
                        free(rows);
                        rows = NULL;
                    } else {
                        rows = (char ***)realloc(rows, count * sizeof(char **));
                    }
                    break;
                }
            }
        } else if (strcmp(log->op_type ? log->op_type : "", "UPDATE") == 0) {
            for (i = 0; i < count; i++) {
                if (row_matches_image(rows[i], col_count, log->before_image)) {
                    char **new_row = deserialize_row(log->after_image, col_count);
                    if (!new_row) break;
                    free_row_values(rows[i], col_count);
                    rows[i] = new_row;
                    break;
                }
            }
        }
    }

    *row_count = count;
    return rows;
}

static int apply_txn_logs_to_table(const char *table) {
    FieldDef *schema;
    char ***rows;
    int row_count = 0;
    int col_count;
    TxnLogRecord *log;

    schema = load_schema(current_db, table);
    if (!schema) {
        fprintf(stderr, "Error: Table %s does not exist.\n", table);
        return 0;
    }

    rows = load_table(current_db, table, schema, &row_count);
    col_count = schema_col_count(schema);

    for (log = current_txn.logs; log; log = log->next) {
        int i;

        if (strcmp(log->table ? log->table : "", table) != 0) continue;

        if (strcmp(log->op_type ? log->op_type : "", "INSERT") == 0) {
            char **new_row = deserialize_row(log->after_image, col_count);
            if (!new_row) continue;
            rows = (char ***)realloc(rows, (row_count + 1) * sizeof(char **));
            rows[row_count++] = new_row;
        } else if (strcmp(log->op_type ? log->op_type : "", "DELETE") == 0) {
            for (i = 0; i < row_count; i++) {
                if (row_matches_image(rows[i], col_count, log->before_image)) {
                    free_row_values(rows[i], col_count);
                    for (; i < row_count - 1; i++) {
                        rows[i] = rows[i + 1];
                    }
                    row_count--;
                    if (row_count == 0) {
                        free(rows);
                        rows = NULL;
                    } else {
                        rows = (char ***)realloc(rows, row_count * sizeof(char **));
                    }
                    break;
                }
            }
        } else if (strcmp(log->op_type ? log->op_type : "", "UPDATE") == 0) {
            for (i = 0; i < row_count; i++) {
                if (row_matches_image(rows[i], col_count, log->before_image)) {
                    char **new_row = deserialize_row(log->after_image, col_count);
                    if (!new_row) break;
                    free_row_values(rows[i], col_count);
                    rows[i] = new_row;
                    break;
                }
            }
        }
    }

    save_table(current_db, table, schema, rows, row_count);
    rebuild_indexes_for_table(table);
    free_table(rows, row_count, col_count);
    free_schema(schema);
    return 1;
}

static int append_txn_log_record(const char *op_type, const char *table,
                                 const char *before_image, const char *after_image) {
    char log_path[1024];
    FILE *fp;
    int path_len;
    TxnLogRecord *node;
    TxnLogRecord *tail;

    if (!current_txn.active) return 1;
    if (!ensure_logs_dir(current_db)) return 0;

    path_len = snprintf(log_path, sizeof(log_path), "%s/%s/logs/db.log", data_dir, current_db);
    if (path_len < 0 || (size_t)path_len >= sizeof(log_path)) {
        fprintf(stderr, "Error: log path too long.\n");
        return 0;
    }

    fp = fopen(log_path, "a");
    if (!fp) {
        perror("fopen");
        return 0;
    }
    fprintf(fp, "%s|%s|%s|%s|%s\n",
            current_txn.txn_id,
            op_type ? op_type : "",
            table ? table : "",
            before_image ? before_image : "",
            after_image ? after_image : "");
    fclose(fp);

    node = (TxnLogRecord *)malloc(sizeof(TxnLogRecord));
    if (!node) return 0;
    node->txn_id = strdup(current_txn.txn_id);
    node->op_type = strdup(op_type ? op_type : "");
    node->table = strdup(table ? table : "");
    node->before_image = strdup(before_image ? before_image : "");
    node->after_image = strdup(after_image ? after_image : "");
    node->next = NULL;

    if (!current_txn.logs) {
        current_txn.logs = node;
        return 1;
    }

    tail = current_txn.logs;
    while (tail->next) tail = tail->next;
    tail->next = node;
    return 1;
}

static int parse_index_header(const char *line, char *index_name, char *table, char *column) {
    return sscanf(line, "#INDEX|%255[^|]|%255[^|]|%255[^|\n]", index_name, table, column) == 3;
}

static int find_index_file_by_name(const char *dbname, const char *index_name,
                                   char *path_out, size_t path_size,
                                   char *table_out, size_t table_size,
                                   char *column_out, size_t column_size) {
    char dir_path[1024];
    DIR *dir;
    struct dirent *ent;

    snprintf(dir_path, sizeof(dir_path), "%s/%s/indexes", data_dir, dbname);
    dir = opendir(dir_path);
    if (!dir) return 0;

    while ((ent = readdir(dir)) != NULL) {
        char file_path[1024];
        char line[512];
        FILE *fp;
        char idx_name[256], table[256], column[256];
        int path_len;

        if (ent->d_name[0] == '.') continue;
        path_len = snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, ent->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(file_path)) {
            continue;
        }
        fp = fopen(file_path, "r");
        if (!fp) continue;
        if (fgets(line, sizeof(line), fp) && parse_index_header(line, idx_name, table, column) &&
            strcmp(idx_name, index_name) == 0) {
            fclose(fp);
            closedir(dir);
            if (path_out) snprintf(path_out, path_size, "%s", file_path);
            if (table_out) snprintf(table_out, table_size, "%s", table);
            if (column_out) snprintf(column_out, column_size, "%s", column);
            return 1;
        }
        fclose(fp);
    }

    closedir(dir);
    return 0;
}

static IndexDef *load_indexes_for_table(const char *dbname, const char *table) {
    char dir_path[1024];
    DIR *dir;
    struct dirent *ent;
    IndexDef *head = NULL, *tail = NULL;

    snprintf(dir_path, sizeof(dir_path), "%s/%s/indexes", data_dir, dbname);
    dir = opendir(dir_path);
    if (!dir) return NULL;

    while ((ent = readdir(dir)) != NULL) {
        char file_path[1024];
        char line[512];
        FILE *fp;
        char idx_name[256], tbl[256], col[256];
        int path_len;

        if (ent->d_name[0] == '.') continue;
        path_len = snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, ent->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(file_path)) {
            continue;
        }
        fp = fopen(file_path, "r");
        if (!fp) continue;
        if (fgets(line, sizeof(line), fp) && parse_index_header(line, idx_name, tbl, col) &&
            strcmp(tbl, table) == 0) {
            IndexDef *node = (IndexDef *)malloc(sizeof(IndexDef));
            node->table = strdup(tbl);
            node->column = strdup(col);
            node->path = strdup(file_path);
            node->next = NULL;
            if (!head) head = tail = node;
            else {
                tail->next = node;
                tail = node;
            }
        }
        fclose(fp);
    }

    closedir(dir);
    return head;
}

static void free_indexes(IndexDef *indexes) {
    while (indexes) {
        IndexDef *next = indexes->next;
        free(indexes->table);
        free(indexes->column);
        free(indexes->path);
        free(indexes);
        indexes = next;
    }
}

static int create_index_file(const char *index_name, const char *table, const char *column) {
    FieldDef *schema;
    char ***rows;
    int row_count = 0;
    int col_count;
    int col_idx;
    int i;
    char path[1024];
    FILE *fp;

    if (!ensure_indexes_dir(current_db)) return 0;
    schema = load_schema(current_db, table);
    if (!schema) {
        fprintf(stderr, "Error: Table %s does not exist.\n", table);
        return 0;
    }

    col_idx = find_col_index(schema, column);
    if (col_idx < 0) {
        fprintf(stderr, "Error: Column %s does not exist.\n", column);
        free_schema(schema);
        return 0;
    }

    build_index_path(path, sizeof(path), current_db, table, column);
    fp = fopen(path, "w");
    if (!fp) {
        perror("fopen");
        free_schema(schema);
        return 0;
    }
    fprintf(fp, "#INDEX|%s|%s|%s\n", index_name, table, column);

    rows = load_table(current_db, table, schema, &row_count);
    col_count = schema_col_count(schema);
    for (i = 0; i < row_count; i++) {
        fprintf(fp, "%s|%d\n", rows[i][col_idx] ? rows[i][col_idx] : "", i);
    }
    fclose(fp);

    free_table(rows, row_count, col_count);
    free_schema(schema);
    return 1;
}

static void append_indexes_for_row(const char *table, char **row) {
    IndexDef *indexes = load_indexes_for_table(current_db, table);
    IndexDef *idx;
    FieldDef *schema = load_schema(current_db, table);
    char ***rows;
    int row_count = 0;
    int col_count;

    if (!indexes || !schema) {
        free_indexes(indexes);
        free_schema(schema);
        return;
    }

    rows = load_table(current_db, table, schema, &row_count);
    col_count = schema_col_count(schema);

    for (idx = indexes; idx; idx = idx->next) {
        int col_idx = find_col_index(schema, idx->column);
        FILE *fp = fopen(idx->path, "a");
        if (!fp) continue;
        if (col_idx >= 0) {
            fprintf(fp, "%s|%d\n", row[col_idx] ? row[col_idx] : "", row_count - 1);
        }
        fclose(fp);
    }

    free_table(rows, row_count, col_count);
    free_indexes(indexes);
    free_schema(schema);
}

static void rebuild_indexes_for_table(const char *table) {
    IndexDef *indexes = load_indexes_for_table(current_db, table);
    IndexDef *idx;
    char index_name[256], table_name[256], column_name[256];

    for (idx = indexes; idx; idx = idx->next) {
        FILE *fp = fopen(idx->path, "r");
        char line[512];
        if (!fp) continue;
        if (fgets(line, sizeof(line), fp) &&
            parse_index_header(line, index_name, table_name, column_name)) {
            fclose(fp);
            remove(idx->path);
            create_index_file(index_name, table_name, column_name);
        } else {
            fclose(fp);
        }
    }
    free_indexes(indexes);
}

static int try_select_with_index(SelectStmt *s) {
    TableRef *t;
    ColRef *c;
    FieldDef *schema;
    char index_path[1024], idx_table[256], idx_col[256];
    char value_buf[256];
    FILE *fp;
    char line[512];
    int *matched_rows = NULL;
    int matched_count = 0;
    char ***rows;
    int row_count = 0;
    int col_count;
    int *selected_indices = NULL;
    char **selected_headers = NULL;
    int selected_count = 0;
    int i;

    if (current_txn.active) return 0;
    if (!s || !s->tables || s->tables->next != NULL || !s->cond) return 0;
    if (s->cond->op != OP_EQ) return 0;
    if (s->cond->left_table && strcmp(s->cond->left_table, s->tables->table) != 0) return 0;
    if (!(s->cond->right_kind == VAL_INT || s->cond->right_kind == VAL_STR)) return 0;

    t = s->tables;
    snprintf(idx_table, sizeof(idx_table), "%s", t->table);
    snprintf(idx_col, sizeof(idx_col), "%s", s->cond->left_col);
    build_index_path(index_path, sizeof(index_path), current_db, idx_table, idx_col);
    fp = fopen(index_path, "r");
    if (!fp) return 0;

    if (s->cond->right_kind == VAL_INT) snprintf(value_buf, sizeof(value_buf), "%d", s->cond->right_int);
    else snprintf(value_buf, sizeof(value_buf), "%s", s->cond->right_str ? s->cond->right_str : "");

    fgets(line, sizeof(line), fp);
    while (fgets(line, sizeof(line), fp)) {
        char val[256];
        int row_no;
        line[strcspn(line, "\r\n")] = '\0';
        if (sscanf(line, "%255[^|]|%d", val, &row_no) == 2 && strcmp(val, value_buf) == 0) {
            matched_rows = (int *)realloc(matched_rows, (matched_count + 1) * sizeof(int));
            matched_rows[matched_count++] = row_no;
        }
    }
    fclose(fp);

    schema = load_schema(current_db, t->table);
    if (!schema) {
        free(matched_rows);
        return 0;
    }
    rows = load_table_with_txn(current_db, t->table, schema, &row_count);
    col_count = schema_col_count(schema);

    {
        FieldDef *merged_schema = schema;
        int total_cols = schema_col_count(merged_schema);
        if (s->cols == NULL) {
            selected_indices = (int *)malloc(total_cols * sizeof(int));
            selected_headers = (char **)malloc(total_cols * sizeof(char *));
            FieldDef *cur = merged_schema;
            for (i = 0; i < total_cols; i++) {
                selected_indices[i] = i;
                selected_headers[i] = strdup(cur->name);
                cur = cur->next;
            }
            selected_count = total_cols;
        } else {
            int count = 0;
            for (c = s->cols; c; c = c->next) count++;
            selected_indices = (int *)malloc(count * sizeof(int));
            selected_headers = (char **)malloc(count * sizeof(char *));
            selected_count = count;
            i = 0;
            for (c = s->cols; c; c = c->next) {
                selected_indices[i] = find_col_index(schema, c->col);
                selected_headers[i] = strdup(c->col);
                i++;
            }
        }
    }

    print_selected_header(selected_headers, selected_count);
    for (i = 0; i < matched_count; i++) {
        int rn = matched_rows[i];
        if (rn >= 0 && rn < row_count && eval_cond(s->cond, schema, rows[rn])) {
            print_selected_row(rows[rn], selected_indices, selected_count);
        }
    }

    free(matched_rows);
    free_str_array(selected_headers, selected_count);
    free(selected_indices);
    free_table(rows, row_count, col_count);
    free_schema(schema);
    return 1;
}

static int view_exists_in_db(const char *dbname, const char *view_name) {
    char path[1024];
    char line[4096];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s/views.meta", data_dir, dbname);
    fp = fopen(path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        char name[256];
        trim_newline(line);
        if (sscanf(line, "%255[^|]", name) == 1 && strcmp(name, view_name) == 0) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static char *load_view_sql(const char *dbname, const char *view_name) {
    char path[1024];
    char line[4096];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s/views.meta", data_dir, dbname);
    fp = fopen(path, "r");
    if (!fp) return NULL;

    while (fgets(line, sizeof(line), fp)) {
        char name[256];
        char sql[3800];

        trim_newline(line);
        if (sscanf(line, "%255[^|]|%3799[^\n]", name, sql) == 2 &&
            strcmp(name, view_name) == 0) {
            fclose(fp);
            return strdup(sql);
        }
    }

    fclose(fp);
    return NULL;
}

static int user_exists_in_db(const char *dbname, const char *user) {
    char path[1024];
    char line[256];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s/users.meta", data_dir, dbname);
    fp = fopen(path, "r");
    if (!fp) return 0;

    while (fgets(line, sizeof(line), fp)) {
        char uname[256];

        trim_newline(line);
        if (sscanf(line, "%255[^|]", uname) == 1 && strcmp(uname, user) == 0) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int check_table_priv(const char *user, const char *table, PrivType priv) {
    char path[1024];
    char line[512];
    const char *priv_name;
    FILE *fp;

    if (!auth_enabled) return 1;
    if (!user || strcmp(user, "ADMIN") == 0) return 1;
    if (!current_db[0]) return 0;
    if (!ensure_security_meta(current_db)) return 0;

    priv_name = priv_type_name(priv);
    snprintf(path, sizeof(path), "%s/%s/grants.meta", data_dir, current_db);
    fp = fopen(path, "r");
    if (!fp) {
        perror("fopen");
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        char uname[256], obj[256], pbuf[256];
        trim_newline(line);
        if (sscanf(line, "%255[^|]|%255[^|]|%255[^|]", uname, obj, pbuf) == 3 &&
            strcmp(uname, user) == 0 &&
            strcmp(obj, table) == 0 &&
            strcmp(pbuf, priv_name) == 0) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    fprintf(stderr, "Error: user %s has no %s privilege on %s.\n", user, priv_name, table);
    return 0;
}

static int value_is_empty(const char *v) {
    return (!v || v[0] == '\0');
}

static int row_has_value_in_column(char ***rows, int row_count, int col_index, const char *value, int skip_row) {
    int i;

    for (i = 0; i < row_count; i++) {
        if (i == skip_row) continue;
        if (col_index >= 0 && rows && rows[i] && rows[i][col_index] &&
            strcmp(rows[i][col_index], value ? value : "") == 0) {
            return 1;
        }
    }
    return 0;
}

static int validate_row_constraints(const char *table,
                                    FieldDef *schema,
                                    ConstraintDef *constraints,
                                    char ***rows,
                                    int row_count,
                                    char **candidate_row,
                                    int skip_row) {
    ConstraintDef *c;

    for (c = constraints; c; c = c->next) {
        int col_idx = find_col_index(schema, c->column);
        const char *value;

        if (col_idx < 0) continue;
        value = candidate_row[col_idx] ? candidate_row[col_idx] : "";

        if (c->type == CONS_NOT_NULL) {
            if (value_is_empty(value)) {
                fprintf(stderr, "Error: %s.%s cannot be NULL.\n", table, c->column);
                return 0;
            }
        } else if (c->type == CONS_PRIMARY_KEY) {
            if (value_is_empty(value)) {
                fprintf(stderr, "Error: primary key %s.%s cannot be NULL.\n", table, c->column);
                return 0;
            }
            if (row_has_value_in_column(rows, row_count, col_idx, value, skip_row)) {
                fprintf(stderr, "Error: duplicate primary key on %s.%s.\n", table, c->column);
                return 0;
            }
        } else if (c->type == CONS_UNIQUE) {
            if (!value_is_empty(value) &&
                row_has_value_in_column(rows, row_count, col_idx, value, skip_row)) {
                fprintf(stderr, "Error: duplicate unique value on %s.%s.\n", table, c->column);
                return 0;
            }
        } else if (c->type == CONS_FOREIGN_KEY) {
            FieldDef *ref_schema;
            char ***ref_rows;
            int ref_row_count = 0;
            int ref_col_idx;
            int found = 0;
            int i;

            if (value_is_empty(value)) {
                continue;
            }

            ref_schema = load_schema(current_db, c->ref_table);
            if (!ref_schema) {
                fprintf(stderr, "Error: referenced table %s does not exist.\n", c->ref_table);
                return 0;
            }

            ref_col_idx = find_col_index(ref_schema, c->ref_column);
            ref_rows = load_table(current_db, c->ref_table, ref_schema, &ref_row_count);
            for (i = 0; i < ref_row_count; i++) {
                if (ref_col_idx >= 0 && ref_rows[i][ref_col_idx] &&
                    strcmp(ref_rows[i][ref_col_idx], value) == 0) {
                    found = 1;
                    break;
                }
            }
            free_table(ref_rows, ref_row_count, schema_col_count(ref_schema));
            free_schema(ref_schema);

            if (!found) {
                fprintf(stderr, "Error: foreign key %s.%s references missing value in %s.%s.\n",
                        table, c->column, c->ref_table, c->ref_column);
                return 0;
            }
        }
    }

    return 1;
}

static int validate_delete_references(const char *table,
                                      FieldDef *schema,
                                      char **row) {
    char path[1024];
    char line[512];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s/constraints.meta", data_dir, current_db);
    fp = fopen(path, "r");
    if (!fp) return 1;

    while (fgets(line, sizeof(line), fp)) {
        char src_table[256];
        char type[64];
        char src_col[256];
        char ref_table[256];
        char ref_col[256];

        line[strcspn(line, "\r\n")] = '\0';
        if (sscanf(line, "%255[^|]|%63[^|]|%255[^|]|%255[^|]|%255[^|]",
                   src_table, type, src_col, ref_table, ref_col) == 5 &&
            strcmp(type, "FK") == 0 &&
            strcmp(ref_table, table) == 0) {
            int ref_idx = find_col_index(schema, ref_col);
            FieldDef *child_schema;
            char ***child_rows;
            int child_row_count = 0;
            int child_idx;
            int i;
            const char *target_value;

            if (ref_idx < 0) continue;
            target_value = row[ref_idx] ? row[ref_idx] : "";

            child_schema = load_schema(current_db, src_table);
            if (!child_schema) continue;

            child_idx = find_col_index(child_schema, src_col);
            child_rows = load_table(current_db, src_table, child_schema, &child_row_count);
            for (i = 0; i < child_row_count; i++) {
                if (child_idx >= 0 && child_rows[i][child_idx] &&
                    strcmp(child_rows[i][child_idx], target_value) == 0) {
                    fprintf(stderr, "Error: row is referenced by foreign key %s.%s.\n",
                            src_table, src_col);
                    free_table(child_rows, child_row_count, schema_col_count(child_schema));
                    free_schema(child_schema);
                    fclose(fp);
                    return 0;
                }
            }
            free_table(child_rows, child_row_count, schema_col_count(child_schema));
            free_schema(child_schema);
        }
    }

    fclose(fp);
    return 1;
}

static int try_select_view(SelectStmt *s) {
    TableRef *t;
    char *view_sql;
    char temp_path[1024];
    char command[1400];
    FILE *fp;

    if (!s || !s->tables) {
        return 0;
    }

    t = s->tables;
    if (t->next != NULL) {
        return 0;
    }

    if (!view_exists_in_db(current_db, t->table)) {
        return 0;
    }

    if (current_txn.active) {
        fprintf(stderr, "Error: selecting from views inside a transaction is not supported.\n");
        return 1;
    }

    if (s->cols != NULL || s->cond != NULL) {
        fprintf(stderr, "Error: view queries currently support only SELECT * FROM view.\n");
        return 1;
    }

    view_sql = load_view_sql(current_db, t->table);
    if (!view_sql) {
        fprintf(stderr, "Error: view %s definition not found.\n", t->table);
        return 1;
    }

    snprintf(temp_path, sizeof(temp_path), "/tmp/dbms_view_%ld.sql", (long)getpid());
    fp = fopen(temp_path, "w");
    if (!fp) {
        perror("fopen");
        free(view_sql);
        return 1;
    }

    fprintf(fp, "USE %s;\n", current_db);
    if (auth_enabled && current_user[0] != '\0' && strcmp(current_user, "ADMIN") != 0) {
        fprintf(fp, "LOGIN %s;\n", current_user);
    }
    fprintf(fp, "%s;\n", view_sql);
    fclose(fp);

    snprintf(command, sizeof(command), "./dbms %s", temp_path);
    system(command);
    remove(temp_path);

    free(view_sql);
    return 1;
}

static void append_text(char *buf, size_t size, const char *text) {
    size_t len = strlen(buf);
    if (len < size - 1) {
        snprintf(buf + len, size - len, "%s", text);
    }
}

static void append_cond_sql(char *buf, size_t size, Cond *cond) {
    char tmp[256];

    if (!cond) return;

    if (cond->op == OP_AND || cond->op == OP_OR) {
        append_text(buf, size, "(");
        append_cond_sql(buf, size, cond->left);
        append_text(buf, size, cond->op == OP_AND ? " AND " : " OR ");
        append_cond_sql(buf, size, cond->right);
        append_text(buf, size, ")");
        return;
    }

    if (cond->op == OP_NOT) {
        append_text(buf, size, "NOT (");
        append_cond_sql(buf, size, cond->left);
        append_text(buf, size, ")");
        return;
    }

    if (cond->left_table) {
        append_text(buf, size, cond->left_table);
        append_text(buf, size, ".");
    }
    append_text(buf, size, cond->left_col ? cond->left_col : "");

    switch (cond->op) {
        case OP_EQ: append_text(buf, size, "="); break;
        case OP_NEQ: append_text(buf, size, "!="); break;
        case OP_LT: append_text(buf, size, "<"); break;
        case OP_GT: append_text(buf, size, ">"); break;
        case OP_LE: append_text(buf, size, "<="); break;
        case OP_GE: append_text(buf, size, ">="); break;
        default: break;
    }

    if (cond->right_kind == VAL_INT) {
        snprintf(tmp, sizeof(tmp), "%d", cond->right_int);
        append_text(buf, size, tmp);
    } else if (cond->right_kind == VAL_STR) {
        append_text(buf, size, "'");
        append_text(buf, size, cond->right_str ? cond->right_str : "");
        append_text(buf, size, "'");
    } else if (cond->right_kind == VAL_COL) {
        if (cond->right_table) {
            append_text(buf, size, cond->right_table);
            append_text(buf, size, ".");
        }
        append_text(buf, size, cond->right_str ? cond->right_str : "");
    }
}

static char *select_stmt_to_sql(SelectStmt *stmt) {
    char buf[8192];
    ColRef *c;
    TableRef *t;

    if (!stmt) return strdup("");

    buf[0] = '\0';
    append_text(buf, sizeof(buf), "SELECT ");

    if (stmt->cols == NULL) {
        append_text(buf, sizeof(buf), "*");
    } else {
        for (c = stmt->cols; c; c = c->next) {
            if (c != stmt->cols) append_text(buf, sizeof(buf), ",");
            if (c->table) {
                append_text(buf, sizeof(buf), c->table);
                append_text(buf, sizeof(buf), ".");
            }
            append_text(buf, sizeof(buf), c->col ? c->col : "*");
        }
    }

    append_text(buf, sizeof(buf), " FROM ");
    for (t = stmt->tables; t; t = t->next) {
        if (t != stmt->tables) append_text(buf, sizeof(buf), ",");
        append_text(buf, sizeof(buf), t->table);
    }

    if (stmt->cond) {
        append_text(buf, sizeof(buf), " WHERE ");
        append_cond_sql(buf, sizeof(buf), stmt->cond);
    }

    return strdup(buf);
}

static char *default_value_for_field(FieldDef *field) {
    if (field->type == TYPE_INT) {
        return strdup("0");
    }
    return strdup("");
}

static void free_str_array(char **arr, int count) {
    int i;

    if (!arr) return;
    for (i = 0; i < count; i++) {
        free(arr[i]);
    }
    free(arr);
}

static void free_row_values(char **row, int col_count) {
    int i;

    if (!row) return;
    for (i = 0; i < col_count; i++) {
        free(row[i]);
    }
    free(row);
}

static FieldDef *merge_schema_list(FieldDef **schemas, int table_count) {
    FieldDef *head = NULL;
    FieldDef *tail = NULL;
    int i;

    for (i = 0; i < table_count; i++) {
        FieldDef *cur = schemas[i];
        while (cur) {
            FieldDef *node = (FieldDef *)malloc(sizeof(FieldDef));
            node->name = strdup(cur->name);
            node->type = cur->type;
            node->length = cur->length;
            node->next = NULL;

            if (!head) {
                head = node;
                tail = node;
            } else {
                tail->next = node;
                tail = node;
            }

            cur = cur->next;
        }
    }

    return head;
}

static void print_selected_header(char **headers, int count) {
    int i;

    for (i = 0; i < count; i++) {
        printf("%-15s", headers[i]);
    }
    printf("\n");
}

static void print_selected_row(char **row, int *indices, int count) {
    int i;

    for (i = 0; i < count; i++) {
        printf("%-15s", row[indices[i]] ? row[indices[i]] : "");
    }
    printf("\n");
}

static int find_table_offset(const char *table_name, char **table_names, int table_count) {
    int i;

    for (i = 0; i < table_count; i++) {
        if (strcmp(table_name, table_names[i]) == 0) {
            return i;
        }
    }

    return -1;
}

static int build_selected_columns(SelectStmt *s,
                                  char **table_names,
                                  FieldDef **schemas,
                                  int *offsets,
                                  int table_count,
                                  FieldDef *merged_schema,
                                  int **indices_out,
                                  char ***headers_out,
                                  int *count_out) {
    int total_cols = schema_col_count(merged_schema);
    int *indices;
    char **headers;
    int count = 0;
    int i;

    if (s->cols == NULL) {
        FieldDef *cur = merged_schema;

        indices = (int *)malloc(total_cols * sizeof(int));
        headers = (char **)malloc(total_cols * sizeof(char *));

        for (i = 0; i < total_cols; i++) {
            indices[i] = i;
            headers[i] = strdup(cur->name);
            cur = cur->next;
        }

        *indices_out = indices;
        *headers_out = headers;
        *count_out = total_cols;
        return 1;
    }

    {
        ColRef *c = s->cols;
        while (c) {
            count++;
            c = c->next;
        }
    }

    indices = (int *)malloc(count * sizeof(int));
    headers = (char **)malloc(count * sizeof(char *));

    {
        ColRef *c = s->cols;
        i = 0;

        while (c) {
            int idx = -1;

            if (c->table) {
                int table_pos = find_table_offset(c->table, table_names, table_count);
                if (table_pos >= 0) {
                    int local_idx = find_col_index(schemas[table_pos], c->col);
                    if (local_idx >= 0) {
                        idx = offsets[table_pos] + local_idx;
                    }
                }
            } else {
                idx = find_col_index(merged_schema, c->col);
            }

            if (idx < 0) {
                fprintf(stderr, "Error: column %s not found.\n", c->col);
                free(indices);
                free_str_array(headers, i);
                return 0;
            }

            indices[i] = idx;
            headers[i] = strdup(c->col);
            i++;
            c = c->next;
        }
    }

    *indices_out = indices;
    *headers_out = headers;
    *count_out = count;
    return 1;
}

static void select_recursive(SelectContext *ctx, int level, char **merged_row) {
    int i;
    int j;

    if (level == ctx->table_count) {
        if (eval_cond(ctx->cond, ctx->merged_schema, merged_row)) {
            print_selected_row(merged_row, ctx->selected_indices, ctx->selected_count);
        }
        return;
    }

    for (i = 0; i < ctx->row_counts[level]; i++) {
        for (j = 0; j < ctx->col_counts[level]; j++) {
            merged_row[ctx->offsets[level] + j] = ctx->tables[level][i][j];
        }
        select_recursive(ctx, level + 1, merged_row);
    }
}

void exec_create_db(const char *name) {
    char sys_path[1024];
    char db_path[1024];
    char db_sys_path[1024];
    char users_path[1024];
    char grants_path[1024];
    char views_path[1024];
    char constraints_path[1024];
    char logs_path[1024];
    char db_log_path[1024];
    char indexes_path[1024];
    FILE *fp;
    int path_len;

    if (!is_safe_name(name)) {
        fprintf(stderr, "Error: invalid database name.\n");
        return;
    }

    if (!ensure_data_root()) return;

    if (database_exists_in_sys(name)) {
        fprintf(stderr, "Error: Database %s already exists.\n", name);
        return;
    }

    snprintf(db_path, sizeof(db_path), "%s/%s", data_dir, name);
    if (mkdir(db_path, 0755) != 0) {
        perror("mkdir");
        return;
    }

    snprintf(sys_path, sizeof(sys_path), "%s/sys.dat", data_dir);
    fp = fopen(sys_path, "a");
    if (!fp) {
        perror("fopen");
        return;
    }
    fprintf(fp, "%s\n", name);
    fclose(fp);

    snprintf(db_sys_path, sizeof(db_sys_path), "%s/%s/sys.dat", data_dir, name);
    fp = fopen(db_sys_path, "w");
    if (!fp) {
        perror("fopen");
        return;
    }
    fclose(fp);

    snprintf(users_path, sizeof(users_path), "%s/%s/users.meta", data_dir, name);
    fp = fopen(users_path, "w");
    if (!fp) {
        perror("fopen");
        return;
    }
    fprintf(fp, "ADMIN|ADMIN\n");
    fclose(fp);

    snprintf(grants_path, sizeof(grants_path), "%s/%s/grants.meta", data_dir, name);
    fp = fopen(grants_path, "w");
    if (!fp) {
        perror("fopen");
        return;
    }
    fclose(fp);

    snprintf(views_path, sizeof(views_path), "%s/%s/views.meta", data_dir, name);
    fp = fopen(views_path, "w");
    if (!fp) {
        perror("fopen");
        return;
    }
    fclose(fp);

    snprintf(constraints_path, sizeof(constraints_path), "%s/%s/constraints.meta", data_dir, name);
    fp = fopen(constraints_path, "w");
    if (!fp) {
        perror("fopen");
        return;
    }
    fclose(fp);

    snprintf(logs_path, sizeof(logs_path), "%s/%s/logs", data_dir, name);
    if (mkdir(logs_path, 0755) != 0) {
        perror("mkdir");
        return;
    }

    path_len = snprintf(db_log_path, sizeof(db_log_path), "%s/db.log", logs_path);
    if (path_len < 0 || (size_t)path_len >= sizeof(db_log_path)) {
        fprintf(stderr, "Error: log path too long.\n");
        return;
    }
    fp = fopen(db_log_path, "w");
    if (!fp) {
        perror("fopen");
        return;
    }
    fclose(fp);

    snprintf(indexes_path, sizeof(indexes_path), "%s/%s/indexes", data_dir, name);
    if (mkdir(indexes_path, 0755) != 0) {
        perror("mkdir");
        return;
    }

    printf("Database %s created.\n", name);
}

void exec_drop_db(const char *name) {
    char sys_path[1024];
    char tmp_path[1024];
    char db_path[1024];
    char line[256];
    char cmd[1200];
    FILE *in;
    FILE *out;

    if (!is_safe_name(name)) {
        fprintf(stderr, "Error: invalid database name.\n");
        return;
    }

    if (!database_exists_in_sys(name)) {
        fprintf(stderr, "Error: Database %s does not exist.\n", name);
        return;
    }

    snprintf(db_path, sizeof(db_path), "%s/%s", data_dir, name);
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", db_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "Error: failed to remove database directory %s.\n", db_path);
        return;
    }

    snprintf(sys_path, sizeof(sys_path), "%s/sys.dat", data_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/sys.tmp", data_dir);
    in = fopen(sys_path, "r");
    if (!in) {
        perror("fopen");
        return;
    }
    out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        perror("fopen");
        return;
    }

    while (fgets(line, sizeof(line), in)) {
        char dbname[256];
        trim_newline(line);
        if (sscanf(line, "%255s", dbname) == 1 && strcmp(dbname, name) != 0) {
            fprintf(out, "%s\n", dbname);
        }
    }

    fclose(in);
    fclose(out);

    if (remove(sys_path) != 0 || rename(tmp_path, sys_path) != 0) {
        perror("rename");
        return;
    }

    if (strcmp(current_db, name) == 0) {
        current_db[0] = '\0';
    }

    printf("Database %s dropped.\n", name);
}

void exec_show_dbs(void) {
    char path[1024];
    char line[256];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/sys.dat", data_dir);
    fp = fopen(path, "r");
    if (!fp) {
        perror("fopen");
        return;
    }

    printf("DATABASES:\n");
    while (fgets(line, sizeof(line), fp)) {
        trim_newline(line);
        if (line[0]) {
            printf(" %s\n", line);
        }
    }

    fclose(fp);
}

void exec_use_db(const char *name) {
    if (!database_exists_in_sys(name)) {
        fprintf(stderr, "Error: Database %s does not exist.\n", name);
        return;
    }

    strcpy(current_db, name);
    ensure_security_meta(current_db);
    ensure_views_meta(current_db);
    ensure_constraints_meta(current_db);
    ensure_logs_dir(current_db);
    ensure_indexes_dir(current_db);
    printf("Using database %s.\n", name);
}

void exec_begin(void) {
    char log_path[1024];
    FILE *fp;

    if (!selected_db_ready()) return;
    if (current_txn.active) {
        fprintf(stderr, "Error: transaction already active.\n");
        return;
    }
    if (!ensure_logs_dir(current_db)) return;

    snprintf(current_txn.txn_id, sizeof(current_txn.txn_id), "TXN%d", next_txn_id++);
    current_txn.active = 1;
    free_txn_logs(current_txn.logs);
    current_txn.logs = NULL;

    snprintf(log_path, sizeof(log_path), "%s/%s/logs/db.log", data_dir, current_db);
    fp = fopen(log_path, "a");
    if (!fp) {
        perror("fopen");
        current_txn.active = 0;
        current_txn.txn_id[0] = '\0';
        return;
    }
    fprintf(fp, "%s|BEGIN\n", current_txn.txn_id);
    fclose(fp);

    printf("Transaction %s started.\n", current_txn.txn_id);
}

void exec_commit(void) {
    char log_path[1024];
    FILE *fp;
    TxnLogRecord *log;

    if (!selected_db_ready()) return;
    if (!current_txn.active) {
        fprintf(stderr, "Error: no active transaction.\n");
        return;
    }
    if (!ensure_logs_dir(current_db)) return;

    snprintf(log_path, sizeof(log_path), "%s/%s/logs/db.log", data_dir, current_db);
    fp = fopen(log_path, "a");
    if (!fp) {
        perror("fopen");
        return;
    }
    fprintf(fp, "%s|COMMIT\n", current_txn.txn_id);
    fclose(fp);

    for (log = current_txn.logs; log; log = log->next) {
        TxnLogRecord *prev;
        int seen = 0;

        for (prev = current_txn.logs; prev != log; prev = prev->next) {
            if (strcmp(prev->table ? prev->table : "", log->table ? log->table : "") == 0) {
                seen = 1;
                break;
            }
        }

        if (!seen && log->table && log->table[0]) {
            if (!apply_txn_logs_to_table(log->table)) {
                return;
            }
        }
    }

    free_txn_logs(current_txn.logs);
    current_txn.logs = NULL;
    current_txn.active = 0;
    current_txn.txn_id[0] = '\0';

    printf("Transaction committed.\n");
}

void exec_rollback(void) {
    char log_path[1024];
    FILE *fp;

    if (!selected_db_ready()) return;
    if (!current_txn.active) {
        fprintf(stderr, "Error: no active transaction.\n");
        return;
    }
    if (!ensure_logs_dir(current_db)) return;

    snprintf(log_path, sizeof(log_path), "%s/%s/logs/db.log", data_dir, current_db);
    fp = fopen(log_path, "a");
    if (!fp) {
        perror("fopen");
        return;
    }
    fprintf(fp, "%s|ROLLBACK\n", current_txn.txn_id);
    fclose(fp);

    free_txn_logs(current_txn.logs);
    current_txn.logs = NULL;
    current_txn.active = 0;
    current_txn.txn_id[0] = '\0';

    printf("Transaction rolled back.\n");
}

void exec_create_index(const char *index_name, const char *table, const char *column) {
    char path[1024];
    char table_name[256], column_name[256];

    if (!selected_db_ready()) return;
    if (!is_safe_name(index_name)) {
        fprintf(stderr, "Error: invalid index name.\n");
        return;
    }
    if (!table_exists_in_db(current_db, table)) {
        fprintf(stderr, "Error: Table %s does not exist.\n", table);
        return;
    }
    if (!ensure_indexes_dir(current_db)) return;
    build_index_path(path, sizeof(path), current_db, table, column);
    if (find_index_file_by_name(current_db, index_name, NULL, 0, table_name, sizeof(table_name), column_name, sizeof(column_name))) {
        fprintf(stderr, "Error: Index %s already exists.\n", index_name);
        return;
    }
    if (access(path, F_OK) == 0) {
        fprintf(stderr, "Error: index on %s(%s) already exists.\n", table, column);
        return;
    }
    if (create_index_file(index_name, table, column)) {
        printf("Index %s created.\n", index_name);
    }
}

void exec_drop_index(const char *index_name) {
    char path[1024];

    if (!selected_db_ready()) return;
    if (!find_index_file_by_name(current_db, index_name, path, sizeof(path), NULL, 0, NULL, 0)) {
        fprintf(stderr, "Error: Index %s does not exist.\n", index_name);
        return;
    }
    if (remove(path) != 0) {
        perror("remove");
        return;
    }
    printf("Index %s dropped.\n", index_name);
}

void exec_create_user(const char *name) {
    char path[1024];
    FILE *fp;

    if (!selected_db_ready()) return;
    if (!is_safe_name(name)) {
        fprintf(stderr, "Error: invalid user name.\n");
        return;
    }
    if (!ensure_security_meta(current_db)) return;
    if (user_exists_in_db(current_db, name)) {
        fprintf(stderr, "Error: User %s already exists.\n", name);
        return;
    }

    snprintf(path, sizeof(path), "%s/%s/users.meta", data_dir, current_db);
    fp = fopen(path, "a");
    if (!fp) {
        perror("fopen");
        return;
    }
    fprintf(fp, "%s|USER\n", name);
    fclose(fp);

    printf("User %s created.\n", name);
}

void exec_drop_user(const char *name) {
    char users_path[1024];
    char grants_path[1024];
    char tmp_users[1024];
    char tmp_grants[1024];
    char line[512];
    FILE *in;
    FILE *out;

    if (!selected_db_ready()) return;
    if (!ensure_security_meta(current_db)) return;
    if (strcmp(name, "ADMIN") == 0) {
        fprintf(stderr, "Error: cannot drop ADMIN.\n");
        return;
    }
    if (!user_exists_in_db(current_db, name)) {
        fprintf(stderr, "Error: User %s does not exist.\n", name);
        return;
    }

    snprintf(users_path, sizeof(users_path), "%s/%s/users.meta", data_dir, current_db);
    snprintf(tmp_users, sizeof(tmp_users), "%s/%s/users.tmp", data_dir, current_db);
    in = fopen(users_path, "r");
    if (!in) {
        perror("fopen");
        return;
    }
    out = fopen(tmp_users, "w");
    if (!out) {
        fclose(in);
        perror("fopen");
        return;
    }
    while (fgets(line, sizeof(line), in)) {
        char uname[256];
        if (sscanf(line, "%255[^|]", uname) == 1 && strcmp(uname, name) != 0) {
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);
    if (remove(users_path) != 0 || rename(tmp_users, users_path) != 0) {
        perror("rename");
        return;
    }

    snprintf(grants_path, sizeof(grants_path), "%s/%s/grants.meta", data_dir, current_db);
    snprintf(tmp_grants, sizeof(tmp_grants), "%s/%s/grants.tmp", data_dir, current_db);
    in = fopen(grants_path, "r");
    if (!in) {
        perror("fopen");
        return;
    }
    out = fopen(tmp_grants, "w");
    if (!out) {
        fclose(in);
        perror("fopen");
        return;
    }
    while (fgets(line, sizeof(line), in)) {
        char uname[256];
        if (sscanf(line, "%255[^|]", uname) == 1 && strcmp(uname, name) != 0) {
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);
    if (remove(grants_path) != 0 || rename(tmp_grants, grants_path) != 0) {
        perror("rename");
        return;
    }

    if (strcmp(current_user, name) == 0) {
        strcpy(current_user, "ADMIN");
    }

    printf("User %s dropped.\n", name);
}

void exec_login(const char *name) {
    if (!selected_db_ready()) return;
    if (!ensure_security_meta(current_db)) return;
    if (!user_exists_in_db(current_db, name)) {
        fprintf(stderr, "Error: User %s does not exist.\n", name);
        return;
    }

    strcpy(current_user, name);
    printf("Logged in as %s.\n", name);
}

void exec_grant(PrivType priv, const char *object, const char *user) {
    char path[1024];
    char line[512];
    const char *priv_name;
    FILE *fp;

    if (!selected_db_ready()) return;
    if (!ensure_security_meta(current_db)) return;
    if (!user_exists_in_db(current_db, user)) {
        fprintf(stderr, "Error: User %s does not exist.\n", user);
        return;
    }

    priv_name = priv_type_name(priv);
    snprintf(path, sizeof(path), "%s/%s/grants.meta", data_dir, current_db);
    fp = fopen(path, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char uname[256], obj[256], pbuf[256];
            trim_newline(line);
            if (sscanf(line, "%255[^|]|%255[^|]|%255[^|]", uname, obj, pbuf) == 3 &&
                strcmp(uname, user) == 0 &&
                strcmp(obj, object) == 0 &&
                strcmp(pbuf, priv_name) == 0) {
                fclose(fp);
                fprintf(stderr, "Error: grant already exists.\n");
                return;
            }
        }
        fclose(fp);
    }

    fp = fopen(path, "a");
    if (!fp) {
        perror("fopen");
        return;
    }
    fprintf(fp, "%s|%s|%s\n", user, object, priv_name);
    fclose(fp);

    printf("Granted %s on %s to %s.\n", priv_name, object, user);
}

void exec_revoke(PrivType priv, const char *object, const char *user) {
    char path[1024];
    char tmp_path[1024];
    char line[512];
    const char *priv_name;
    int removed = 0;
    FILE *in;
    FILE *out;

    if (!selected_db_ready()) return;
    if (!ensure_security_meta(current_db)) return;

    priv_name = priv_type_name(priv);
    snprintf(path, sizeof(path), "%s/%s/grants.meta", data_dir, current_db);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s/grants.tmp", data_dir, current_db);
    in = fopen(path, "r");
    if (!in) {
        perror("fopen");
        return;
    }
    out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        perror("fopen");
        return;
    }

    while (fgets(line, sizeof(line), in)) {
        char uname[256], obj[256], pbuf[256];
        trim_newline(line);
        if (sscanf(line, "%255[^|]|%255[^|]|%255[^|]", uname, obj, pbuf) == 3 &&
            strcmp(uname, user) == 0 &&
            strcmp(obj, object) == 0 &&
            strcmp(pbuf, priv_name) == 0) {
            removed = 1;
            continue;
        }
        fprintf(out, "%s\n", line);
    }

    fclose(in);
    fclose(out);

    if (remove(path) != 0 || rename(tmp_path, path) != 0) {
        perror("rename");
        return;
    }

    if (!removed) {
        fprintf(stderr, "Error: grant does not exist.\n");
        return;
    }

    printf("Revoked %s on %s from %s.\n", priv_name, object, user);
}

void exec_create_view(const char *view_name, SelectStmt *stmt) {
    char path[1024];
    char *sql;
    FILE *fp;

    if (!selected_db_ready()) return;
    if (!is_safe_name(view_name)) {
        fprintf(stderr, "Error: invalid view name.\n");
        return;
    }
    if (!ensure_views_meta(current_db)) return;
    if (view_exists_in_db(current_db, view_name)) {
        fprintf(stderr, "Error: View %s already exists.\n", view_name);
        return;
    }

    sql = select_stmt_to_sql(stmt);
    snprintf(path, sizeof(path), "%s/%s/views.meta", data_dir, current_db);
    fp = fopen(path, "a");
    if (!fp) {
        perror("fopen");
        free(sql);
        return;
    }
    fprintf(fp, "%s|%s\n", view_name, sql);
    fclose(fp);
    free(sql);

    printf("View %s created.\n", view_name);
}

void exec_drop_view(const char *view_name) {
    char path[1024];
    char tmp_path[1024];
    char line[4096];
    int removed = 0;
    FILE *in;
    FILE *out;

    if (!selected_db_ready()) return;
    if (!ensure_views_meta(current_db)) return;
    if (!view_exists_in_db(current_db, view_name)) {
        fprintf(stderr, "Error: View %s does not exist.\n", view_name);
        return;
    }

    snprintf(path, sizeof(path), "%s/%s/views.meta", data_dir, current_db);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s/views.tmp", data_dir, current_db);
    in = fopen(path, "r");
    if (!in) {
        perror("fopen");
        return;
    }
    out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        perror("fopen");
        return;
    }

    while (fgets(line, sizeof(line), in)) {
        char name[256];
        trim_newline(line);
        if (sscanf(line, "%255[^|]", name) == 1 && strcmp(name, view_name) == 0) {
            removed = 1;
            continue;
        }
        fprintf(out, "%s\n", line);
    }

    fclose(in);
    fclose(out);

    if (remove(path) != 0 || rename(tmp_path, path) != 0) {
        perror("rename");
        return;
    }

    if (!removed) {
        fprintf(stderr, "Error: View %s does not exist.\n", view_name);
        return;
    }

    printf("View %s dropped.\n", view_name);
}

void exec_show_views(void) {
    char path[1024];
    char line[4096];
    FILE *fp;

    if (!selected_db_ready()) return;
    if (!ensure_views_meta(current_db)) return;

    snprintf(path, sizeof(path), "%s/%s/views.meta", data_dir, current_db);
    fp = fopen(path, "r");
    if (!fp) {
        perror("fopen");
        return;
    }

    printf("VIEWS:\n");
    while (fgets(line, sizeof(line), fp)) {
        char name[256];
        trim_newline(line);
        if (sscanf(line, "%255[^|]", name) == 1) {
            printf(" %s\n", name);
        }
    }

    fclose(fp);
}

void exec_create_table(CreateStmt *s) {
    char sys_path[1024];
    char data_path[1024];
    char constraints_path[1024];
    FILE *fp;
    FieldDef *f;
    ConstraintDef *c;
    int index = 1;

    if (!selected_db_ready()) return;
    if (!s || !s->table) return;

    if (table_exists_in_db(current_db, s->table)) {
        fprintf(stderr, "Error: Table %s already exists.\n", s->table);
        return;
    }

    snprintf(sys_path, sizeof(sys_path), "%s/%s/sys.dat", data_dir, current_db);
    fp = fopen(sys_path, "a");
    if (!fp) {
        perror("fopen");
        return;
    }

    for (f = s->fields; f; f = f->next) {
        fprintf(fp, "%s %d %s %s %d\n",
                s->table,
                index,
                f->name,
                (f->type == TYPE_INT) ? "INT" : "CHAR",
                f->length);
        index++;
    }
    fclose(fp);

    if (s->constraints) {
        if (!ensure_constraints_meta(current_db)) return;
        snprintf(constraints_path, sizeof(constraints_path), "%s/%s/constraints.meta", data_dir, current_db);
        fp = fopen(constraints_path, "a");
        if (!fp) {
            perror("fopen");
            return;
        }
        for (c = s->constraints; c; c = c->next) {
            if (c->type == CONS_PRIMARY_KEY) {
                fprintf(fp, "%s|PK|%s\n", s->table, c->column);
            } else if (c->type == CONS_NOT_NULL) {
                fprintf(fp, "%s|NOT_NULL|%s\n", s->table, c->column);
            } else if (c->type == CONS_UNIQUE) {
                fprintf(fp, "%s|UNIQUE|%s\n", s->table, c->column);
            } else if (c->type == CONS_FOREIGN_KEY) {
                fprintf(fp, "%s|FK|%s|%s|%s\n",
                        s->table, c->column, c->ref_table, c->ref_column);
            }
        }
        fclose(fp);
    }

    snprintf(data_path, sizeof(data_path), "%s/%s/%s.dat", data_dir, current_db, s->table);
    fp = fopen(data_path, "w");
    if (!fp) {
        perror("fopen");
        return;
    }
    fclose(fp);

    printf("Table %s created.\n", s->table);
}

void exec_drop_table(const char *name) {
    char sys_path[1024];
    char tmp_path[1024];
    char data_path[1024];
    char line[512];
    FILE *in;
    FILE *out;

    if (!selected_db_ready()) return;

    if (!table_exists_in_db(current_db, name)) {
        fprintf(stderr, "Error: Table %s does not exist.\n", name);
        return;
    }

    snprintf(data_path, sizeof(data_path), "%s/%s/%s.dat", data_dir, current_db, name);
    if (remove(data_path) != 0) {
        perror("remove");
        return;
    }

    {
        IndexDef *indexes = load_indexes_for_table(current_db, name);
        IndexDef *idx;
        for (idx = indexes; idx; idx = idx->next) {
            remove(idx->path);
        }
        free_indexes(indexes);
    }

    snprintf(sys_path, sizeof(sys_path), "%s/%s/sys.dat", data_dir, current_db);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s/sys.tmp", data_dir, current_db);
    in = fopen(sys_path, "r");
    if (!in) {
        perror("fopen");
        return;
    }
    out = fopen(tmp_path, "w");
    if (!out) {
        fclose(in);
        perror("fopen");
        return;
    }

    while (fgets(line, sizeof(line), in)) {
        char tname[256];

        if (sscanf(line, "%255s", tname) == 1 && strcmp(tname, name) != 0) {
            fputs(line, out);
        }
    }

    fclose(in);
    fclose(out);

    if (remove(sys_path) != 0 || rename(tmp_path, sys_path) != 0) {
        perror("rename");
        return;
    }

    printf("Table %s dropped.\n", name);
}

void exec_show_tables(void) {
    char path[1024];
    char line[512];
    char **names = NULL;
    int count = 0;
    int i;
    FILE *fp;

    if (!selected_db_ready()) return;

    snprintf(path, sizeof(path), "%s/%s/sys.dat", data_dir, current_db);
    fp = fopen(path, "r");
    if (!fp) {
        perror("fopen");
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        char tname[256];
        int seen = 0;

        if (sscanf(line, "%255s", tname) != 1) {
            continue;
        }

        for (i = 0; i < count; i++) {
            if (strcmp(names[i], tname) == 0) {
                seen = 1;
                break;
            }
        }

        if (!seen) {
            names = (char **)realloc(names, (count + 1) * sizeof(char *));
            names[count++] = strdup(tname);
        }
    }

    fclose(fp);

    printf("TABLES:\n");
    for (i = 0; i < count; i++) {
        printf(" %s\n", names[i]);
        free(names[i]);
    }
    free(names);
}

void exec_insert(InsertStmt *s) {
    FieldDef *schema;
    ConstraintDef *constraints;
    FieldDef *f;
    char data_path[1024];
    char *after_image;
    FILE *fp;
    char **row;
    int col_count;
    int i;

    if (!selected_db_ready()) return;
    if (!s || !s->table) return;
    if (!check_table_priv(current_user, s->table, PRIV_INSERT)) return;

    schema = load_schema(current_db, s->table);
    constraints = load_constraints(current_db, s->table);
    if (!schema) {
        fprintf(stderr, "Error: Table %s does not exist.\n", s->table);
        return;
    }

    col_count = schema_col_count(schema);
    row = (char **)malloc(col_count * sizeof(char *));

    f = schema;
    for (i = 0; i < col_count; i++) {
        row[i] = default_value_for_field(f);
        f = f->next;
    }

    if (s->cols == NULL) {
        StrList *v = s->vals;
        for (i = 0; i < col_count && v; i++, v = v->next) {
            free(row[i]);
            row[i] = strdup(v->val);
        }
    } else {
        StrList *c = s->cols;
        StrList *v = s->vals;

        while (c && v) {
            int idx = find_col_index(schema, c->val);
            if (idx < 0) {
                fprintf(stderr, "Error: Column %s does not exist.\n", c->val);
                free_row_values(row, col_count);
                free_schema(schema);
                free_constraints(constraints);
                return;
            }
            free(row[idx]);
            row[idx] = strdup(v->val);
            c = c->next;
            v = v->next;
        }
    }

    {
        char ***existing_rows;
        int existing_count = 0;
        existing_rows = load_table(current_db, s->table, schema, &existing_count);
        if (!validate_row_constraints(s->table, schema, constraints, existing_rows, existing_count, row, -1)) {
            free_table(existing_rows, existing_count, col_count);
            free_row_values(row, col_count);
            free_schema(schema);
            free_constraints(constraints);
            return;
        }
        free_table(existing_rows, existing_count, col_count);
    }

    if (current_txn.active) {
        after_image = serialize_row(row, col_count);
        if (!after_image || !append_txn_log_record("INSERT", s->table, "", after_image)) {
            free(after_image);
            free_row_values(row, col_count);
            free_schema(schema);
            free_constraints(constraints);
            return;
        }
        free(after_image);
        free_row_values(row, col_count);
        free_schema(schema);
        free_constraints(constraints);
        printf("1 row(s) inserted.\n");
        return;
    }

    snprintf(data_path, sizeof(data_path), "%s/%s/%s.dat", data_dir, current_db, s->table);
    fp = fopen(data_path, "a");
    if (!fp) {
        perror("fopen");
        free_row_values(row, col_count);
        free_schema(schema);
        free_constraints(constraints);
        return;
    }

    for (i = 0; i < col_count; i++) {
        fprintf(fp, "%s", row[i] ? row[i] : "");
        if (i < col_count - 1) {
            fprintf(fp, "\t");
        }
    }
    fprintf(fp, "\n");
    fclose(fp);

    append_indexes_for_row(s->table, row);
    free_row_values(row, col_count);
    free_schema(schema);
    free_constraints(constraints);
    printf("1 row(s) inserted.\n");
}

void exec_select(SelectStmt *s) {
    int table_count = 0;
    TableRef *t;
    int i;
    int total_cols = 0;
    SelectContext ctx;
    char **merged_row;

    if (!selected_db_ready()) return;
    if (!s || !s->tables) return;
    if (try_select_view(s)) return;
    if (try_select_with_index(s)) return;

    for (t = s->tables; t; t = t->next) {
        if (!check_table_priv(current_user, t->table, PRIV_SELECT)) return;
        table_count++;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.table_count = table_count;
    ctx.table_names = (char **)calloc(table_count, sizeof(char *));
    ctx.schemas = (FieldDef **)calloc(table_count, sizeof(FieldDef *));
    ctx.tables = (char ****)calloc(table_count, sizeof(char ***));
    ctx.row_counts = (int *)calloc(table_count, sizeof(int));
    ctx.col_counts = (int *)calloc(table_count, sizeof(int));
    ctx.offsets = (int *)calloc(table_count, sizeof(int));
    ctx.cond = s->cond;

    t = s->tables;
    for (i = 0; i < table_count; i++, t = t->next) {
        ctx.table_names[i] = strdup(t->table);
        ctx.schemas[i] = load_schema(current_db, t->table);
        if (!ctx.schemas[i]) {
            fprintf(stderr, "Error: Table %s does not exist.\n", t->table);
            goto cleanup;
        }

        ctx.col_counts[i] = schema_col_count(ctx.schemas[i]);
        ctx.offsets[i] = total_cols;
        total_cols += ctx.col_counts[i];
        ctx.tables[i] = load_table_with_txn(current_db, t->table, ctx.schemas[i], &ctx.row_counts[i]);
    }

    ctx.merged_schema = merge_schema_list(ctx.schemas, table_count);
    if (!build_selected_columns(s,
                                ctx.table_names,
                                ctx.schemas,
                                ctx.offsets,
                                table_count,
                                ctx.merged_schema,
                                &ctx.selected_indices,
                                &ctx.selected_headers,
                                &ctx.selected_count)) {
        goto cleanup;
    }

    print_selected_header(ctx.selected_headers, ctx.selected_count);

    merged_row = (char **)malloc(total_cols * sizeof(char *));
    if (table_count == 1) {
        for (i = 0; i < ctx.row_counts[0]; i++) {
            if (eval_cond(s->cond, ctx.schemas[0], ctx.tables[0][i])) {
                print_selected_row(ctx.tables[0][i], ctx.selected_indices, ctx.selected_count);
            }
        }
    } else {
        select_recursive(&ctx, 0, merged_row);
    }
    free(merged_row);

cleanup:
    for (i = 0; i < table_count; i++) {
        free(ctx.table_names ? ctx.table_names[i] : NULL);
        if (ctx.tables && ctx.schemas) {
            free_table(ctx.tables[i], ctx.row_counts[i], ctx.col_counts[i]);
        }
        if (ctx.schemas) {
            free_schema(ctx.schemas[i]);
        }
    }
    if (ctx.selected_headers) {
        free_str_array(ctx.selected_headers, ctx.selected_count);
    }
    free_schema(ctx.merged_schema);
    free(ctx.selected_indices);
    free(ctx.table_names);
    free(ctx.schemas);
    free(ctx.tables);
    free(ctx.row_counts);
    free(ctx.col_counts);
    free(ctx.offsets);
}

void exec_delete(DeleteStmt *s) {
    FieldDef *schema;
    char ***rows;
    char ***kept_rows = NULL;
    char *before_image;
    int row_count = 0;
    int kept_count = 0;
    int deleted = 0;
    int col_count;
    int i;

    if (!selected_db_ready()) return;
    if (!s || !s->table) return;
    if (!check_table_priv(current_user, s->table, PRIV_DELETE)) return;

    schema = load_schema(current_db, s->table);
    if (!schema) {
        fprintf(stderr, "Error: Table %s does not exist.\n", s->table);
        return;
    }

    col_count = schema_col_count(schema);
    rows = load_table(current_db, s->table, schema, &row_count);

    for (i = 0; i < row_count; i++) {
        if (eval_cond(s->cond, schema, rows[i])) {
            if (!validate_delete_references(s->table, schema, rows[i])) {
                free_table(rows, row_count, col_count);
                free(kept_rows);
                free_schema(schema);
                return;
            }
            if (current_txn.active) {
                before_image = serialize_row(rows[i], col_count);
                if (!before_image || !append_txn_log_record("DELETE", s->table, before_image, "")) {
                    free(before_image);
                    free_table(rows, row_count, col_count);
                    free(kept_rows);
                    free_schema(schema);
                    return;
                }
                free(before_image);
            }
            deleted++;
        } else {
            if (!current_txn.active) {
                kept_rows = (char ***)realloc(kept_rows, (kept_count + 1) * sizeof(char **));
                kept_rows[kept_count++] = rows[i];
            }
        }
    }

    if (!current_txn.active) {
        save_table(current_db, s->table, schema, kept_rows, kept_count);
        free(kept_rows);
        rebuild_indexes_for_table(s->table);
    } else {
        free(kept_rows);
    }
    free_table(rows, row_count, col_count);
    free_schema(schema);

    printf("%d row(s) deleted.\n", deleted);
}

void exec_update(UpdateStmt *s) {
    FieldDef *schema;
    ConstraintDef *constraints;
    char ***rows;
    char *before_image;
    char *after_image;
    int row_count = 0;
    int col_count;
    int updated = 0;
    int i;

    if (!selected_db_ready()) return;
    if (!s || !s->table) return;
    if (!check_table_priv(current_user, s->table, PRIV_UPDATE)) return;

    schema = load_schema(current_db, s->table);
    constraints = load_constraints(current_db, s->table);
    if (!schema) {
        fprintf(stderr, "Error: Table %s does not exist.\n", s->table);
        return;
    }

    col_count = schema_col_count(schema);
    rows = load_table(current_db, s->table, schema, &row_count);

    for (i = 0; i < row_count; i++) {
        if (eval_cond(s->cond, schema, rows[i])) {
            SetItem *item = s->sets;
            char **candidate = (char **)malloc(col_count * sizeof(char *));
            int j;

            for (j = 0; j < col_count; j++) {
                candidate[j] = strdup(rows[i][j] ? rows[i][j] : "");
            }

            while (item) {
                int idx = find_col_index(schema, item->col);
                if (idx >= 0) {
                    if (item->kind == VAL_INT) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%d", item->int_val);
                        free(candidate[idx]);
                        candidate[idx] = strdup(buf);
                    } else {
                        free(candidate[idx]);
                        candidate[idx] = strdup(item->str_val ? item->str_val : "");
                    }
                }
                item = item->next;
            }

            if (!validate_row_constraints(s->table, schema, constraints, rows, row_count, candidate, i)) {
                free_row_values(candidate, col_count);
                free_table(rows, row_count, col_count);
                free_schema(schema);
                free_constraints(constraints);
                return;
            }

            before_image = current_txn.active ? serialize_row(rows[i], col_count) : NULL;
            after_image = current_txn.active ? serialize_row(candidate, col_count) : NULL;
            if (current_txn.active &&
                (!before_image || !after_image ||
                 !append_txn_log_record("UPDATE", s->table, before_image, after_image))) {
                free(before_image);
                free(after_image);
                free_row_values(candidate, col_count);
                free_table(rows, row_count, col_count);
                free_schema(schema);
                free_constraints(constraints);
                return;
            }
            free(before_image);
            free(after_image);

            for (j = 0; j < col_count; j++) {
                free(rows[i][j]);
                rows[i][j] = candidate[j];
            }
            free(candidate);
            updated++;
        }
    }

    if (!current_txn.active) {
        save_table(current_db, s->table, schema, rows, row_count);
        rebuild_indexes_for_table(s->table);
    }
    free_table(rows, row_count, col_count);
    free_schema(schema);
    free_constraints(constraints);

    printf("%d row(s) updated.\n", updated);
}
