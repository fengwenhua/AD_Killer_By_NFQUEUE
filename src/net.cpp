// ============================================================================
// AD_Killer_By_NFQUEUE - 有界 HTTP(S) 下载器
//   1. 动态 libcurl:状态码、协议重定向、TLS、大小全部显式约束
//   2. 固定路径 curl/wget:fork + execv argv,URL 永不交给 shell
//   3. 所有后端写唯一临时文件,校验普通文件和大小后 rename
// ============================================================================
#include "net.h"
#include "net_policy.h"
#include "logging.h"
#include "common.h"
#include "task_context.h"
#include "owned_child.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <poll.h>
#include <algorithm>
#include <sstream>

// ---------------------------------------------------------------------------
// libcurl 动态加载(dlopen 系统库)
typedef int (*fn_curl_global_init)(long);
typedef void (*fn_curl_global_cleanup)(void);
typedef void *(*fn_curl_easy_init)(void);
typedef void (*fn_curl_easy_cleanup)(void *);
typedef int (*fn_curl_easy_setopt)(void *, int, ...);
typedef int (*fn_curl_easy_perform)(void *);
typedef int (*fn_curl_easy_getinfo)(void *, int, ...);

static fn_curl_global_init p_global_init;
static fn_curl_global_cleanup p_global_cleanup;
static fn_curl_easy_init p_easy_init;
static fn_curl_easy_cleanup p_easy_cleanup;
static fn_curl_easy_setopt p_easy_setopt;
static fn_curl_easy_perform p_easy_perform;
static fn_curl_easy_getinfo p_easy_getinfo;
static void *g_curl_lib = nullptr;
static bool g_curl_ready = false;

// curl.h 中跨 7.x/8.x 保持稳定的 ABI 数值。
enum {
    CURLOPT_WRITEDATA_ = 10001,
    CURLOPT_RANGE_ = 10007,
    CURLOPT_URL_ = 10002,
    CURLOPT_ERRORBUFFER_ = 10010,
    CURLOPT_WRITEFUNCTION_ = 20011,
    CURLOPT_TIMEOUT_ = 13,
    CURLOPT_LOW_SPEED_LIMIT_ = 19,
    CURLOPT_LOW_SPEED_TIME_ = 20,
    CURLOPT_USERAGENT_ = 10018,
    CURLOPT_FAILONERROR_ = 45,
    CURLOPT_NOPROGRESS_ = 43,
    CURLOPT_PROGRESSFUNCTION_ = 20056,
    CURLOPT_XFERINFODATA_ = 10057,
    CURLOPT_XFERINFOFUNCTION_ = 20219,
    CURLOPT_FOLLOWLOCATION_ = 52,
    CURLOPT_SSL_VERIFYPEER_ = 64,
    CURLOPT_MAXREDIRS_ = 68,
    CURLOPT_SSL_VERIFYHOST_ = 81,
    CURLOPT_CONNECTTIMEOUT_ = 78,
    CURLOPT_NOSIGNAL_ = 99,
    CURLOPT_MAXFILESIZE_LARGE_ = 30117,
    CURLOPT_PROTOCOLS_ = 181,
    CURLOPT_REDIR_PROTOCOLS_ = 182,
};

enum {
    CURLINFO_EFFECTIVE_URL_ = 0x100001,
    CURLINFO_RESPONSE_CODE_ = 0x200002,
};

constexpr long CURLPROTO_HTTP_HTTPS = (1L << 0) | (1L << 1);

static const char *CURL_LIBRARY_PATHS[] = {
#if defined(ADK_NFQ_TEST_CURL_LIBRARY)
    ADK_NFQ_TEST_CURL_LIBRARY,
#else
    "/system/lib64/libcurl.so",
    "/system/lib/libcurl.so",
    "/system_ext/lib64/libcurl.so",
    "/vendor/lib64/libcurl.so",
#endif
};

