#include "edit_config.h"
#include "cli_utils.h"
#include "exclusive_file_lock.h"
#include "rules.h"
#include "sha256.h"
#include "source_config.h"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace edit_config {
namespace {
std::string filename(const std::string &target) {
    if (target == "custom") return "custom_rules.txt";
    if (target == "allow") return "app_whitelist.txt";
    if (target == "intercept") return "app_intercept.txt";
    return "";
}
Result failed(const std::string &status, const std::string &message) {
    Result out; out.status = status; out.message = message; return out;
}
bool utf8(const std::string &text) {
    for (size_t at = 0; at < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[at++]);
        if (c < 128) { if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127) return false; continue; }
        unsigned count = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!count || count > text.size() - at) return false;
        uint32_t value = c & (count == 1 ? 31u : count == 2 ? 15u : 7u);
        for (unsigned i = 0; i < count; ++i) {
            const unsigned char next = static_cast<unsigned char>(text[at++]);
            if ((next & 0xc0) != 0x80) return false;
            value = value * 64 + (next & 63);
        }
        if (value < (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
std::string digest(const std::string &content) {
    Sha256 hash; hash.update(content.data(), content.size()); return hash.finish();
}
bool revision_valid(const std::string &revision) {
    return revision == "missing" || (revision.size() == 64 &&
        revision.find_first_not_of("0123456789abcdef") == std::string::npos);
}
bool trusted_directory(const std::string &root) {
    struct stat info {};
    return lstat(root.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
           info.st_uid == geteuid() && !(info.st_mode & (S_IWGRP | S_IWOTH));
}
std::string validate(const std::string &target, const std::string &content) {
    if (content.size() > MAX_BYTES) return "配置超过 1 MiB 上限";
    if (!utf8(content)) return "配置不是有效 UTF-8 文本或含非法控制字符";
    std::istringstream input(content);
    std::string line; size_t lines = 0;
    while (std::getline(input, line)) {
        if (++lines > MAX_LINES) return "配置超过 20000 行上限";
        if (line.size() > 1024) return "配置单行超过 1024 字节";
        const size_t begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos) continue;
        line = line.substr(begin, line.find_last_not_of(" \t\r") - begin + 1);
        if (line[0] == '#' || (target == "custom" && (line[0] == '!' || line[0] == '['))) continue;
        ParsedDomainRule parsed;
        if (target == "custom" ? !parse_adguard_rule(line, parsed) : !valid_package_name(line))
            return "第 " + std::to_string(lines) + " 行不是支持的" + (target == "custom" ? "域名规则" : "应用包名");
    }
    return "";
}
}

std::string validate_content(const std::string &target, const std::string &text) {
    return filename(target).empty() ? "非法配置目标" : validate(target, text);
}

Result read(const std::string &root, const std::string &target) {
    const std::string name = filename(target);
    if (name.empty()) return failed("invalid_target", "只允许编辑规则或两种应用名单");
    if (!trusted_directory(root))
        return failed("read_error", "配置目录不存在、归属不符或不是实际目录，请先初始化模块");
    const std::string path = root + "/" + name;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) { auto result = failed("missing", "配置文件不存在，请先初始化或恢复配置"); result.revision = "missing"; return result; }
        return failed("read_error", "配置读取失败，请检查权限，原文件未修改");
    }
    struct stat info {};
    bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
              static_cast<uint64_t>(info.st_size) <= MAX_BYTES;
    std::string content;
    char buffer[8192];
    while (ok) {
        ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (static_cast<size_t>(n) > MAX_BYTES - content.size()) { ok = false; break; }
        content.append(buffer, static_cast<size_t>(n));
    }
    if (close(fd) != 0) ok = false;
    if (!ok) return failed("read_error", "配置不可完整读取、不是普通文件或超过 1 MiB；禁止用空内容代替");
    if (!utf8(content)) return failed("encoding_error", "配置编码无效，禁止用替换字符覆盖原文件");
    Result result; result.status = "ok"; result.content = std::move(content);
    result.revision = digest(result.content); return result;
}

Result save(const std::string &root, const std::string &target,
            const std::string &revision, const std::string &text, const Commit &commit) {
    const std::string name = filename(target);
    if (name.empty()) return failed("invalid_target", "非法配置目标");
    if (!revision_valid(revision)) return failed("invalid_revision", "必须先成功读取配置后再保存");
    const auto error = validate(target, text);
    if (!error.empty()) return failed("invalid_content", error);
    if (!trusted_directory(root)) return failed("read_error", "配置目录不可信，禁止写入");
    ExclusiveFileLock lock((root + "/.editable.lock").c_str());
    if (!lock.ok()) return failed("busy", "其他窗口正在保存配置，请稍后重试");
    auto previous = read(root, target);
    if (previous.status != "ok" && previous.status != "missing") return previous;
    if (previous.revision != revision) return failed("conflict", "配置已被其他窗口修改，请重新加载并合并后再保存");
    if (previous.status == "ok" && previous.content == text) return previous;
    const std::string path = root + "/" + name;
    const auto written = commit ? commit(path.c_str(), text) : write_file_durable_atomic(path.c_str(), text);
    if (!written.committed) return failed("write_error", "配置写入失败，原文件未替换");
    Result result; result.committed = true; result.revision = digest(text);
    result.status = written.durable ? "ok" : "durability_unknown";
    result.message = written.durable ? "" : "配置已替换，但持久化未确认；请重新读取核对，不要盲目重复保存";
    return result;
}

int command(const std::string &root, bool saving, const std::string &target, const std::string &revision, const Commit &commit) {
    Result result;
    if (saving) {
        std::string text; char buffer[8192]; size_t count = 0;
        while ((count = fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
            if (count > MAX_BYTES - text.size()) { result = failed("invalid_content", "配置超过 1 MiB 上限"); break; }
            text.append(buffer, count);
        }
        if (result.message.empty()) result = ferror(stdin) ? failed("read_error", "无法完整读取保存内容") : save(root, target, revision, text, commit);
    } else result = read(root, target);
    printf("{\"status\":%s,\"target\":%s,\"revision\":%s,\"content\":%s,\"committed\":%s,\"message\":%s}\n",
        source_json_string(result.status).c_str(), source_json_string(target).c_str(),
        source_json_string(result.revision).c_str(), source_json_string(result.content).c_str(),
        result.committed ? "true" : "false", source_json_string(result.message).c_str());
    return result.status == "ok" ? 0 : result.status == "conflict" || result.status == "busy" ? 2 : 1;
}
}
