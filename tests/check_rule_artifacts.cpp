#include "rules.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

long read_stat(const char *path, const char *key) {
    std::ifstream input(path);
    std::string line;
    std::string prefix = std::string(key) + "=";
    while (std::getline(input, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            return std::strtol(line.c_str() + prefix.size(), nullptr, 10);
        }
    }
    return -1;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: check_rule_artifacts HOSTS ADGUARD STATS\n";
        return EXIT_FAILURE;
    }

    RuleDB db;
    load_rule_file(argv[1], false, db);
    load_rule_file(argv[2], true, db);
    long expected_black = read_stat(argv[3], "black");
    long expected_white = read_stat(argv[3], "white");
    std::cout << "engine black=" << db.black.size()
              << " white=" << db.white.size() << "\n";
    std::cout << "report black=" << expected_black
              << " white=" << expected_white << "\n";

    if (expected_black < 0 || expected_white < 0 ||
        db.black.size() != (size_t)expected_black ||
        db.white.size() != (size_t)expected_white) {
        std::cerr << "rule artifact counts do not match engine semantics\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
