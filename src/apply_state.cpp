#include "apply_state.h"
#include "exclusive_file_lock.h"
#include "logging.h"
#include "sha256.h"
#include <atomic>
#include <chrono>
#include <limits>
#include <sstream>

namespace apply_state {
namespace {
constexpr size_t MAX_BYTES = 1024u * 1024u, MAX_UIDS = 20000;
bool trusted(const std::string &root) {
    struct stat info {};
    return lstat(root.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid() &&
        !(info.st_mode & (S_IWGRP | S_IWOTH));
}
std::string token(const std::string &text) { return text.empty() ? "-" : text; }
bool revision(std::istream &in, std::string &value) {
    if (!(in >> value)) return false;
    if (value == "-") value.clear();
    return value.empty() || valid_revision(value);
}
bool number(std::istream &in, uint64_t &value, uint64_t maximum = UINT64_MAX) {
    std::string word;
    return bool(in >> word) && parse_u64(word, value) && value <= maximum;
}
bool uids(std::istream &in, std::vector<uid_t> &values) {
    uint64_t count = 0, uid = 0;
    if (!number(in, count, MAX_UIDS)) return false;
    for (uint64_t i = 0; i < count; ++i) {
        if (!number(in, uid, std::numeric_limits<uid_t>::max()) || !uid) return false;
        values.push_back(static_cast<uid_t>(uid));
    }
    return true;
}
std::string hex(const std::string &text) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : text.substr(0, 2048)) { out += digits[c >> 4]; out += digits[c & 15]; }
    return token(out);
}
bool unhex(std::string text, std::string &out) {
    if (text == "-") return true;
    const std::string digits = "0123456789abcdef";
    if (text.size() > 4096 || text.size() % 2 || text.find_first_not_of(digits) != std::string::npos) return false;
    for (size_t i = 0; i < text.size(); i += 2)
        out += static_cast<char>(digits.find(text[i]) * 16 + digits.find(text[i + 1]));
    return true;
}
bool parse_record(const std::string &text, Record &record) {
    record = {};
    std::istringstream in(text);
    std::string version, submitted, generation, message, extra;
    uint64_t pid = 0, mode = 0, scope = 0, quic = 0;
    if (!(in >> version) || version != "ADK_APPLY_1" || !revision(in, record.desired) ||
        !(in >> submitted) || (submitted != "0" && submitted != "1") ||
        !revision(in, record.applied) || !revision(in, record.attempt) || !revision(in, record.last_revision) ||
        !(in >> record.owner.boot) || !number(in, pid, INT32_MAX) || !number(in, record.owner.started) ||
        !number(in, mode, 2) || mode < 1 || !number(in, scope, 1) || !number(in, quic, 1) ||
        !uids(in, record.actual.config.bypass_uids) || !uids(in, record.actual.config.intercept_uids) ||
        !(in >> generation) || generation.size() > 128 ||
        generation.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
        !number(in, record.actual.black, 10000000) || !number(in, record.actual.white, 10000000) ||
        !number(in, record.actual.updated_at, INT64_MAX) || !(in >> message) || !unhex(message, record.error) || (in >> extra)) return false;
    record.owner.pid = static_cast<int>(pid);
    if (record.owner.boot == "-") record.owner.boot.clear();
    if ((pid || record.owner.started || !record.owner.boot.empty()) && !record.owner.valid()) return false;
    if ((!record.applied.empty() || !record.attempt.empty()) && !record.owner.valid()) return false;
    if (record.desired.empty() || (!record.applied.empty() && record.actual.updated_at == 0)) return false;
    record.actual.config.mode = static_cast<int>(mode);
    record.actual.config.scope = static_cast<int>(scope);
    record.actual.config.block_quic = quic == 1;
    record.actual.generation = generation == "-" ? "" : generation;
    record.submitted = submitted == "1";
    record.valid = true;
    return true;
}
bool load(const std::string &root, Record &record) {
    record = {};
    if (!trusted(root)) return false;
    std::string text;
    const auto result = read_regular_bounded(root + "/apply.state", text, MAX_BYTES);
    if (result == BoundedRead::MISSING) return true;
    return result == BoundedRead::OK && parse_record(text, record);
}
bool save(const std::string &root, Record &record, std::string &error) {
    if (record.actual.config.bypass_uids.size() > MAX_UIDS || record.actual.config.intercept_uids.size() > MAX_UIDS) {
        error = "生效配置 UID 数量超过上限"; return false;
    }
    std::ostringstream out;
    out << "ADK_APPLY_1\n" << token(record.desired) << ' ' << record.submitted << '\n'
        << token(record.applied) << ' ' << token(record.attempt) << ' ' << token(record.last_revision) << '\n'
        << token(record.owner.boot) << ' ' << record.owner.pid << ' ' << record.owner.started << '\n'
        << record.actual.config.mode << ' ' << record.actual.config.scope << ' ' << record.actual.config.block_quic << '\n';
    for (const auto *values : {&record.actual.config.bypass_uids, &record.actual.config.intercept_uids}) {
        out << values->size(); for (const auto uid : *values) out << ' ' << uid; out << '\n';
    }
    out << token(record.actual.generation) << ' ' << record.actual.black << ' ' << record.actual.white << ' '
        << record.actual.updated_at << '\n' << hex(record.error) << '\n';
    const std::string text = out.str(); Record checked;
    if (text.size() > MAX_BYTES || !parse_record(text, checked)) { error = "候选生效回执含非法字段，未发布"; return false; }
    const auto written = write_file_durable_atomic((root + "/apply.state").c_str(), text);
    if (!written.committed || !written.durable) { error = "生效回执持久化未能确认，请重新查询，不要重复启动"; return false; }
    record.valid = true; return true;
}
std::string nonce(const Record &record) {
    static std::atomic<uint64_t> sequence{0};
    const std::string seed = record.desired + record.applied + std::to_string(getpid()) + ":" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ":" + std::to_string(++sequence);
    Sha256 hash; hash.update(seed.data(), seed.size()); return hash.finish();
}
bool writable(const std::string &root, Record &record, std::string &error) {
    if (!load(root, record)) { error = "生效回执不可读或格式错误，未修改配置"; return false; }
    return true;
}
}

