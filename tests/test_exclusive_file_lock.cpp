#include "exclusive_file_lock.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

int main() {
    std::string path = "/tmp/ad_killer_by_nfqueue_daemon_lock_test_" + std::to_string((long)getpid());
    unlink(path.c_str());

    bool second_acquired = false;
    {
        ExclusiveFileLock first(path.c_str());
        if (!first.ok()) {
            std::cerr << "FAIL: first lock should succeed\n";
            unlink(path.c_str());
            return EXIT_FAILURE;
        }
        ExclusiveFileLock second(path.c_str());
        second_acquired = second.ok();
    }
    if (second_acquired) {
        std::cerr << "FAIL: second concurrent lock should fail\n";
        unlink(path.c_str());
        return EXIT_FAILURE;
    }

    ExclusiveFileLock after_release(path.c_str());
    if (!after_release.ok()) {
        std::cerr << "FAIL: lock should be reusable after release\n";
        unlink(path.c_str());
        return EXIT_FAILURE;
    }
    unlink(path.c_str());
    std::cout << "exclusive file lock tests passed\n";
    return EXIT_SUCCESS;
}
