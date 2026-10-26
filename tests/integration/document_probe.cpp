#include "net.h"
#include "work_budget.h"
#include "source_config.h"
#include "cli_utils.h"
#include <cstdio>

int main(int argc, char **argv) {
    int milliseconds = 0, bytes = 0, cap = 0;
    if (argc < 5 || !parse_integer_arg(argv[1], milliseconds) || milliseconds <= 0 ||
        !parse_integer_arg(argv[2], bytes) || bytes < 0 || !parse_integer_arg(argv[3], cap) || cap <= 0) return 2;
    WorkBudget budget(static_cast<unsigned>(milliseconds), static_cast<unsigned>(bytes));
    printf("[");
    for (int i = 4; i < argc; ++i) {
        std::string body; long status = 0;
        const auto error = download_document_status(argv[i], body, static_cast<size_t>(cap), status);
        printf("%s{\"bytes\":%zu,\"http_status\":%ld,\"error\":%s,\"remaining\":%llu}", i == 4 ? "" : ",", body.size(), status,
            source_json_string(error).c_str(), static_cast<unsigned long long>(WorkBudget::remaining_bytes(UINT64_MAX)));
        if (!error.empty()) break;
    }
    puts("]");
}
