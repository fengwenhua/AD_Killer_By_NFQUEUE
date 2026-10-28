#include "source_staging.h"
#include "task_context.h"
#include "work_budget.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>

namespace fs = std::filesystem;
static void put(const fs::path &path, const std::string &text = "owned") {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    assert(!close(fd));
}
static pid_t holder(const char *fixture, const fs::path &ready, const fs::path &release) {
    const auto child = fork(); assert(child >= 0);
    if (!child) {
        if (!inherit_task_lease()) _exit(127);
        execl(fixture, fixture, "wait-file", ready.c_str(), release.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    for (int attempt = 0; attempt < 200 && !fs::exists(ready); ++attempt) usleep(10000);
    assert(fs::exists(ready));
    return child;
}
static void finish(pid_t child, const fs::path &release) {
    put(release);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    char temporary[] = "/tmp/adk-source-staging-XXXXXX";
    assert(mkdtemp(temporary));
    const fs::path base(temporary);
    operation_marker_path = (base / "closing").string();
    pid_t children[2];
    fs::path stages[2];
    for (int index = 0; index < 2; ++index) {
        SourceStaging staging(base);
        assert(staging.ok()); stages[index] = staging.path();
        put(stages[index] / "source-download-0.part.tmp.123");
        children[index] = holder(argv[1], base / ("ready-" + std::to_string(index)), base / ("release-" + std::to_string(index)));
    }
    assert(task_download_lease_fd == -1 && fs::exists(stages[0]) && fs::exists(stages[1]));
    { SourceStaging refused(base); assert(!refused.ok()); }
    finish(children[0], base / "release-0");
    { SourceStaging next(base); assert(next.ok() && !fs::exists(stages[0]) && fs::exists(stages[1])); }
    finish(children[1], base / "release-1");
    { SourceStaging next(base); assert(next.ok() && !fs::exists(stages[1])); }
    assert(!fs::exists(base / "source_staging"));
    {
        SourceStaging staging(base); assert(staging.ok());
        for (int index : {0, 2, 40, 128, 129})
            put(fs::path(staging.path()) / ("source-download-" + std::to_string(index) + ".part"));
    }
    assert(!fs::exists(base / "source_staging"));
    const auto outside = base / "outside";
    put(outside);
    fs::path unsafe;
    {
        SourceStaging staging(base); assert(staging.ok()); unsafe = staging.path();
        put(unsafe / "source-download-0.part");
        fs::create_symlink(outside, unsafe / "source-download-1.part.tmp.123");
    }
    assert(fs::exists(unsafe / "source-download-0.part"));
    { SourceStaging refused(base); assert(!refused.ok()); }
    assert(fs::file_size(outside) == 5);
    fs::remove(unsafe / "source-download-1.part.tmp.123");
    { SourceStaging next(base); assert(next.ok() && !fs::exists(unsafe)); }
    { WorkBudget expired(0, 100); SourceStaging refused(base); assert(!refused.ok()); }
    assert(task_download_lease_fd == -1);
    operation_marker_path.clear();
    fs::remove_all(base);
    std::cout << "source staging: inherited exec leases, bounded pinned writers, crash-temp GC, whole-inventory refusal and symlink isolation passed\n";
}
