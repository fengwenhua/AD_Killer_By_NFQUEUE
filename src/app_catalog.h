#pragma once
#include "process_runner.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace app_catalog {
constexpr size_t MAX_BYTES = 8u * 1024u * 1024u;
constexpr size_t MAX_INSTANCES = 100000;
constexpr uint32_t USER_RANGE = 100000;
struct Instance {
    std::string package;
    uint32_t user = 0, uid = 0;
};
struct Catalog {
    bool known = false;
    std::vector<Instance> instances;
    std::vector<std::string> without_uid;
    std::string revision, error;
};
struct Mapping { std::string package; uint32_t uid = 0; };
struct Resolution {
    bool known = false;
    std::vector<Mapping> mappings;
    std::vector<std::string> uninstalled, shared_packages;
    size_t other_user_instances = 0;
    std::string revision, error;
};
using Runner = std::function<ProcessResult(const std::vector<std::string> &, unsigned, size_t)>;
// Output from the fixed "list packages -U --user all" query. Missing UID
// records (e.g. APEX) are explicit, never interpreted as uninstalled packages.
Catalog parse(const std::string &text);
// One total 4 s / 8 MiB allowance. Failure or incomplete output is unknown;
// alternate command selection never converts a partial result into success.
Catalog query(const Runner &runner = {});
// Legacy package-only lists select the primary user (0). Never collapse a
// work/clone UID into the owner's appId or silently extend an existing list.
Resolution resolve_primary(const Catalog &, const std::vector<std::string> &packages);
} // namespace app_catalog