bool net_curl_init() {
    if (g_curl_ready) return true;
    for (const char *path : CURL_LIBRARY_PATHS) {
        g_curl_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (g_curl_lib) break;
    }
    if (!g_curl_lib) return false;

    p_global_init = reinterpret_cast<fn_curl_global_init>(
        dlsym(g_curl_lib, "curl_global_init"));
    p_global_cleanup = reinterpret_cast<fn_curl_global_cleanup>(
        dlsym(g_curl_lib, "curl_global_cleanup"));
    p_easy_init = reinterpret_cast<fn_curl_easy_init>(
        dlsym(g_curl_lib, "curl_easy_init"));
    p_easy_cleanup = reinterpret_cast<fn_curl_easy_cleanup>(
        dlsym(g_curl_lib, "curl_easy_cleanup"));
    p_easy_setopt = reinterpret_cast<fn_curl_easy_setopt>(
        dlsym(g_curl_lib, "curl_easy_setopt"));
    p_easy_perform = reinterpret_cast<fn_curl_easy_perform>(
        dlsym(g_curl_lib, "curl_easy_perform"));
    p_easy_getinfo = reinterpret_cast<fn_curl_easy_getinfo>(
        dlsym(g_curl_lib, "curl_easy_getinfo"));
    if (!p_global_init || !p_global_cleanup || !p_easy_init || !p_easy_cleanup ||
        !p_easy_setopt || !p_easy_perform || !p_easy_getinfo ||
        p_global_init(3 /*CURL_GLOBAL_DEFAULT*/) != 0) {
        dlclose(g_curl_lib);
        g_curl_lib = nullptr;
        return false;
    }
    g_curl_ready = true;
    return true;
}

bool net_curl_available() { return g_curl_ready || net_curl_init(); }

void net_curl_shutdown() {
    if (g_curl_ready) {
        p_global_cleanup();
        g_curl_ready = false;
    }
    if (g_curl_lib) {
        dlclose(g_curl_lib);
        g_curl_lib = nullptr;
    }
}

namespace {

// A failed termination poisons the rest of this process's downloads: never
// reuse a temp name or start another backend under an unconfirmed writer.
thread_local bool unconfirmed_download_writer = false;
struct TransferCancellation {
    bool enabled = !operation_marker_path.empty(), cancelled = false;
    std::chrono::steady_clock::time_point checked {};
    bool check() {
        if (!enabled || cancelled) return cancelled;
        const auto now = std::chrono::steady_clock::now();
        if (checked.time_since_epoch().count() && now - checked < std::chrono::milliseconds(100)) return false;
        checked = now; cancelled = operation_closing(); return cancelled;
    }
};
int cancel_transfer(void *user, int64_t, int64_t, int64_t, int64_t) {
    return static_cast<TransferCancellation *>(user)->check() ? 1 : 0;
}
int cancel_transfer_legacy(void *user, double, double, double, double) {
    return static_cast<TransferCancellation *>(user)->check() ? 1 : 0;
}
int configure_transfer_cancellation(void *easy, TransferCancellation &cancel) {
    if (!cancel.enabled) return 0;
    int code = p_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION_, cancel_transfer);
    // XFERINFO appeared in 7.32.0; keep the exact legacy callback ABI for old
    // system libraries rather than silently dropping idle cancellation.
    if (code) code = p_easy_setopt(easy, CURLOPT_PROGRESSFUNCTION_, cancel_transfer_legacy);
    if (!code) code = p_easy_setopt(easy, CURLOPT_XFERINFODATA_, &cancel);
    if (!code) code = p_easy_setopt(easy, CURLOPT_NOPROGRESS_, 0L);
    return code;
}

struct DownloadContext {
    FILE *file = nullptr;
    size_t bytes = 0;
    bool too_large = false;
    bool write_failed = false;
    TransferCancellation cancellation;
};

