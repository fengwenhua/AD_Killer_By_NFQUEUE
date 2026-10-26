// ============================================================================
// AD_Killer_By_NFQUEUE - 日志与运行时状态管理
//
// 职责:
//   - log_line:      进程独立的有界日志行,格式 "[HH:MM:SS] [标签] 消息"
//   - runtime.state: 运行时状态文件(WebUI 轮询等待启停完成)
//                    状态机:starting → running / error;stopping → stopped
//   - dns_query.log: DNS 查询日志,格式 "[HH:MM:SS] domain -> verdict"
//                    (verdict 为四态:PASS(whitelist)/BLOCK(suffix)/
//                     BLOCK(exact)/PASS(default))
//   - state.conf:    键值配置读写(保留其它行)
//   - daemon.pid:    守护进程 pid 文件(cmdline 校验防错杀)
//   - gzip 压缩:     规则文件落盘用
// ============================================================================
#include "logging.h"
#include "common.h"
#include "exclusive_file_lock.h"
#include "bounded_log.h"
#include "cli_utils.h"
#include "process_identity.h"

#include <cstdio>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdarg>
#include <ctime>
#include <cstring>
#include <chrono>
#include <fstream>
#include <sstream>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

namespace {

std::mutex log_mutex;
std::string process_log_path;
size_t process_log_max_bytes = 0;
int process_log_backups = 0;
AsyncLog dns_log;

}  // namespace

void configure_process_log(const char *path, size_t max_bytes, int backups) {
    std::lock_guard<std::mutex> lock(log_mutex);
    process_log_path = path ? path : "";
    process_log_max_bytes = max_bytes;
    process_log_backups = backups;
}

