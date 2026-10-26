#pragma once
#include "module_version.h"
#include <functional>
#include <string>

namespace module_update {
using Fetch = std::function<std::string(const std::string &, const std::string &)>;
using Prefix = std::function<std::string(const std::string &, std::string &, bool &, size_t)>;
std::string package_name(const std::string &version);
std::string asset_url(const std::string &version);
std::string verify_package(const std::string &path, const std::string &digest);
std::string download(const std::string &version, const std::string &directory,
                     const Fetch &fetch, const Prefix &prefix, bool preview = false);
std::string ready_version(const std::string &directory);
std::string ready_channel(const std::string &directory);
int release_info(bool preview);
int release_notes();
int worker(const std::string &version, bool install, bool preview = false);
int status();
}
