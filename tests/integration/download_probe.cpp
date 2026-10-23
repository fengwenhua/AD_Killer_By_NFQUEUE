#include "net.h"
#include "operation_gate.h"

#include <cstdio>

int main(int argc, char **argv) {
    if (argc != 3 && argc != 5) return 2;
    if (argc == 5) operation_marker_path = argv[3];
    std::string prefix;
    bool limited = false;
    const std::string error = argc == 5 && std::string(argv[4]) == "prefix" ?
        download_prefix(argv[1], prefix, limited, 512) : download_file(argv[1], argv[2]);
    if (argc == 5 && !error.empty() && operation_closing()) {
        // Repeated attempts in this process must stay closed too.
        if (download_file(argv[1], argv[2]).empty()) return 3;
    }
    net_curl_shutdown();
    if (!error.empty()) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    return 0;
}
