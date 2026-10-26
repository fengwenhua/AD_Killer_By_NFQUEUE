#include "app_catalog.h"
#include "cli_utils.h"
#include "process_identity.h"
#include "sha256.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace app_catalog {
static bool catalog_name(const std::string &name) {
    if (valid_package_name(name)) return true;
    // Framework/resource packages may have a single segment ("android" and
    // OEM variants). They are catalog records, not newly selectable app keys.
    if (name.empty() || name.size() > 255 ||
        !((name.front() >= 'a' && name.front() <= 'z') || (name.front() >= 'A' && name.front() <= 'Z'))) return false;
    return name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos;
}

Catalog parse(const std::string &text) {
    const auto invalid = [](const char *reason) { Catalog result; result.error = reason; return result; };
    if (text.size() > MAX_BYTES || text.find('\0') != std::string::npos) return invalid("package_output_limit_or_nul");
    std::map<std::pair<std::string, uint32_t>, uint32_t> unique;
    std::set<std::string> missing;
    std::istringstream input(text);
    std::string row;
    size_t lines = 0;
    while (std::getline(input, row)) {
        if (++lines > MAX_INSTANCES || row.size() > 65536) return invalid("package_record_limit");
        if (!row.empty() && row.back() == '\r') row.pop_back();
        if (row.empty()) continue;
        if (row.compare(0, 8, "package:") != 0) return invalid("unexpected_package_output");
        const auto separator = row.find(" uid:", 8);
        const auto package = row.substr(8, separator == std::string::npos ? std::string::npos : separator - 8);
        if (!catalog_name(package)) return invalid("invalid_package_name");
        if (separator == std::string::npos) { missing.insert(package); continue; }
        const auto numbers = row.substr(separator + 5);
        if (numbers.empty() || numbers.back() == ',') return invalid("missing_uid");
        for (size_t begin = 0; begin < numbers.size();) {
            const auto comma = numbers.find(',', begin);
            const auto end = comma == std::string::npos ? numbers.size() : comma;
            uint64_t uid = 0;
            if (!parse_u64(numbers.substr(begin, end - begin), uid) || uid > std::numeric_limits<int32_t>::max())
                return invalid("invalid_uid");
            const auto user = static_cast<uint32_t>(uid) / USER_RANGE;
            const auto inserted = unique.emplace(std::make_pair(package, user), static_cast<uint32_t>(uid));
            if (!inserted.second && inserted.first->second != uid) return invalid("conflicting_package_uid");
            if (unique.size() > MAX_INSTANCES) return invalid("package_instance_limit");
            begin = end == numbers.size() ? end : end + 1;
        }
    }
    if (unique.empty() && missing.empty()) return invalid("empty_package_catalog");
    Catalog result;
    Sha256 hash;
    for (const auto &entry : unique) {
        if (missing.count(entry.first.first)) return invalid("ambiguous_missing_uid");
        result.instances.push_back({entry.first.first, entry.first.second, entry.second});
        const auto canonical = entry.first.first + " " + std::to_string(entry.second) + "\n";
        hash.update(canonical.data(), canonical.size());
    }
    for (const auto &package : missing) {
        result.without_uid.push_back(package);
        const auto canonical = package + " -\n";
        hash.update(canonical.data(), canonical.size());
    }
    result.revision = hash.finish(); result.known = true;
    return result;
}

Catalog query(const Runner &runner) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(4000);
    size_t remaining = MAX_BYTES;
    const std::vector<std::vector<std::string>> commands {
        {"cmd", "package", "list", "packages", "-U", "--user", "all"},
        {"pm", "list", "packages", "-U", "--user", "all"}};
    for (const auto &command : commands) {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (milliseconds <= 0 || !remaining) break;
        const auto timeout = static_cast<unsigned>(std::min<int64_t>(2000, milliseconds));
        auto response = runner ? runner(command, timeout, remaining) : run_process(command, timeout, remaining);
        if (response.output.size() > remaining || response.diagnostic.size() > remaining - response.output.size()) break;
        remaining -= response.output.size() + response.diagnostic.size();
        if (response.ok()) {
            if (response.diagnostic.find_first_not_of(" \t\r\n") != std::string::npos) {
                Catalog failure; failure.error = "package_query_diagnostic"; return failure;
            }
            return parse(response.output);
        }
        // Timed-out, overflowing or unconfirmed children must not be followed
        // by another Binder query. A well-formed failed command may use pm.
        if (response.cancelled || response.timed_out || response.overflow || !response.reaped || !response.group_gone) break;
    }
    Catalog failure; failure.error = "package_query_unavailable_or_incomplete"; return failure;
}

Resolution resolve_primary(const Catalog &catalog, const std::vector<std::string> &packages) {
    Resolution result;
    std::set<std::string> selected(packages.begin(), packages.end());
    Sha256 hash;
    if (!selected.empty() && !catalog.known) { result.error = catalog.error; return result; }
    std::set<uint32_t> uids;
    for (const auto &package : selected) {
        if (!valid_package_name(package) || std::binary_search(catalog.without_uid.begin(), catalog.without_uid.end(), package)) {
            Resolution failure; failure.error = "package_identity_unavailable"; return failure;
        }
        const auto found = std::lower_bound(catalog.instances.begin(), catalog.instances.end(), package,
            [](const Instance &instance, const std::string &name) { return instance.package < name; });
        if (found == catalog.instances.end() || found->package != package || found->user != 0) {
            result.uninstalled.push_back(package);
            const auto row = package + " -\n"; hash.update(row.data(), row.size()); continue;
        }
        if (!found->uid) { Resolution failure; failure.error = "root_application_identity_unsupported"; return failure; }
        result.mappings.push_back({package, found->uid}); uids.insert(found->uid);
        const auto row = package + " " + std::to_string(found->uid) + "\n"; hash.update(row.data(), row.size());
    }
    for (const auto &instance : catalog.instances) {
        if (instance.user != 0 && selected.count(instance.package)) ++result.other_user_instances;
        if (instance.user == 0 && uids.count(instance.uid) && !selected.count(instance.package))
            result.shared_packages.push_back(instance.package);
    }
    result.revision = hash.finish(); result.known = true; return result;
}
} // namespace app_catalog
