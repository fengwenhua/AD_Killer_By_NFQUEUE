#include "net.h"
#include "source_config.h"
#include "work_budget.h"
#include <cstdio>
#include <memory>

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    std::unique_ptr<WorkBudget> budget;
    if (std::string(argv[5]) == "scoped") budget = std::make_unique<WorkBudget>(5000, 64u * 1024u * 1024u);
    const auto response = download_file_response(argv[1], argv[2], {argv[3], argv[4]});
    printf("{\"error\":%s,\"status\":%ld,\"url\":%s,\"etag\":%s,\"last_modified\":%s,\"bytes\":%zu,\"not_modified\":%s}\n",
           source_json_string(response.error).c_str(), response.http_status,
           source_json_string(response.effective_url).c_str(), source_json_string(response.validators.etag).c_str(),
           source_json_string(response.validators.last_modified).c_str(), response.bytes, response.not_modified ? "true" : "false");
    net_curl_shutdown();
    return response.error.empty() ? 0 : 1;
}