// ---------------------------------------------------------------------------
// 引擎日志行:配置了独立文件时有界追加,否则输出 stdout。
void log_line(const char *tag, const char *fmt, ...) {
    char timebuf[32];
    struct tm tmv;
    time_t now = time(nullptr);
    localtime_r(&now, &tmv);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tmv);
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char line[2112];
    int length = snprintf(line, sizeof(line), "[%s] [%s] %s\n", timebuf, tag, msg);
    if (length < 0) return;
    size_t bytes = static_cast<size_t>(length);
    if (bytes >= sizeof(line)) bytes = sizeof(line) - 1;
    std::lock_guard<std::mutex> lock(log_mutex);
    if (!process_log_path.empty()) {
        append_bounded_log(process_log_path, std::string(line, bytes),
                           process_log_max_bytes, process_log_backups, process_log_backups == 0);
    } else {
        fwrite(line, 1, bytes, stdout);
        fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
// 写运行时状态(WebUI 的 waitEngine 轮询 cat 此文件)
bool write_runtime_state(const char *state) {
    if (!state) return false;
    ExclusiveFileLock lock(STATE_LOCK_FILE, true, 1000);
    return lock.ok() && write_file_atomic(FILE_RUNTIME_STATE, state);
}

// 读取运行时状态(空串表示文件不存在)
std::string read_runtime_state() {
    std::string out;
    if (!read_file(FILE_RUNTIME_STATE, out)) return "";
    // 去掉首尾空白
    size_t b = out.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = out.find_last_not_of(" \t\r\n");
    return out.substr(b, e - b + 1);
}

// ---------------------------------------------------------------------------
// DNS 查询日志(dev_mode 开启时由 DNS 队列线程写入)
bool start_dns_log() {
    return dns_log.start([](const char *data, size_t bytes) {
        return append_bounded_log(FILE_DNS_LOG, std::string(data, bytes), DNS_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
    });
}
void stop_dns_log() { dns_log.stop(); }
AsyncLogStats dns_log_stats() { return dns_log.stats(); }
void log_dns_query(const std::string &domain, const char *verdict) {
    if (domain.empty()) return;
    if (domain.size() > 480) return;   // 防御:超长行丢弃
    char timebuf[32];
    struct tm tmv;
    time_t now = time(nullptr);
    localtime_r(&now, &tmv);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tmv);
    char line[640];
    int length = snprintf(line, sizeof(line), "[%s] %s -> %s\n",
                          timebuf, domain.c_str(), verdict);
    if (length <= 0) return;
    size_t bytes = static_cast<size_t>(length);
    if (bytes >= sizeof(line)) bytes = sizeof(line) - 1;
    dns_log.enqueue(line, bytes);
}

// ---------------------------------------------------------------------------
// 文件读写工具
bool read_file(const char *path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// 文本原子写(临时文件 + rename,避免半写状态被读到)
bool write_file_atomic(const char *path, const std::string &content) {
    return write_file_raw_atomic(path, content);
}

// 二进制原子写(规则 gzip 落盘用)
static bool write_atomic_snapshot(const char *path, const std::string &data, bool sync) {
    if (!path || !*path) return false;
    std::string tmp = std::string(path) + ".tmp.XXXXXX";
    int fd = mkstemp(tmp.data());
    if (fd < 0) return false;
    bool ok = fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
    size_t offset = 0;
    while (ok && offset < data.size()) {
        ssize_t written = write(fd, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) ok = false;
        else offset += static_cast<size_t>(written);
    }
    if (ok && sync) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok) ok = rename(tmp.c_str(), path) == 0;
    if (!ok) unlink(tmp.c_str());
    return ok;
}

bool write_file_raw_atomic(const char *path, const std::string &data) { return write_atomic_snapshot(path, data, true); }
bool write_file_volatile_atomic(const char *path, const std::string &data) { return write_atomic_snapshot(path, data, false); }

AtomicWriteResult write_file_durable_atomic(const char *path, const std::string &data) {
    AtomicWriteResult result;
    if (!write_file_raw_atomic(path, data)) return result;
    result.committed = true;
    const std::string name(path);
    const size_t slash = name.rfind('/');
    const std::string parent = slash == std::string::npos ? "." : slash == 0 ? "/" : name.substr(0, slash);
    // The trusted caller chooses the destination. Normal directory aliases
    // (notably /tmp -> /private/tmp on macOS) must remain usable here.
    int fd = open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (fd < 0) return result;
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    result.durable = ok;
    return result;
}

// ---------------------------------------------------------------------------
// state.conf 键值读取(忽略 # 注释;返回空串表示键不存在)
std::string read_conf_value(const char *path, const char *key) {
    std::string content;
    if (!read_file(path, content)) return "";
    std::istringstream ss(content);
    std::string line;
    std::string prefix = std::string(key) + "=";
    while (std::getline(ss, line)) {
        if (!line.empty() && line[0] == '#') continue;
        if (line.compare(0, prefix.size(), prefix) == 0) {
            std::string v = line.substr(prefix.size());
            // trim
            size_t b = v.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return "";
            size_t e = v.find_last_not_of(" \t\r\n");
            return v.substr(b, e - b + 1);
        }
    }
    return "";
}

// state.conf 键值写入(存在则替换,不存在则追加;保留其它行)
bool write_conf_value_with_lock(const char *path, const char *key,
                                const char *value, const char *lock_path,
                                const std::function<bool()> &before_commit) {
    if (!path || !key || !*key || !value || !lock_path) return false;
    ExclusiveFileLock lock(lock_path, true, 1000);
    if (!lock.ok()) return false;
    std::string content;
    if (read_regular_bounded(path, content, 1024u * 1024u) == BoundedRead::ERROR) return false;
    std::istringstream ss(content);
    std::ostringstream out;
    std::string line;
    std::string prefix = std::string(key) + "=";
    bool found = false;
    while (std::getline(ss, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            out << key << "=" << value << "\n";
            found = true;
        } else {
            out << line << "\n";
        }
    }
    if (!found) out << key << "=" << value << "\n";
    if (before_commit && !before_commit()) return false;
    const auto saved = write_file_durable_atomic(path, out.str());
    return saved.committed && saved.durable;
}

bool write_conf_value(const char *path, const char *key, const char *value) {
    return write_conf_value_with_lock(path, key, value, STATE_LOCK_FILE);
}

// ---------------------------------------------------------------------------
// 时间工具
std::string now_string() {
    char buf[64];
    struct tm tmv;
    time_t now = time(nullptr);
    localtime_r(&now, &tmv);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

int64_t now_epoch() {
    return (int64_t)time(nullptr);
}

// ---------------------------------------------------------------------------
// daemon.pid 管理
// 写 pid(供 --stop 快速定位守护进程)
bool write_daemon_pid(int pid) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", pid);
    return write_file_atomic(DAEMON_PID_FILE, buf);
}

// 同时校验精确 argv 和可执行文件名，避免把 shell/其它进程参数中的子串
// "AD_Killer_By_NFQUEUE --daemon" 误认为守护进程。
bool is_daemon_pid(int pid) {
    if (pid <= 0) return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    std::string cl;
    if (!read_file(path, cl)) return false;
    const size_t first_end = cl.find('\0');
    if (first_end == std::string::npos) return false;
    const size_t second_end = cl.find('\0', first_end + 1);
    if (second_end == std::string::npos ||
        cl.substr(first_end + 1, second_end - first_end - 1) != "--daemon")
        return false;
    std::string program = cl.substr(0, first_end);
    const size_t slash = program.rfind('/');
    if (slash != std::string::npos) program.erase(0, slash + 1);
    if (program != MODULE_ID && program != DAEMON_ARGV0) return false;

    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    char executable[4096];
    const ssize_t length = readlink(path, executable, sizeof(executable));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(executable)) return false;
    std::string name(executable, static_cast<size_t>(length));
    const size_t executable_slash = name.rfind('/');
    if (executable_slash != std::string::npos) name.erase(0, executable_slash + 1);
    return name == MODULE_ID || name == MODULE_ID " (deleted)";
}