size_t write_download(char *data, size_t size, size_t count, void *userdata) {
    auto *context = static_cast<DownloadContext *>(userdata);
    if (context->cancellation.check()) return 0;
    if (count != 0 && size > SIZE_MAX / count) {
        context->too_large = true;
        return 0;
    }
    size_t bytes = size * count;
    if (bytes > MAX_RULE_DOWNLOAD_BYTES - context->bytes) {
        context->too_large = true;
        return 0;
    }
    size_t written = fwrite(data, 1, bytes, context->file);
    context->bytes += written;
    if (written != bytes) context->write_failed = true;
    return written;
}

std::string unique_temp_path(const std::string &path) {
    return path + ".tmp." + std::to_string(static_cast<long>(getpid()));
}

int create_temp_file(const std::string &path) {
    unlink(path.c_str());
    return open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                0600);
}

bool valid_download_file(const std::string &path) {
    struct stat info;
    return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           info.st_size > 0 &&
           static_cast<uint64_t>(info.st_size) <= MAX_RULE_DOWNLOAD_BYTES;
}

std::string download_via_libcurl(const std::string &url,
                                 const std::string &temp_path) {
    int fd = create_temp_file(temp_path);
    if (fd < 0) return "无法创建安全临时文件";
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(temp_path.c_str());
        return "无法打开临时文件流";
    }

    void *easy = p_easy_init();
    if (!easy) {
        fclose(file);
        unlink(temp_path.c_str());
        return "curl_easy_init 失败";
    }

    DownloadContext context;
    context.file = file;
    char error_buffer[256] = {0};
    int option_error = 0;
    option_error |= configure_transfer_cancellation(easy, context.cancellation);
    option_error |= p_easy_setopt(easy, CURLOPT_URL_, url.c_str());
    // CURLOPT_WRITEFUNCTION takes a function pointer, not an object pointer.
    // Passing the exact callback type matters for the variadic API ABI.
    option_error |= p_easy_setopt(easy, CURLOPT_WRITEFUNCTION_, write_download);
    option_error |= p_easy_setopt(easy, CURLOPT_WRITEDATA_, &context);
    option_error |= p_easy_setopt(easy, CURLOPT_ERRORBUFFER_, error_buffer);
    option_error |= p_easy_setopt(easy, CURLOPT_FAILONERROR_, 1L);
    option_error |= p_easy_setopt(easy, CURLOPT_FOLLOWLOCATION_, 1L);
    option_error |= p_easy_setopt(easy, CURLOPT_MAXREDIRS_,
                                  static_cast<long>(MAX_DOWNLOAD_REDIRECTS));
    option_error |= p_easy_setopt(easy, CURLOPT_PROTOCOLS_, CURLPROTO_HTTP_HTTPS);
    option_error |= p_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_, https_download(url) ? (1L << 1) : CURLPROTO_HTTP_HTTPS);
    option_error |= p_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER_, 1L);
    option_error |= p_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST_, 2L);
    option_error |= p_easy_setopt(easy, CURLOPT_NOSIGNAL_, 1L);
    option_error |= p_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_, 15L);
    option_error |= p_easy_setopt(easy, CURLOPT_TIMEOUT_, 300L);
    option_error |= p_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT_, 1L);
    option_error |= p_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME_, 60L);
    option_error |= p_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE_,
                                  static_cast<int64_t>(MAX_RULE_DOWNLOAD_BYTES));
    const std::string user_agent = std::string(MODULE_ID) + "/" +
                                   MODULE_VERSION + " (rules downloader)";
    option_error |= p_easy_setopt(easy, CURLOPT_USERAGENT_, user_agent.c_str());

    int perform_result = option_error == 0 ? p_easy_perform(easy) : -1;
    long response_code = 0;
    char *effective_url = nullptr;
    int response_info = p_easy_getinfo(easy, CURLINFO_RESPONSE_CODE_, &response_code);
    int url_info = p_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL_, &effective_url);
    // CURLINFO_EFFECTIVE_URL 由 easy handle 持有,cleanup 前必须复制。
    std::string final_url = effective_url ? effective_url : "";
    p_easy_cleanup(easy);
    int close_result = fclose(file);

    std::string effective_error;
    bool effective_valid = !final_url.empty() &&
        validate_download_url(final_url, &effective_error);
    bool success = option_error == 0 && perform_result == 0 &&
        response_info == 0 && url_info == 0 && response_code >= 200 &&
        response_code < 300 && effective_valid && !context.too_large &&
        !context.write_failed && close_result == 0 && valid_download_file(temp_path);
    if (success) return "";

    std::string reason;
    if (context.cancellation.cancelled || operation_closing()) reason = "卸载已请求，下载已安全取消";
    else if (context.too_large) reason = "响应超过 64 MiB";
    else if (response_code != 0 && (response_code < 200 || response_code >= 300))
        reason = "HTTP 状态码 " + std::to_string(response_code);
    else if (!effective_valid) reason = "重定向目标非法: " + effective_error;
    else if (option_error != 0) reason = "当前 libcurl 不支持所需安全选项";
    else if (error_buffer[0] != '\0') reason = error_buffer;
    else reason = "libcurl 下载或文件校验失败";
    unlink(temp_path.c_str());
    return reason;
}

