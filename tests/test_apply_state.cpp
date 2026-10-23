#include "apply_state.h"
#include "logging.h"
#include <cassert>
#include <filesystem>
#include <sys/wait.h>

using namespace apply_state;
int main() {
    char directory[] = "/tmp/adk-apply-state-XXXXXX";
    assert(mkdtemp(directory)); const std::string root(directory);
    const ProcessIdentity owner{1234, "12345678-1234-1234-1234-123456789abc", 100};
    assert(owner.valid());
    std::string error, first, second, newest, captured;
    assert(!read(root).valid && view(read(root), owner, true).phase == "unverified");
    assert(queue(root, first, error) && valid_revision(first));
    assert(view(read(root), owner, false).phase == "deferred");
    assert(capture(root, false, owner, captured, error) && captured == first);
    assert(view(read(root), owner, true).phase == "applying");
    Applied actual; actual.generation = "123-4"; actual.black = 2; actual.white = 1; actual.updated_at = 100;
    actual.config.scope = 1; actual.config.block_quic = true; actual.config.intercept_uids = {10001, 10002};
    assert(finish(root, owner, captured, &actual, "", error));
    auto result = view(read(root), owner, true);
    assert(result.phase == "applied" && result.actual_verified);
    assert(result.record.actual.config.intercept_uids == actual.config.intercept_uids);
    assert(dirty(root, error)); // durable BEFORE any mutable input is replaced
    assert(view(read(root), owner, true).phase == "saved");
    assert(!capture(root, false, owner, captured, error));
    assert(queue(root, second, error) && second != first && pending(read(root), owner));
    assert(capture(root, false, owner, captured, error) && captured == second);
    assert(queue(root, newest, error) && newest != second && !current(root, owner, second));
    actual.black = 3;
    assert(finish(root, owner, second, &actual, "", error));
    result = view(read(root), owner, true);
    assert(result.phase == "pending" && result.record.applied == second && pending(read(root), owner));
    assert(view(read(root), owner, true, second).phase == "superseded");
    assert(capture(root, false, owner, captured, error) && captured == newest);
    assert(finish(root, owner, newest, nullptr, "IPv6 failed\n<script>\"&", error));
    result = view(read(root), owner, true);
    assert(result.phase == "failed" && result.actual_verified && result.record.applied == second);
    assert(result.record.actual.black == 3 && result.record.error == "IPv6 failed\n<script>\"&" && !pending(read(root), owner));
    auto reused = owner; ++reused.started;
    assert(!view(read(root), reused, true).actual_verified);
    auto rebooted = owner; rebooted.boot[0] = 'a';
    assert(!view(read(root), rebooted, true).actual_verified);
    assert(!finish(root, reused, newest, &actual, "", error));
    assert(dirty(root, error) && capture(root, true, reused, captured, error));
    assert(read(root).applied.empty()); // new daemon cannot inherit old ACKs
    assert(finish(root, reused, captured, &actual, "", error));
    assert(view(read(root), reused, true).phase == "applied");
    for (int i = 0; i < 8; ++i) {
        const auto child = fork(); assert(child >= 0);
        if (!child) { std::string message; _exit(dirty(root, message) ? 0 : 1); }
    }
    for (int i = 0; i < 8; ++i) { int status = 0; assert(wait(&status) > 0 && WIFEXITED(status) && !WEXITSTATUS(status)); }
    assert(read(root).valid && !read(root).submitted);
    assert(write_file_atomic((root + "/apply.state").c_str(), "corrupt record\n"));
    assert(!queue(root, newest, error) && !dirty(root, error) && !read(root).valid);
    assert(unlink((root + "/apply.state").c_str()) == 0);
    assert(symlink("/dev/null", (root + "/apply.state").c_str()) == 0);
    assert(!queue(root, newest, error) && !read(root).valid);
    assert(unlink((root + "/apply.state").c_str()) == 0);
    assert(mkfifo((root + "/apply.state").c_str(), 0600) == 0);
    assert(!queue(root, newest, error));
    assert(unlink((root + "/apply.state").c_str()) == 0);
    assert(write_file_atomic((root + "/apply.state").c_str(), std::string(1024u * 1024u + 1, 'x')));
    assert(!queue(root, newest, error));
    uint64_t started = 0;
    std::string stat = "1234 (name) with\nparens)) S";
    for (int field = 4; field < 22; ++field) stat += " 0";
    stat += " 12345 0 0\n";
    assert(parse_process_start(stat, 1234, started) && started == 12345);
    assert(!parse_process_start(stat, 1235, started));
    assert(!parse_process_start("1234 (x) Z 0 0", 1234, started));
    assert(!parse_u64("-1", started) && !parse_u64("18446744073709551616", started));
    std::filesystem::create_directories(root + "/proc/sys/kernel/random");
    std::filesystem::create_directories(root + "/proc/1234");
    assert(write_file_atomic((root + "/proc/sys/kernel/random/boot_id").c_str(), owner.boot + "\n"));
    assert(write_file_atomic((root + "/proc/1234/stat").c_str(), stat));
    auto observed = process_identity(1234, root + "/proc");
    assert(observed.valid() && observed.started == 12345 && observed.boot == owner.boot);
    assert(write_file_atomic((root + "/proc/sys/kernel/random/boot_id").c_str(), "bad\n"));
    assert(!process_identity(1234, root + "/proc").valid());
    std::filesystem::remove_all(root);
    puts("apply journal: crash-before-submit, revisions, supersession, identity, rollback, contention and bounded malformed reads passed");
}
