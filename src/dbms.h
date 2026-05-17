#ifndef DBMS_H
#define DBMS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ===== 基础类型 ===== */
typedef enum { TYPE_INT, TYPE_CHAR } ColType;

/* ===== 列定义链表节点 ===== */
typedef struct FieldDef {
    char *name;
    ColType type;
    int length;
    struct FieldDef *next;
} FieldDef;

/* ===== 约束 ===== */
typedef enum {
    CONS_PRIMARY_KEY,
    CONS_FOREIGN_KEY,
    CONS_NOT_NULL,
    CONS_UNIQUE
} ConstraintType;

typedef struct ConstraintDef {
    ConstraintType type;
    char *table;
    char *column;
    char *ref_table;
    char *ref_column;
    struct ConstraintDef *next;
} ConstraintDef;

typedef struct TableDefParts {
    FieldDef *fields;
    ConstraintDef *constraints;
} TableDefParts;

/* ===== CREATE TABLE 语法树 ===== */
typedef struct {
    char *table;
    FieldDef *fields;
    ConstraintDef *constraints;
} CreateStmt;

/* ===== 字符串链表 ===== */
typedef struct StrList {
    char *val;
    struct StrList *next;
} StrList;

/* ===== INSERT 语法树 ===== */
typedef struct {
    char *table;
    StrList *cols;
    StrList *vals;
} InsertStmt;

/* ===== 条件操作类型 ===== */
typedef enum {
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR, OP_NOT
} OpType;

/* ===== 右值类型 ===== */
typedef enum { VAL_COL, VAL_STR, VAL_INT } ValKind;

/* ===== 条件树节点 ===== */
typedef struct Cond {
    OpType op;
    char *left_table;
    char *left_col;
    ValKind right_kind;
    char *right_str;
    int right_int;
    char *right_table;
    struct Cond *left;
    struct Cond *right;
} Cond;

/* ===== 列引用链表 ===== */
typedef struct ColRef {
    char *table;
    char *col;
    struct ColRef *next;
} ColRef;

/* ===== 表引用链表 ===== */
typedef struct TableRef {
    char *table;
    struct TableRef *next;
} TableRef;

/* ===== SELECT 语法树 ===== */
typedef struct {
    ColRef *cols;
    TableRef *tables;
    Cond *cond;
} SelectStmt;

/* ===== DELETE 语法树 ===== */
typedef struct {
    char *table;
    Cond *cond;
} DeleteStmt;

/* ===== UPDATE SET 链表 ===== */
typedef struct SetItem {
    char *col;
    ValKind kind;
    char *str_val;
    int int_val;
    struct SetItem *next;
} SetItem;

/* ===== UPDATE 语法树 ===== */
typedef struct {
    char *table;
    SetItem *sets;
    Cond *cond;
} UpdateStmt;

/* ===== 视图定义 ===== */
typedef struct ViewDef {
    char *name;
    char *select_sql;
    struct ViewDef *next;
} ViewDef;

/* ===== 索引定义 ===== */
typedef struct IndexDef {
    char *table;
    char *column;
    char *path;
    struct IndexDef *next;
} IndexDef;

typedef struct TxnLogRecord {
    char *txn_id;
    char *op_type;
    char *table;
    char *before_image;
    char *after_image;
    struct TxnLogRecord *next;
} TxnLogRecord;

typedef struct TxnState {
    int active;
    char txn_id[64];
    TxnLogRecord *logs;
} TxnState;

/* ===== 用户/授权 ===== */
typedef enum {
    PRIV_SELECT,
    PRIV_INSERT,
    PRIV_UPDATE,
    PRIV_DELETE
} PrivType;

typedef struct GrantItem {
    char *user;
    char *object;
    PrivType priv;
    struct GrantItem *next;
} GrantItem;

/* ===== 外部变量 ===== */
extern char current_db[256];
extern char data_dir[512];
extern char current_user[256];
extern int auth_enabled;
extern TxnState current_txn;
extern int next_txn_id;

#endif
