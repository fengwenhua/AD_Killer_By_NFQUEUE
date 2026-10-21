#include "rule_bundle.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <sys/stat.h>

namespace {

void set_error(std::string *error, const std::string &message) {
    if (error) *error = message;
}

bool valid_generation(const std::string &generation) {
    if (generation.empty() || generation.size() > 128 || generation[0] == '.')
        return false;
    for (unsigned char ch : generation) {
        bool ascii_alnum = (ch >= 'a' && ch <= 'z') ||
                           (ch >= 'A' && ch <= 'Z') ||
                           (ch >= '0' && ch <= '9');
        if (!ascii_alnum && ch != '-' && ch != '_') return false;
    }
    return true;
}

bool regular_file(const std::string &path) {
    struct stat info;
    return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool ensure_directory(const std::string &path, mode_t mode) {
    if (mkdir(path.c_str(), mode) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat info;
    return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool write_all(int fd, const std::string &data) {
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t written = write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool write_new_file(const std::string &path, const std::string &data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0) return false;
    bool ok = write_all(fd, data) && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (!ok) unlink(path.c_str());
    return ok;
}

void remove_bundle_dir(const std::string &dir) {
    if (dir.empty()) return;
    unlink((dir + "/hosts_rules.gz").c_str());
    unlink((dir + "/adguard_rules.gz").c_str());
    rmdir(dir.c_str());
}

void sync_directory(const std::string &path) {
#ifdef O_DIRECTORY
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
#else
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

std::string parent_directory(const std::string &path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

}  // namespace

RuleBundlePaths resolve_rule_bundle(const std::string &data_dir,
                                    const std::string &manifest_path,
                                    const std::string &legacy_hosts,
                                    const std::string &legacy_adguard) {
    RuleBundlePaths paths;
    paths.hosts_path = legacy_hosts;
    paths.adguard_path = legacy_adguard;

    std::ifstream manifest(manifest_path, std::ios::binary);
    std::string generation;
    if (!manifest.is_open() || !std::getline(manifest, generation)) return paths;
    while (!generation.empty() &&
           (generation.back() == '\r' || generation.back() == ' ' ||
            generation.back() == '\t'))
        generation.pop_back();
    if (!valid_generation(generation)) return paths;

    std::string base = data_dir + "/rulesets/" + generation;
    std::string hosts = base + "/hosts_rules.gz";
    std::string adguard = base + "/adguard_rules.gz";
    if (!regular_file(hosts) || !regular_file(adguard)) return paths;
    paths.generation = generation;
    paths.hosts_path = hosts;
    paths.adguard_path = adguard;
    paths.from_manifest = true;
    return paths;
}

bool stage_rule_bundle(const std::string &data_dir,
                       const std::string &generation,
                       const std::string &hosts_gzip,
                       const std::string &adguard_gzip,
                       StagedRuleBundle &staged,
                       std::string *error) {
    staged = StagedRuleBundle{};
    if (!valid_generation(generation)) {
        set_error(error, "generation 名称非法");
        return false;
    }
    std::string rulesets = data_dir + "/rulesets";
    if (!ensure_directory(data_dir, 0755) || !ensure_directory(rulesets, 0700)) {
        set_error(error, "无法创建 rulesets 目录");
        return false;
    }

    staged.generation = generation;
    staged.staging_dir = rulesets + "/.staging-" + generation;
    staged.final_dir = rulesets + "/" + generation;
    staged.hosts_path = staged.staging_dir + "/hosts_rules.gz";
    staged.adguard_path = staged.staging_dir + "/adguard_rules.gz";
    remove_bundle_dir(staged.staging_dir);
    if (mkdir(staged.staging_dir.c_str(), 0700) != 0) {
        set_error(error, "无法创建规则 staging 目录");
        staged = StagedRuleBundle{};
        return false;
    }
    if (!write_new_file(staged.hosts_path, hosts_gzip) ||
        !write_new_file(staged.adguard_path, adguard_gzip)) {
        set_error(error, "无法完整写入 staging 规则");
        discard_staged_rule_bundle(staged);
        return false;
    }
    sync_directory(staged.staging_dir);
    if (error) error->clear();
    return true;
}

bool commit_rule_bundle(const std::string &manifest_path,
                        const StagedRuleBundle &staged,
                        std::string *error) {
    if (!valid_generation(staged.generation) || staged.staging_dir.empty() ||
        staged.final_dir.empty()) {
        set_error(error, "staging 状态非法");
        return false;
    }
    if (rename(staged.staging_dir.c_str(), staged.final_dir.c_str()) != 0) {
        set_error(error, "无法固化规则 generation");
        return false;
    }
    sync_directory(parent_directory(staged.final_dir));

    std::string temp_manifest = manifest_path + ".tmp." +
                                std::to_string(static_cast<long>(getpid()));
    unlink(temp_manifest.c_str());
    if (!write_new_file(temp_manifest, staged.generation + "\n") ||
        rename(temp_manifest.c_str(), manifest_path.c_str()) != 0) {
        unlink(temp_manifest.c_str());
        remove_bundle_dir(staged.final_dir);
        set_error(error, "无法原子切换规则指针");
        return false;
    }
    sync_directory(parent_directory(manifest_path));
    if (error) error->clear();
    return true;
}

void discard_staged_rule_bundle(const StagedRuleBundle &staged) {
    remove_bundle_dir(staged.staging_dir);
}
