#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    bool overflow = false;
    bool reaped = true;
    bool group_gone = true;
    bool cancelled = false;
    std::string output, diagnostic, error;
    bool ok() const { return exit_code == 0 && error.empty() && !timed_out && !overflow && !cancelled && reaped && group_gone; }
};
// No shell expansion. stdout/stderr are separate and jointly bounded. A fresh
// process group allows timeout cancellation of the command and its children.
ProcessResult run_process(const std::vector<std::string> &arguments,
                          unsigned timeout_ms = 6000, size_t output_limit = 2u * 1024u * 1024u,
                          int inherited_lock = -1,
                          const std::function<bool()> &cancelled = {});
