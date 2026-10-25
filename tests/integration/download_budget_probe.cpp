#include "net.h"
#include "work_budget.h"
#include "task_context.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/file.h>

int main(int argc, char **argv) {
    if (argc < 7) return 2;
    if (const char *lease = std::getenv("ADK_TEST_DOWNLOAD_LEASE")) {
        task_lease_fd = open(lease, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (task_lease_fd < 0 || flock(task_lease_fd, LOCK_SH)) return 3;
    }
    WorkBudget task(std::strtoull(argv[1], nullptr, 10), std::strtoull(argv[3], nullptr, 10));
    for (int index = 5; index < argc; ++index) {
        WorkBudget source(std::strtoull(argv[2], nullptr, 10), UINT64_MAX);
        const auto error = download_file(argv[index], std::string(argv[4]) + "/" + std::to_string(index - 5));
        printf("%d %d %llu\n", index - 5, error.empty() ? 1 : 0,
               static_cast<unsigned long long>(WorkBudget::remaining_bytes(UINT64_MAX)));
        if (!error.empty()) fprintf(stderr, "%s\n", error.c_str());
    }
    net_curl_shutdown();
}
