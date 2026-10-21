#include "logging.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>

int main() {
    std::string base = "/tmp/ad_killer_by_nfqueue_config_test_" + std::to_string((long)getpid());
    std::string conf = base + ".conf";
    std::string lock = base + ".lock";
    unlink(conf.c_str());
    unlink(lock.c_str());

    constexpr int kWriters = 12;
    std::vector<std::thread> writers;
    std::vector<int> results(kWriters, 0);
    for (int i = 0; i < kWriters; i++) {
        writers.emplace_back([&, i]() {
            std::string key = "key" + std::to_string(i);
            std::string value = "value" + std::to_string(i);
            results[i] = write_conf_value_with_lock(
                conf.c_str(), key.c_str(), value.c_str(), lock.c_str()) ? 1 : 0;
        });
    }
    for (auto &writer : writers) writer.join();

    bool ok = true;
    for (int i = 0; i < kWriters; i++) {
        std::string key = "key" + std::to_string(i);
        std::string value = "value" + std::to_string(i);
        if (!results[i] || read_conf_value(conf.c_str(), key.c_str()) != value) {
            std::cerr << "FAIL: concurrent update lost " << key << "\n";
            ok = false;
        }
    }

    std::string missing_dir = base + "/missing/config.conf";
    if (write_conf_value_with_lock(missing_dir.c_str(), "key", "value",
                                   (base + "/missing/lock").c_str())) {
        std::cerr << "FAIL: write failure should be reported\n";
        ok = false;
    }

    // Force a real short write: publishing the temporary file would corrupt
    // the previous configuration even though the final rename succeeds.
    std::string before;
    read_file(conf.c_str(), before);
    pid_t child = fork();
    if (child == 0) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit = {64, 64};
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(2);
        _exit(write_file_atomic(conf.c_str(), std::string(4096, 'x')) ? 3 : 0);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "FAIL: short write should be reported\n";
        ok = false;
    }
    std::string after;
    if (!read_file(conf.c_str(), after) || after != before) {
        std::cerr << "FAIL: failed atomic write must preserve old config\n";
        ok = false;
    }

    unlink(conf.c_str());
    unlink(lock.c_str());
    if (!ok) return EXIT_FAILURE;
    std::cout << "config store tests passed\n";
    return EXIT_SUCCESS;
}
