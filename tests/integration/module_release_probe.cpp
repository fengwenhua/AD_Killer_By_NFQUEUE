// Explicit live-network smoke probe. Writes only the caller's output directory.
#include "module_update.h"
#include "net.h"
#include <cstdio>
#include <string>
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "notes")
        return module_update::release_notes();
    if (argc == 3 && std::string(argv[1]) == "check")
        return module_update::release_info(std::string(argv[2]) == "preview");
    if (argc != 4 || std::string(argv[1]) != "download") return 2;
    net_curl_init();
    auto error = module_update::download(argv[2],argv[3],download_file,download_prefix);
    net_curl_shutdown();
    if (!error.empty()) { fprintf(stderr,"%s\n",error.c_str()); return 1; }
    printf("verified release v%s\n",module_update::ready_version(argv[3]).c_str());
    return 0;
}
