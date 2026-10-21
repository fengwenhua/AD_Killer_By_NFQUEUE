#include "cli_utils.h"

#include <cstdlib>
#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

}  // namespace

int main() {
    int value = 99;
    expect(parse_integer_arg("0", value) && value == 0, "zero should parse");
    expect(parse_integer_arg("1", value) && value == 1, "one should parse");
    expect(parse_integer_arg("2", value) && value == 2, "two should parse");
    expect(!parse_integer_arg("", value), "empty argument should fail");
    expect(!parse_integer_arg(nullptr, value), "null argument should fail");
    expect(!parse_integer_arg("1x", value), "trailing characters should fail");
    expect(!parse_integer_arg("999999999999999999999", value), "overflow should fail");

    expect(valid_scope(0) && valid_scope(1), "scope 0 and 1 should be valid");
    expect(!valid_scope(-1) && !valid_scope(2), "other scopes should be invalid");
    expect(valid_mode(1) && valid_mode(2), "mode 1 and 2 should be valid");
    expect(!valid_mode(0) && !valid_mode(3), "other modes should be invalid");

    if (failures != 0) {
        std::cerr << failures << " CLI utility test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "CLI utility tests passed\n";
    return EXIT_SUCCESS;
}
