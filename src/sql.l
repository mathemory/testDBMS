%{
#include "sql.tab.h"
#include "dbms.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

char *to_upper(const char *s) {
    char *r = strdup(s);
    if (!r) return NULL;
    for (int i = 0; r[i]; i++) {
        r[i] = toupper((unsigned char)r[i]);
    }
    return r;
}
%}

%option noyywrap noinput nounput

digit       [0-9]
letter      [A-Za-z]
id          {letter}({letter}|{digit}|_)*
integer     {digit}+
string      '[^']*'
whitespace  [ \t\r\n]+
comment     "//"[^\n]*
sqlcomment  "--"[^\n]*

%%
{whitespace}    ;
{comment}       ;
{sqlcomment}    ;

{id} {
    char *up = to_upper(yytext);

    if (strcmp(up, "CREATE") == 0) { free(up); return CREATE; }
    if (strcmp(up, "DATABASE") == 0) { free(up); return DATABASE; }
    if (strcmp(up, "USE") == 0) { free(up); return USE; }
    if (strcmp(up, "DROP") == 0) { free(up); return DROP; }
    if (strcmp(up, "SHOW") == 0) { free(up); return SHOW; }
    if (strcmp(up, "DATABASES") == 0) { free(up); return DATABASES; }
    if (strcmp(up, "TABLE") == 0) { free(up); return TABLE; }
    if (strcmp(up, "TABLES") == 0) { free(up); return TABLES; }
    if (strcmp(up, "USER") == 0) { free(up); return USER; }
    if (strcmp(up, "VIEW") == 0) { free(up); return VIEW; }
    if (strcmp(up, "VIEWS") == 0) { free(up); return VIEWS; }
    if (strcmp(up, "AS") == 0) { free(up); return AS; }
    if (strcmp(up, "LOGIN") == 0) { free(up); return LOGIN; }
    if (strcmp(up, "GRANT") == 0) { free(up); return GRANT; }
    if (strcmp(up, "REVOKE") == 0) { free(up); return REVOKE; }
    if (strcmp(up, "ON") == 0) { free(up); return ON; }
    if (strcmp(up, "TO") == 0) { free(up); return TO; }
    if (strcmp(up, "INSERT") == 0) { free(up); return INSERT; }
    if (strcmp(up, "INTO") == 0) { free(up); return INTO; }
    if (strcmp(up, "VALUES") == 0) { free(up); return VALUES; }
    if (strcmp(up, "SELECT") == 0) { free(up); return SELECT; }
    if (strcmp(up, "FROM") == 0) { free(up); return FROM; }
    if (strcmp(up, "WHERE") == 0) { free(up); return WHERE; }
    if (strcmp(up, "DELETE") == 0) { free(up); return DELETE; }
    if (strcmp(up, "UPDATE") == 0) { free(up); return UPDATE; }
    if (strcmp(up, "SET") == 0) { free(up); return SET; }
    if (strcmp(up, "PRIMARY") == 0) { free(up); return PRIMARY; }
    if (strcmp(up, "KEY") == 0) { free(up); return KEY; }
    if (strcmp(up, "FOREIGN") == 0) { free(up); return FOREIGN; }
    if (strcmp(up, "REFERENCES") == 0) { free(up); return REFERENCES; }
    if (strcmp(up, "NULL") == 0) { free(up); return NULL_T; }
    if (strcmp(up, "UNIQUE") == 0) { free(up); return UNIQUE; }
    if (strcmp(up, "INDEX") == 0) { free(up); return INDEX; }
    if (strcmp(up, "BEGIN") == 0) { free(up); return BEGIN_T; }
    if (strcmp(up, "COMMIT") == 0) { free(up); return COMMIT; }
    if (strcmp(up, "ROLLBACK") == 0) { free(up); return ROLLBACK; }
    if (strcmp(up, "AND") == 0) { free(up); return AND; }
    if (strcmp(up, "OR") == 0) { free(up); return OR; }
    if (strcmp(up, "NOT") == 0) { free(up); return NOT; }
    if (strcmp(up, "INT") == 0) { free(up); return INT_TYPE; }
    if (strcmp(up, "CHAR") == 0) { free(up); return CHAR_TYPE; }
    if (strcmp(up, "EXIT") == 0) { free(up); return EXIT; }

    yylval.sval = up;
    return ID;
}

{integer} {
    yylval.ival = atoi(yytext);
    return INT_LIT;
}

{string} {
    char *s = strndup(yytext + 1, yyleng - 2);
    yylval.sval = to_upper(s);
    free(s);
    return STR_LIT;
}

"!="            { return NEQ; }
"<="            { return LE; }
">="            { return GE; }
[(),;=<>*.]     { return yytext[0]; }

. {
    fprintf(stderr, "Lexer error: unknown char '%c'\n", yytext[0]);
}

%%
