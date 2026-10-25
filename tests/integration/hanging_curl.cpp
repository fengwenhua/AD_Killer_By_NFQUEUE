// ABI-complete library whose initialization never reaches any HTTP request.
// The parent's deadline must work even before curl's own options are applied.
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
extern "C" {
int curl_global_init(long) {
    if (const char *path = std::getenv("ADK_TEST_HANG_PID")) {
        if (FILE *file = std::fopen(path, "w")) { std::fprintf(file, "%ld\n", static_cast<long>(getpid())); std::fclose(file); }
    }
    sleep(30);
    return 0;
}
void curl_global_cleanup() {}
void *curl_easy_init() { return nullptr; }
void curl_easy_cleanup(void *) {}
int curl_easy_setopt(void *, int, ...) { return 0; }
int curl_easy_perform(void *) { return 0; }
int curl_easy_getinfo(void *, int, ...) { return 0; }
}
