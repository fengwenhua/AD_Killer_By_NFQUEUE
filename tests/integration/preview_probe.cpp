#include "source_preview.h"
#include "source_config.h"
#include "net.h"
#include "cli_utils.h"
#include <cstdio>
#include <string>

int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--http-status") {
        std::string body;
        bool limited = false;
        long status = 0;
        auto error = download_prefix_status(argv[2],body,limited,128,status);
        net_curl_shutdown();
        printf("{\"http_status\":%ld,\"bytes\":%zu,\"error\":%s}\n",status,body.size(),source_json_string(error).c_str());
        return error.empty() ? 0 : 1;
    }
    int count = SOURCE_PREVIEW_DEFAULT;
    if ((argc != 2 && argc != 3) || (argc == 3 && !parse_integer_arg(argv[2], count))) return 2;
    auto preview = preview_source(argv[1], count < 0 ? 0 : static_cast<size_t>(count));
    net_curl_shutdown();
    printf("{\"text\":%s,\"error\":%s,\"characters\":%zu,\"truncated\":%s}\n",
           source_json_string(preview.text).c_str(), source_json_string(preview.error).c_str(),
           preview.characters, preview.truncated ? "true" : "false");
    return preview.error.empty() ? 0 : 1;
}
