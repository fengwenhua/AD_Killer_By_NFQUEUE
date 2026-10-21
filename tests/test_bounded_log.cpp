#include "bounded_log.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

off_t file_size(const std::string &path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 ? info.st_size : -1;
}

}  // namespace

int main() {
    char dir_template[] = "/tmp/ad_killer_by_nfqueue_log.XXXXXX";
    char *created = mkdtemp(dir_template);
    expect(created != nullptr, "temporary log directory should be created");
    if (!created) return EXIT_FAILURE;
    std::string dir = created;
    std::string path = dir + "/engine.log";

    for (int i = 0; i < 12; i++) {
        std::string line = "line-" + std::to_string(i) + "-abcdefghijklmnop\n";
        expect(append_bounded_log(path, line, 64, 2),
               "bounded append should succeed");
    }
    expect(file_size(path) >= 0 && file_size(path) <= 64,
           "active log must stay under its size limit");
    expect(file_size(path + ".1") >= 0 && file_size(path + ".1") <= 64,
           "first rotated log should exist and stay bounded");
    expect(file_size(path + ".2") >= 0 && file_size(path + ".2") <= 64,
           "second rotated log should exist and stay bounded");
    expect(file_size(path + ".3") < 0,
           "rotation must not retain more than configured backups");

    std::string single = dir + "/update.log";
    expect(append_bounded_log(single, std::string(50, 'a'), 64, 0) &&
               append_bounded_log(single, std::string(50, 'b'), 64, 0),
           "backup-free update log should truncate and continue");
    expect(file_size(single) == 50,
           "backup-free rotation should keep only the newest bounded chunk");

    unlink(path.c_str());
    unlink((path + ".1").c_str());
    unlink((path + ".2").c_str());
    unlink(single.c_str());
    rmdir(dir.c_str());

    if (failures != 0) {
        std::cerr << failures << " bounded log test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "bounded log tests passed\n";
    return EXIT_SUCCESS;
}
