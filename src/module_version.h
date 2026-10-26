#pragma once
#include <array>
#include <string>

// Shared by update discovery and install admission.
namespace module_update {
namespace version_detail {
inline std::array<unsigned, 3> version_parts(const std::string &text) {
    std::array<unsigned, 3> result{};
    size_t index = 0;
    for (char ch : text) {
        if (ch == '.') ++index;
        else result[index] = result[index]*10 + unsigned(ch-'0');
    }
    return result;
}
}
inline bool valid_version(const std::string &version) {
    if (version.empty() || version.size() > 20) return false;
    unsigned segments = 1, digits = 0;
    for (char c : version) {
        if (c == '.') { if (!digits || ++segments > 3) return false; digits=0; }
        else if (c < '0' || c > '9' || ++digits > 6) return false;
    }
    return digits && segments >= 2;
}
inline bool newer_version(const std::string &candidate, const std::string &installed) {
    return valid_version(candidate) && valid_version(installed) &&
           version_detail::version_parts(candidate) > version_detail::version_parts(installed);
}
inline bool upgrade_allowed(const std::string &candidate, const std::string &installed,
                     const std::string &installed_channel, const std::string &candidate_channel) {
    if (candidate_channel != "stable" && candidate_channel != "preview") return false;
    if (!valid_version(candidate) || !valid_version(installed)) return false;
    const auto order = version_detail::version_parts(candidate);
    const auto current = version_detail::version_parts(installed);
    return order > current || (order == current && installed_channel == "preview" && candidate_channel == "stable");
}
} // namespace module_update