bool valid_revision(const std::string &value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
Record read(const std::string &root) { Record record; if (!load(root, record)) return {}; return record; }
View view(const Record &record, const ProcessIdentity &live, bool running, const std::string &expected) {
    View result; result.record = record;
    if (!record.valid) { if (!running && expected.empty()) result.phase = "deferred"; return result; }
    if (!expected.empty() && expected != record.desired) { result.phase = "superseded"; return result; }
    if (!running) { result.phase = "deferred"; return result; }
    if (!live.valid()) return result;
    result.owner_verified = live.valid() && record.owner == live;
    result.actual_verified = result.owner_verified && !record.applied.empty();
    if (!record.submitted) result.phase = "saved";
    else if (!result.owner_verified) result.phase = "pending";
    else if (record.attempt == record.desired) result.phase = "applying";
    else if (record.last_revision == record.desired && !record.error.empty()) result.phase = "failed";
    else if (record.applied == record.desired) result.phase = "applied";
    else result.phase = "pending";
    return result;
}
bool dirty(const std::string &root, std::string &error) {
    if (!trusted(root)) { error = "生效回执目录不可信"; return false; }
    ExclusiveFileLock lock((root + "/.apply.lock").c_str(), true, 1000);
    Record record;
    if (!lock.ok()) { error = "生效回执正在更新，请稍后重试"; return false; }
    if (!writable(root, record, error)) return false;
    record.desired = nonce(record); record.submitted = false;
    return save(root, record, error);
}
bool queue(const std::string &root, std::string &revision, std::string &error) {
    if (!trusted(root)) { error = "生效回执目录不可信"; return false; }
    ExclusiveFileLock lock((root + "/.apply.lock").c_str(), true, 1000);
    Record record;
    if (!lock.ok()) { error = "生效回执正在更新，请稍后重试"; return false; }
    if (!writable(root, record, error)) return false;
    record.desired = nonce(record); record.submitted = true;
    if (!save(root, record, error)) return false;
    revision = record.desired; return true;
}
bool capture(const std::string &root, bool startup, const ProcessIdentity &owner,
             std::string &revision, std::string &error) {
    if (!owner.valid()) { error = "无法验证守护启动身份，拒绝接管配置"; return false; }
    if (!trusted(root)) { error = "生效回执目录不可信"; return false; }
    ExclusiveFileLock lock((root + "/.apply.lock").c_str(), true, 1000);
    Record record;
    if (!lock.ok()) { error = "无法取得生效回执锁"; return false; }
    if (!writable(root, record, error)) return false;
    if (record.valid && !record.submitted && !startup) { error = "配置已保存但尚未提交应用"; return false; }
    if (!record.valid || startup || record.desired == record.applied || record.desired == record.last_revision)
        record.desired = nonce(record);
    record.submitted = true;
    if (!(record.owner == owner)) {
        record.applied.clear(); record.actual = {}; record.last_revision.clear(); record.error.clear();
    }
    record.owner = owner; record.attempt = record.desired;
    record.error.clear();
    if (!save(root, record, error)) return false;
    revision = record.desired; return true;
}
bool current(const std::string &root, const ProcessIdentity &owner, const std::string &revision) {
    const auto record = read(root);
    return record.valid && record.submitted && record.owner == owner && record.desired == revision && record.attempt == revision;
}
bool finish(const std::string &root, const ProcessIdentity &owner, const std::string &revision,
            const Applied *actual, const std::string &failure, std::string &error) {
    if (!trusted(root)) { error = "生效回执目录不可信"; return false; }
    ExclusiveFileLock lock((root + "/.apply.lock").c_str(), true, 1000);
    Record record;
    if (!lock.ok()) { error = "无法取得生效回执锁"; return false; }
    if (!writable(root, record, error)) return false;
    if (!(record.owner == owner) || record.attempt != revision || !valid_revision(revision)) {
        error = "生效回执已被新实例替代"; return false;
    }
    record.attempt.clear(); record.last_revision = revision; record.error = failure.substr(0, 2048);
    if (actual) { record.actual = *actual; record.applied = revision; record.error.clear(); }
    else if (record.error.empty()) record.error = "未能应用候选配置，保留旧策略";
    return save(root, record, error);
}
bool pending(const Record &record, const ProcessIdentity &owner) {
    return record.valid && record.submitted && !record.desired.empty() &&
        (record.owner == owner) && record.desired != record.applied && record.desired != record.last_revision && record.attempt.empty();
}
}