// 读 pid 文件并校验进程身份。
int read_daemon_pid() {
    std::string s;
    if (!read_file(DAEMON_PID_FILE, s)) return -1;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ' || s.back() == '\t')) s.pop_back();
    int pid = -1;
    if (!parse_integer_arg(s.c_str(), pid)) return -1;
    return is_daemon_pid(pid) ? pid : -1;
}

void clear_daemon_pid_if(int expected_pid) {
    std::string s;
    if (!read_file(DAEMON_PID_FILE, s)) return;
    char *end = nullptr;
    long pid = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || pid != expected_pid) return;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end == '\0') unlink(DAEMON_PID_FILE);
}

// ---------------------------------------------------------------------------
// gzip 压缩字符串(zlib 内存压缩,gzip 封装)
// windowBits = 15 + 16:gzip 封装(带 1f 8b 魔数头)
bool gz_compress_string(const std::string &in, std::string &out) {
    return gz_compress_parts({in}, out);
}

bool gz_compress_parts(std::initializer_list<std::string_view> parts, std::string &out) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, 9, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    out.clear();
    char buf[65536];
    for (const auto &part : parts) {
        for (size_t offset = 0; offset < part.size();) {
            const auto bytes = std::min<size_t>(sizeof(buf), part.size() - offset);
            zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(part.data() + offset));
            zs.avail_in = static_cast<uInt>(bytes);
            while (zs.avail_in) {
                zs.next_out = reinterpret_cast<Bytef *>(buf);
                zs.avail_out = sizeof(buf);
                if (deflate(&zs, Z_NO_FLUSH) != Z_OK) {
                    deflateEnd(&zs); out.clear(); return false;
                }
                out.append(buf, sizeof(buf) - zs.avail_out);
            }
            offset += bytes;
        }
    }
    int ret;
    do {
        zs.next_out = (Bytef *)buf;
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, Z_FINISH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret == Z_OK);
    deflateEnd(&zs);
    if (ret != Z_STREAM_END) { out.clear(); return false; }
    return true;
}
