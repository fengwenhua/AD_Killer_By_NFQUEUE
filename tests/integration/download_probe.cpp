#include "net.h"

#include <cstdio>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    const std::string error = download_file(argv[1], argv[2]);
    net_curl_shutdown();
    if (!error.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    return 0;
}
