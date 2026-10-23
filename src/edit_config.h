#pragma once
#include "logging.h"
#include <functional>
#include <string>

namespace edit_config {
constexpr size_t MAX_BYTES = 1024u * 1024u;
constexpr size_t MAX_LINES = 20000;
struct Result {
    std::string status = "error";
    std::string content;
    std::string revision;
    std::string message;
    bool committed = false;
};
using Commit = std::function<AtomicWriteResult(const char *, const std::string &)>;
// root is explicit for isolated tests. Production CLI always supplies DATA_DIR;
// target is an enum-like name, NEVER an arbitrary file path.
Result read(const std::string &root, const std::string &target);
Result save(const std::string &root, const std::string &target,
            const std::string &revision, const std::string &text, const Commit &commit = {});
int command(const std::string &root, bool saving, const std::string &target,
            const std::string &revision = "");
}
