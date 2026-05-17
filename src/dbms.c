#include "dbms.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int yyparse(void);
extern FILE *yyin;

static void storage_init(void) {
    FILE *fp;

    if (mkdir("data", 0755) != 0 && errno != EEXIST) {
        perror("mkdir");
        return;
    }

    if (access("data/sys.dat", F_OK) != 0) {
        fp = fopen("data/sys.dat", "w");
        if (!fp) {
            perror("fopen");
            return;
        }
        fclose(fp);
    }
}

int main(int argc, char *argv[]) {
    const char *script_path = NULL;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    storage_init();

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-auth") == 0) {
            continue;
        }

        if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }

        if (!script_path) {
            script_path = argv[i];
            continue;
        }

        fprintf(stderr, "Unexpected extra argument: %s\n", argv[i]);
        return 1;
    }

    if (script_path) {
        yyin = fopen(script_path, "r");
        if (!yyin) {
            perror("fopen");
            return 1;
        }
    }

    yyparse();
    return 0;
}
