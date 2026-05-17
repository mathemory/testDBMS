CC = gcc
CFLAGS = -Wall -g -Isrc
LEX = flex
YACC = bison
SRCS = src/dbms.c src/executor.c src/storage.c

all: dbms

src/sql.tab.c src/sql.tab.h: src/sql.y
	$(YACC) -d -o src/sql.tab.c src/sql.y

src/lex.yy.c: src/sql.l src/sql.tab.h
	$(LEX) -o src/lex.yy.c src/sql.l

dbms: src/lex.yy.c src/sql.tab.c $(SRCS)
	$(CC) $(CFLAGS) -o dbms src/lex.yy.c src/sql.tab.c $(SRCS) -lfl

clean:
	rm -f src/lex.yy.c src/sql.tab.c src/sql.tab.h dbms

reset:
	rm -rf data

run: dbms
	./dbms test/test.sql

test: dbms
	./dbms test/test.sql

.PHONY: all clean reset run test
