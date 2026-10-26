#include "process_runner.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string fixture(argv[1]);
    const std::string literal = "a '; $(not-a-command) \"中文\"";
    auto result = run_process({fixture, "echo", literal}, 1000, 1024);
    assert(result.ok() && result.output == literal && result.diagnostic == "diagnostic");
    result = run_process({fixture, "fail"});
    assert(!result.ok() && result.exit_code == 7 && result.reaped);
    result = run_process({"/does/not/exist/adk-test"});
    assert(!result.ok() && !result.error.empty());
    assert(!run_process({fixture, std::string("x\0y", 3)}).ok());
    assert(!run_process({fixture, "sleep"}, 0).ok());
    for (const char *operation : {"stdout", "stderr"}) {
        const auto started = std::chrono::steady_clock::now();
        result = run_process({fixture, operation}, 1000, 32);
        assert(result.overflow && !result.ok() && result.reaped);
        assert(result.output.size() + result.diagnostic.size() <= 32);
        assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    }
    for (const char *operation : {"sleep", "descendant"}) {
        const auto started = std::chrono::steady_clock::now();
        result = run_process({fixture, operation}, std::string(operation) == "descendant" ? 250 : 80, 1024);
        assert(result.timed_out && !result.ok() && result.reaped);
        if (std::string(operation) == "descendant") assert(result.exit_code == 0 && !result.output.empty());
        assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    }
    const auto cancellation_started = std::chrono::steady_clock::now();
    result = run_process({fixture, "sleep"}, 2000, 1024, -1, [&] {
        return std::chrono::steady_clock::now() - cancellation_started >= std::chrono::milliseconds(50);
    });
    assert(result.cancelled && !result.timed_out && !result.ok() && result.reaped && result.group_gone);
    assert(std::chrono::steady_clock::now() - cancellation_started < std::chrono::seconds(1));
    result = run_process({fixture, "echo", "not spawned"}, 2000, 1024, -1, [] { return true; });
    assert(result.cancelled && result.exit_code == -1 && result.output.empty());
    result = run_process({fixture, "echo", "not spawned"}, 2000, 1024, -1, []() -> bool { throw 1; });
    assert(result.cancelled && result.exit_code == -1 && result.output.empty());
    char lease_path[] = "/tmp/adk-process-lease.XXXXXX";
    const int lease = mkstemp(lease_path); assert(lease >= 0);
    assert(fcntl(lease, F_SETFD, FD_CLOEXEC) == 0);
    const pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        struct rlimit limit {32, 32};
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0) _exit(2);
        const auto bounded = run_process({fixture, "echo", "leased"}, 1000, 1024, lease);
        _exit(bounded.ok() && bounded.output == "leased" ? 0 : 3);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(lease); assert(unlink(lease_path) == 0);
    std::cout << "bounded argv execution, separate output, exit status, overflow and descendant-pipe cancellation passed\n";
}
