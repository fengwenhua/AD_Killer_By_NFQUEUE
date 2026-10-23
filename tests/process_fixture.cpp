#include <cstdio>
#include <cstring>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (strcmp(argv[1], "echo") == 0) {
        if (argc != 3) return 2;
        fputs(argv[2], stdout); fputs("diagnostic", stderr); return 0;
    }
    if (strcmp(argv[1], "fail") == 0) return 7;
    if (strcmp(argv[1], "descendant") == 0) {
        const pid_t child = fork();
        if (child < 0) return 2;
        if (child != 0) { printf("%d\n", child); return 0; }
        for (;;) pause();
    }
    if (strcmp(argv[1], "sleep") == 0) { sleep(5); return 0; }
    FILE *out = strcmp(argv[1], "stderr") == 0 ? stderr : stdout;
    for (unsigned i = 0; i < 100000; ++i) fputc('x', out);
    return 0;
}