const char *find_executable(const char *const *paths, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (access(paths[i], X_OK) == 0) return paths[i];
    }
    return nullptr;
}

bool run_download_command(const std::vector<std::string> &arguments) {
    if (arguments.empty() || unconfirmed_download_writer || operation_closing()) return false;
    std::vector<char *> argv;
    for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        if (setpgid(0, 0) || !inherit_task_lease()) _exit(127);
        struct rlimit file_limit;
        file_limit.rlim_cur = MAX_RULE_DOWNLOAD_BYTES;
        file_limit.rlim_max = MAX_RULE_DOWNLOAD_BYTES;
        if (setrlimit(RLIMIT_FSIZE, &file_limit)) _exit(127);

        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }

        execv(argv[0], argv.data());
        _exit(127);
    }

    setpgid(child, child);
    const bool group_owned = getpgid(child) == child;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(305);
    bool owned = true, interrupted = false;
    while (true) {
        siginfo_t info {};
        const int result = waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT);
        if (result == 0 && info.si_pid == child) break;
        if (result < 0 && errno != EINTR) { owned = false; interrupted = true; break; }
        if (operation_closing() || std::chrono::steady_clock::now() >= deadline) { interrupted = true; break; }
        usleep(100 * 1000);
    }
    const auto finished = finish_owned_child(child, owned, group_owned);
    if (!finished.reaped || !finished.group_gone) unconfirmed_download_writer = true;
    return !interrupted && finished.reaped && finished.group_gone && finished.exit_code == 0;
}

bool try_exec_backend(const std::vector<std::string> &arguments,
                      const std::string &temp_path) {
    int fd = create_temp_file(temp_path);
    if (fd < 0) return false;
    close(fd);
    bool success = run_download_command(arguments) && valid_download_file(temp_path);
    if (!success && !unconfirmed_download_writer) unlink(temp_path.c_str());
    return success;
}

