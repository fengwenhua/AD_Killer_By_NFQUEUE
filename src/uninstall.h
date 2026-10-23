#pragma once
#include <functional>
#include <string>

struct UninstallResult {
    bool ok = false;
    std::string status = "uninstall_incomplete", message;
};

// Fixed production paths are supplied by main, never by CLI arguments.
// stop performs cleanup; verify repeats the read-only proof AFTER writer drain.
UninstallResult uninstall_module(const std::string &data, const std::string &module,
    const std::function<bool()> &stop, const std::function<bool()> &verify);
