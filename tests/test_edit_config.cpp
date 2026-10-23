#include "edit_config.h"
#include "logging.h"
#include <cassert>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

int main() {
    char pattern[] = "/tmp/adk-edit-config.XXXXXX";
    const char *created = mkdtemp(pattern); assert(created);
    const std::string root(created), path = root + "/custom_rules.txt";
    assert(edit_config::read(root, "../anything").status == "invalid_target");
    assert(edit_config::read(root, "custom").status == "missing");
    auto saved = edit_config::save(root, "custom", "missing", "ads.example.com\n");
    assert(saved.status == "ok" && saved.committed && saved.revision.size() == 64);
    auto read = edit_config::read(root, "custom");
    assert(read.status == "ok" && read.content == "ads.example.com\n" && read.revision == saved.revision);
    assert(edit_config::save(root, "custom", "missing", "").status == "conflict");
    assert(edit_config::save(root, "custom", "bad", "").status == "invalid_revision");
    for (const auto &invalid : {std::string("||example.com^$third-party\n"), std::string("<html>error</html>\n"),
                               std::string("x\0y", 3), std::string("#\xff", 2), std::string(1025, '#'),
                               std::string(edit_config::MAX_BYTES + 1, 'x')}) {
        assert(edit_config::save(root, "custom", read.revision, invalid).status == "invalid_content");
        assert(edit_config::read(root, "custom").content == read.content);
    }
    std::string many_lines;
    for (size_t i = 0; i <= edit_config::MAX_LINES; ++i) many_lines += "#\n";
    assert(edit_config::save(root, "custom", read.revision, many_lines).status == "invalid_content");
    assert(edit_config::save(root, "allow", "missing", "../../bad\n").status == "invalid_content");
    assert(edit_config::save(root, "allow", "missing", "com.example.app\n").status == "ok");
    assert(edit_config::save(root, "intercept", "missing", "com.other.app\n").status == "ok");

    // Explicit pre-commit failure and post-rename durability uncertainty differ.
    const auto failed = edit_config::save(root, "custom", read.revision, "new.example.com\n",
        [](const char *, const std::string &) { return AtomicWriteResult{}; });
    assert(failed.status == "write_error" && !failed.committed);
    assert(edit_config::read(root, "custom").content == read.content);
    for (const int code : {ENOSPC, EIO}) {
        const auto error = edit_config::save(root, "custom", read.revision, "new.example.com\n",
            [code](const char *, const std::string &) { errno = code; return AtomicWriteResult{}; });
        assert(error.status == "write_error" && !error.committed);
        assert(edit_config::read(root, "custom").content == read.content);
    }
    const auto uncertain = edit_config::save(root, "custom", read.revision, "new.example.com\n",
        [](const char *name, const std::string &text) {
            assert(write_file_atomic(name, text)); return AtomicWriteResult{true, false};
        });
    assert(uncertain.status == "durability_unknown" && uncertain.committed);
    assert(edit_config::read(root, "custom").revision == uncertain.revision);

    // Same base revision in two windows: at most one may replace the file.
    read = edit_config::read(root, "custom");
    std::vector<edit_config::Result> results(2);
    std::thread one([&] { results[0] = edit_config::save(root, "custom", read.revision, "one.example.com\n"); });
    std::thread two([&] { results[1] = edit_config::save(root, "custom", read.revision, "two.example.com\n"); });
    one.join(); two.join();
    assert((results[0].status == "ok") != (results[1].status == "ok"));
    for (const auto &result : results) assert(result.status == "ok" || result.status == "busy" || result.status == "conflict");

    // Readers (including a simultaneous reload) see whole old/new files only.
    const std::string large = std::string(1000, '#') + "\n";
    const std::string small = "small.example.com\n";
    assert(write_file_atomic(path.c_str(), small));
    std::atomic<bool> done{false};
    std::thread reader([&] {
        do {
            const auto value = edit_config::read(root, "custom");
            assert(value.status == "ok" && (value.content == small || value.content == large));
        } while (!done.load());
    });
    for (int i = 0; i < 30; ++i) {
        const auto prior = edit_config::read(root, "custom");
        assert(edit_config::save(root, "custom", prior.revision, i % 2 ? small : large).status == "ok");
    }
    done.store(true); reader.join();

    // Real short-write failure cannot publish the temporary partial contents.
    read = edit_config::read(root, "custom");
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit limit {32, 32};
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(2);
        const auto result = edit_config::save(root, "custom", read.revision, std::string(1000, '#') + "\n");
        _exit(result.status == "write_error" && !result.committed ? 0 : 3);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(edit_config::read(root, "custom").content == read.content);

    // Non-regular and symlink files are errors, never empty configuration.
    assert(unlink(path.c_str()) == 0);
    assert(symlink((root + "/app_whitelist.txt").c_str(), path.c_str()) == 0);
    assert(edit_config::read(root, "custom").status == "read_error");
    assert(edit_config::save(root, "custom", read.revision, "").status == "read_error");
    assert(unlink(path.c_str()) == 0);
    assert(mkfifo(path.c_str(), 0600) == 0);
    assert(edit_config::read(root, "custom").status == "read_error");
    assert(unlink(path.c_str()) == 0);
    assert(write_file_atomic(path.c_str(), ""));
    const auto empty = edit_config::read(root, "custom");
    assert(empty.status == "ok" && empty.content.empty() && empty.revision.size() == 64);
    assert(edit_config::save(root, "custom", empty.revision, "# 中文注释 😀\n@@||safe.example.org^\n").status == "ok");
    assert(chmod(root.c_str(), 0777) == 0);
    assert(edit_config::read(root, "custom").status == "read_error");
    assert(edit_config::save(root, "custom", empty.revision, "").status == "read_error");
    assert(chmod(root.c_str(), 0700) == 0);
    for (const char *file : {"custom_rules.txt", "app_whitelist.txt", "app_intercept.txt", ".editable.lock"})
        assert(unlink((root + "/" + file).c_str()) == 0);
    assert(rmdir(root.c_str()) == 0);  // Also detects leaked staging files.
    std::cout << "editable config validation, CAS, UTF-8, short write, atomicity and durability outcomes passed\n";
}