std::string download_via_exec(const std::string &url,
                              const std::string &temp_path) {
    static const char *CURL_PATHS[] = {
#if defined(ADK_NFQ_TEST_CURL_PROGRAM)
        ADK_NFQ_TEST_CURL_PROGRAM,
#else
        "/system/bin/curl", "/system/xbin/curl", "/vendor/bin/curl",
#endif
    };
    if (const char *curl = find_executable(
            CURL_PATHS, sizeof(CURL_PATHS) / sizeof(CURL_PATHS[0]))) {
        if (try_exec_backend(build_curl_download_argv(curl, url, temp_path),
                             temp_path))
            return "";
    }
    if (unconfirmed_download_writer) return "无法确认下载进程退出，保留临时文件并停止后续下载";
    if (operation_closing()) return "卸载已请求，下载已安全取消";

    // BusyBox wget has no --max-redirect option and its internal TLS backend
    // need not validate certificates. Do not silently use it as a secure
    // fallback. A compatible GNU wget must accept the required options below.
    static const char *WGET_PATHS[] = {
        "/system/bin/wget", "/system/xbin/wget", "/vendor/bin/wget",
    };
    if (const char *wget = find_executable(
            WGET_PATHS, sizeof(WGET_PATHS) / sizeof(WGET_PATHS[0]))) {
        if (try_exec_backend(build_wget_download_argv(
                                 wget, url, temp_path), temp_path))
            return "";
    }
    return "无可用的安全下载后端或下载校验失败";
}

}  // namespace

namespace {
struct PrefixContext {
    std::string bytes; size_t limit = 4096; bool limited = false; long http_status = 0;
    TransferCancellation cancellation;
};

size_t write_prefix(char *data, size_t size, size_t count, void *user) {
    auto &context = *static_cast<PrefixContext *>(user);
    if (context.cancellation.check()) return 0;
    if (count && size > SIZE_MAX / count) return 0;
    const size_t incoming = size * count;
    const size_t take = std::min(incoming, context.limit - context.bytes.size());
    context.bytes.append(data, take);
    if (context.bytes.size() == context.limit) {
        context.limited = true;
        return 0; // intentional CURLE_WRITE_ERROR, even if Range was ignored
    }
    return incoming;
}

std::string prefix_libcurl(const std::string &url, PrefixContext &context) {
    void *easy = p_easy_init();
    if (!easy) return "curl 初始化失败";
    int options = configure_transfer_cancellation(easy, context.cancellation);
    options |= p_easy_setopt(easy, CURLOPT_URL_, url.c_str());
    const std::string range = "0-" + std::to_string(context.limit - 1);
    options |= p_easy_setopt(easy, CURLOPT_RANGE_, range.c_str());
    options |= p_easy_setopt(easy, CURLOPT_WRITEFUNCTION_, write_prefix);
    options |= p_easy_setopt(easy, CURLOPT_WRITEDATA_, &context);
    options |= p_easy_setopt(easy, CURLOPT_FAILONERROR_, 1L);
    options |= p_easy_setopt(easy, CURLOPT_FOLLOWLOCATION_, 1L);
    options |= p_easy_setopt(easy, CURLOPT_MAXREDIRS_, static_cast<long>(MAX_DOWNLOAD_REDIRECTS));
    options |= p_easy_setopt(easy, CURLOPT_PROTOCOLS_, CURLPROTO_HTTP_HTTPS);
    options |= p_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_, https_download(url) ? (1L << 1) : CURLPROTO_HTTP_HTTPS);
    const std::string user_agent = std::string(MODULE_ID)+"/"+MODULE_VERSION;
    options |= p_easy_setopt(easy, CURLOPT_USERAGENT_, user_agent.c_str());
    options |= p_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER_, 1L);
    options |= p_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST_, 2L);
    options |= p_easy_setopt(easy, CURLOPT_NOSIGNAL_, 1L);
    options |= p_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_, 5L);
    options |= p_easy_setopt(easy, CURLOPT_TIMEOUT_, 15L);
    const int performed = options ? -1 : p_easy_perform(easy);
    long status = 0;
    char *effective = nullptr;
    const bool info = p_easy_getinfo(easy, CURLINFO_RESPONSE_CODE_, &status) == 0 &&
                      p_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL_, &effective) == 0;
    const bool valid = info && effective && validate_download_url(effective) && status >= 200 && status < 300;
    context.http_status = status;
    p_easy_cleanup(easy);
    if (context.cancellation.cancelled || operation_closing()) return "卸载已请求，预览已安全取消";
    if (!options && valid && (performed == 0 || (performed == 23 && context.limited))) return "";
    return "预览 HTTP/TLS 请求失败（状态 " + std::to_string(status) + "）";
}

