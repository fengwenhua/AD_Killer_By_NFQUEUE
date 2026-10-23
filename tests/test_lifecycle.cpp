#include "lifecycle.h"
#include "logging.h"
#include "exclusive_file_lock.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

int main() {
    char pattern[] = "/tmp/adk-lifecycle.XXXXXX";
    const char *made = mkdtemp(pattern); assert(made);
    const std::string root(made), state = root + "/state.conf";
    assert(write_file_atomic(state.c_str(), "enabled=false\nmanual_stop=true\nautostart=false\n"));
    lifecycle::Ticket first, paused, second, boot;
    std::string error;
    assert(lifecycle::begin(root, lifecycle::Action::START, first, error));
    assert(lifecycle::current(root, first));
    assert(!lifecycle::publish(root, first, [] { assert(false); return true; }, error));
    assert(lifecycle::enable(root, first, error));
    assert(lifecycle::begin(root, lifecycle::Action::STOP, paused, error));
    assert(!lifecycle::enable(root, first, error));
    assert(!lifecycle::publish(root, first, [] { assert(false); return true; }, error));
    assert(read_conf_value(state.c_str(), "enabled") == "false");
    assert(lifecycle::begin(root, lifecycle::Action::START, second, error));
    assert(second.revision != first.revision && second.revision != paused.revision);
    assert(lifecycle::enable(root, second, error));
    unsigned published = 0;
    assert(lifecycle::publish(root, second, [&] { ++published; return true; }, error));
    assert(published == 1);
    assert(!lifecycle::begin(root, lifecycle::Action::BOOT, boot, error));
    assert(lifecycle::set_autostart(root, true, error));
    assert(lifecycle::begin(root, lifecycle::Action::BOOT, boot, error));
    assert(boot.boot && lifecycle::enable(root, boot, error));
    assert(lifecycle::set_autostart(root, false, error));
    assert(!lifecycle::publish(root, boot, [] { assert(false); return true; }, error));

    // The PID publication callback linearizes before a newer STOP. A second
    // process must not change the intent while this short transaction is held.
    int ready[2], release[2]; assert(pipe(ready) == 0 && pipe(release) == 0);
    assert(lifecycle::begin(root, lifecycle::Action::START, second, error));
    assert(lifecycle::enable(root, second, error));
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) {
        close(ready[0]); close(release[1]);
        const bool ok = lifecycle::publish(root, second, [&] {
            char value = 'r'; assert(write(ready[1], &value, 1) == 1);
            assert(read(release[0], &value, 1) == 1); return true;
        }, error);
        _exit(ok ? 0 : 2);
    }
    close(ready[1]); close(release[0]); char value;
    assert(read(ready[0], &value, 1) == 1);
    {
        const auto started = std::chrono::steady_clock::now();
        ExclusiveFileLock blocked((root + "/.intent.lock").c_str(), true, 30);
        assert(!blocked.ok());
        assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    }
    assert(write(release[1], "x", 1) == 1);
    int result; assert(waitpid(child, &result, 0) == child && WIFEXITED(result) && WEXITSTATUS(result) == 0);
    close(ready[0]); close(release[1]);
    assert(lifecycle::begin(root, lifecycle::Action::STOP, paused, error));
    assert(!lifecycle::current(root, second));
    assert(write_file_atomic(state.c_str(), "autostart=true\nenabled=true\nenabled=false\n"));
    assert(!lifecycle::begin(root, lifecycle::Action::BOOT, boot, error));
    for (const char *name : {"state.conf", "lifecycle.intent", ".intent.lock", ".state.lock"})
        assert(unlink((root + "/" + name).c_str()) == 0);
    assert(rmdir(root.c_str()) == 0);
    std::cout << "lifecycle intent revision, pause/boot policy, PID fence and bounded interprocess lock passed\n";
}
