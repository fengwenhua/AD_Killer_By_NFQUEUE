#include "source_preview.h"
#include "source_config.h"
#include "net.h"
#include "cli_utils.h"
#include <cstdio>

int main(int argc, char **argv) {
    int count = SOURCE_PREVIEW_DEFAULT;
    if ((argc != 2 && argc != 3) || (argc == 3 && !parse_integer_arg(argv[2], count))) return 2;
    auto preview = preview_source(argv[1], count < 0 ? 0 : static_cast<size_t>(count));
    net_curl_shutdown();
    printf("{\"text\":%s,\"error\":%s,\"characters\":%zu,\"truncated\":%s}\n",
           source_json_string(preview.text).c_str(), source_json_string(preview.error).c_str(),
           preview.characters, preview.truncated ? "true" : "false");
    return preview.error.empty() ? 0 : 1;
}