std::string prefix_exec(const std::string &url, PrefixContext &context) {
    static const char *paths[] = {
#if defined(ADK_NFQ_TEST_CURL_PROGRAM)
        ADK_NFQ_TEST_CURL_PROGRAM,
#else
        "/system/bin/curl", "/system/xbin/curl", "/vendor/bin/curl",
#endif
    };
    const char *program = find_executable(paths, sizeof(paths) / sizeof(paths[0]));
    if (!program) return "没有支持片段预览的安全 curl 后端";
    std::vector<std::string> arguments = {program, "--disable", "--globoff", "--fail", "--silent", "--show-error",
        "--location", "--max-redirs", "5", "--proto", "=http,https", "--proto-redir", https_download(url) ? "=https" : "=http,https",
        "--connect-timeout", "5", "--max-time", "15", "--range", "0-" + std::to_string(context.limit - 1),
        // Small prefixes must reach the pipe without waiting for a stdio block
        // to fill (the server may deliberately stream the tail very slowly).
        "--include", "--suppress-connect-headers", "--no-buffer", "--output", "-", "--", url};
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    int descriptors[2];
    if (pipe(descriptors) != 0) return "无法创建预览管道";
    const pid_t child = fork();
    if (child < 0) { close(descriptors[0]); close(descriptors[1]); return "无法创建预览进程"; }
    if (child == 0) {
        if (setpgid(0, 0) || !inherit_task_lease()) _exit(127);
        close(descriptors[0]);
        dup2(descriptors[1], STDOUT_FILENO); close(descriptors[1]);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) { dup2(null_fd, STDIN_FILENO); dup2(null_fd, STDERR_FILENO); if (null_fd > 2) close(null_fd); }
        execv(program, argv.data()); _exit(127);
    }
    setpgid(child, child);
    const bool group_owned = getpgid(child) == child;
    close(descriptors[1]);
    fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
    timespec start {}; clock_gettime(CLOCK_MONOTONIC, &start);
    std::string headers, error;
    size_t header_bytes = 0;
    bool body = false, ended = false;
    while (!ended && error.empty() && !context.limited) {
        if (context.cancellation.check()) { error = "卸载已请求，预览已安全取消"; break; }
        timespec now {}; clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 20) { error = "预览请求超时"; break; }
        pollfd descriptor {descriptors[0], POLLIN | POLLHUP, 0};
        const int polled = poll(&descriptor, 1, 100);
        if (polled < 0) { if (errno == EINTR) continue; error = "预览管道读取失败"; break; }
        if (polled == 0) continue;
        char buffer[4096];
        ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
        if (count < 0) { if (errno == EINTR || errno == EAGAIN) continue; error = "预览读取失败"; break; }
        if (count == 0) { ended = true; break; }
        std::string chunk(buffer, static_cast<size_t>(count));
        if (!body) {
            headers += chunk;
            chunk.clear();
            while (!body) {
                const auto split = headers.find("\r\n\r\n");
                if (split == std::string::npos) break;
                header_bytes += split + 4;
                if (header_bytes > 32768) { error = "预览响应头过大"; break; }
                std::istringstream status_line(headers);
                std::string protocol; int status = 0;
                status_line >> protocol >> status;
                context.http_status = status;
                headers.erase(0, split + 4);
                if (protocol.compare(0, 5, "HTTP/") != 0) { error = "预览响应头非法"; break; }
                if (status >= 100 && status < 200) continue;
                if (status >= 300 && status < 400) continue;
                if (status < 200 || status >= 300) { error = "预览 HTTP 状态 " + std::to_string(status); break; }
                body = true; chunk.swap(headers);
            }
            if (!body && header_bytes + headers.size() > 32768) error = "预览响应头过大";
        }
        if (body && error.empty()) write_prefix(chunk.data(), 1, chunk.size(), &context);
    }
    close(descriptors[0]);
    // EOF may precede the child's final exit. Reserve its PID with WNOWAIT
    // while allowing a short normal exit, then terminate only this owned group.
    bool owned = true;
    const auto exit_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (true) {
        siginfo_t info {};
        const int result = waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT);
        if (result == 0 && info.si_pid == child) break;
        if (result < 0 && errno != EINTR) { owned = false; break; }
        if (!ended || !error.empty() || context.limited || std::chrono::steady_clock::now() >= exit_deadline) break;
        usleep(1000);
    }
    const auto finished = finish_owned_child(child, owned, group_owned);
    if (!finished.reaped || !finished.group_gone) {
        unconfirmed_download_writer = true;
        return "无法确认预览进程退出，停止后续下载";
    }
    if (error.empty() && (!body || (!context.limited && finished.exit_code != 0)))
        error = "预览 HTTP/TLS 请求失败";
    return error;
}
} // namespace

