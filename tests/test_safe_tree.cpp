#include "safe_tree.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
int main() {
    char pattern[] = "/tmp/adk_safe_tree.XXXXXX";
    const char *created = mkdtemp(pattern); assert(created);
    const fs::path base(created), data = base / "data", outside = base / "outside";
    std::ofstream(outside) << "outside bytes";
    const int parent = open(base.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY); assert(parent >= 0);
    std::string error;
    const auto setup = [&] {
        fs::create_directories(data / "rulesets/gen");
        std::ofstream(data / "rulesets/gen/rules.gz") << "rules";
        std::ofstream(data / ".state.lock") << "";
        fs::create_symlink(outside, data / "outside-link");
        fs::create_hard_link(outside, data / "outside-hardlink");
    };
    setup();
    // No unlink is allowed before all reader/writer leases pass preflight.
    {
        const int reader = open((data / "rulesets/gen").c_str(), O_RDONLY | O_DIRECTORY);
        assert(reader >= 0 && !flock(reader, LOCK_SH | LOCK_NB));
        SafeTreeRemoval tree;
        assert(!tree.prepare(parent, "data", error));
        assert(fs::exists(data / "rulesets/gen/rules.gz") && fs::exists(data / ".state.lock"));
        close(reader);
    }
    {
        const int lease = open((data / ".state.lock").c_str(), O_RDONLY);
        assert(lease >= 0 && !flock(lease, LOCK_SH | LOCK_NB));
        SafeTreeRemoval tree; assert(!tree.prepare(parent, "data", error));
        assert(fs::exists(data / "rulesets/gen/rules.gz")); close(lease);
    }
    {
        SafeTreeRemoval tree;
        assert(tree.prepare(parent, "data", error) && tree.remove(error));
        assert(!fs::exists(data) && fs::file_size(outside) == 13 && fs::hard_link_count(outside) == 1);
    }
    fs::create_directory_symlink(base, data);
    { SafeTreeRemoval tree; assert(!tree.prepare(parent, "data", error)); }
    fs::remove(data); setup();
    {
        SafeTreeRemoval tree; assert(tree.prepare(parent, "data", error));
        fs::rename(data, base / "old-data"); fs::create_directory(data);
        std::ofstream(data / "new-install") << "preserved";
        assert(!tree.remove(error) && fs::exists(data / "new-install"));
    }
    fs::remove_all(data); fs::remove_all(base / "old-data"); setup();
    assert(chmod((data / "rulesets").c_str(), 0777) == 0);
    { SafeTreeRemoval tree; assert(!tree.prepare(parent, "data", error)); }
    assert(fs::exists(data / "rulesets/gen/rules.gz"));
    assert(chmod((data / "rulesets").c_str(), 0755) == 0);
    assert(mkfifo((data / "unexpected-fifo").c_str(), 0600) == 0);
    { SafeTreeRemoval tree; assert(!tree.prepare(parent, "data", error)); }
    assert(fs::exists(data / "rulesets/gen/rules.gz"));
    close(parent); fs::remove_all(base);
    std::cout << "safe tree: preflight leases, directory identity, symlink/hardlink isolation and special-entry refusal passed\n";
}