std::string download_prefix(const std::string &url, std::string &prefix, bool &limited, size_t byte_limit) {
    long status = 0;
    return download_prefix_status(url,prefix,limited,byte_limit,status);
}

std::string download_prefix_status(const std::string &url, std::string &prefix, bool &limited, size_t byte_limit, long &http_status) {
    http_status = 0;
    prefix.clear(); limited = false;
    if (unconfirmed_download_writer) return "先前下载进程尚未确认退出";
    if (operation_closing()) return "卸载已请求，未开始预览";
    if (byte_limit == 0 || byte_limit > 32768) return "预览字节限制非法";
    std::string error;
    if (!validate_download_url(url, &error)) return error;
    PrefixContext context;
    context.limit = byte_limit;
    if (net_curl_available()) {
        error = prefix_libcurl(url, context);
        // Do not double a slow request's latency. Exec fallback is for systems
        // without libcurl, not a retry of TLS/HTTP failures.
    } else error = prefix_exec(url, context);
    http_status = context.http_status;
    if (error.empty()) { prefix = std::move(context.bytes); limited = context.limited; }
    return error;
}

std::string download_file(const std::string &url, const std::string &path) {
    if (unconfirmed_download_writer) return "先前下载进程尚未确认退出";
    if (operation_closing()) return "卸载已请求，未开始下载";
    std::string policy_error;
    if (!validate_download_url(url, &policy_error)) return policy_error;

    std::string temp_path = unique_temp_path(path);
    unlink(temp_path.c_str());
    std::string error;
    if (net_curl_available()) {
        log_line("libcurl", "使用受限 HTTP(S) 策略下载: %s", url.c_str());
        error = download_via_libcurl(url, temp_path);
        if (!error.empty())
            log_line("libcurl", "下载失败: %s,尝试无 shell 后端", error.c_str());
    } else {
        log_line("libcurl", "系统未找到可用 libcurl,尝试无 shell 后端");
    }

    if (operation_closing()) {
        unlink(temp_path.c_str());
        return "卸载已请求，下载已安全取消";
    }
    if (!valid_download_file(temp_path)) {
        unlink(temp_path.c_str());
        error = download_via_exec(url, temp_path);
    }
    if (!error.empty()) {
        if (!unconfirmed_download_writer) unlink(temp_path.c_str());
        return error;
    }
    if (operation_closing()) {
        unlink(temp_path.c_str());
        return "卸载已请求，下载结果未发布";
    }
    if (!valid_download_file(temp_path)) {
        unlink(temp_path.c_str());
        return "下载文件为空、过大或不是普通文件";
    }
    if (rename(temp_path.c_str(), path.c_str()) != 0) {
        unlink(temp_path.c_str());
        return "重命名失败";
    }
    return "";
}
