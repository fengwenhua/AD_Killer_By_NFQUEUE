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
#include "work_budget.h"
#include "process_identity.h"
#include <dirent.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#if defined(__linux__)
#include <sys/prctl.h>
#endif

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
typedef void *(*fn_curl_slist_append)(void *, const char *);
typedef void (*fn_curl_slist_free_all)(void *);

static fn_curl_global_init p_global_init;
static fn_curl_global_cleanup p_global_cleanup;
static fn_curl_easy_init p_easy_init;
static fn_curl_easy_cleanup p_easy_cleanup;
static fn_curl_easy_setopt p_easy_setopt;
static fn_curl_easy_perform p_easy_perform;
static fn_curl_easy_getinfo p_easy_getinfo;
static fn_curl_slist_append p_slist_append;
static fn_curl_slist_free_all p_slist_free_all;
static void *g_curl_lib = nullptr;
static bool g_curl_ready = false;

// curl.h 中跨 7.x/8.x 保持稳定的 ABI 数值。
enum {
    CURLOPT_WRITEDATA_ = 10001,
    CURLOPT_RANGE_ = 10007,
    CURLOPT_URL_ = 10002,
    CURLOPT_ERRORBUFFER_ = 10010,
    CURLOPT_HTTPHEADER_ = 10023,
    CURLOPT_HEADERDATA_ = 10029,
    CURLOPT_HEADERFUNCTION_ = 20079,
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
    CURLOPT_TIMEOUT_MS_ = 155,
    CURLOPT_CONNECTTIMEOUT_MS_ = 156,
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
    p_slist_append = reinterpret_cast<fn_curl_slist_append>(dlsym(g_curl_lib, "curl_slist_append"));
    p_slist_free_all = reinterpret_cast<fn_curl_slist_free_all>(dlsym(g_curl_lib, "curl_slist_free_all"));
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
struct TransferCancellation {
    bool enabled = !operation_marker_path.empty() || WorkBudget::active, cancelled = false, expired = false;
    std::chrono::steady_clock::time_point checked {};
    bool check() {
        if (!enabled || cancelled || expired) return cancelled || expired;
        const auto now = std::chrono::steady_clock::now();
        if (checked.time_since_epoch().count() && now - checked < std::chrono::milliseconds(100)) return false;
        checked = now; cancelled = operation_closing(); expired = WorkBudget::expired(); return cancelled || expired;
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
    size_t maximum = MAX_RULE_DOWNLOAD_BYTES;
    bool too_large = false;
    bool write_failed = false;
    TransferCancellation cancellation;
    HttpResponseHeaders headers;
    WorkBudget::Reservation *credit = nullptr;
};

size_t capture_download_header(char *data, size_t size, size_t count, void *userdata) {
    auto &context = *static_cast<DownloadContext *>(userdata);
    if (context.cancellation.check() || (count && size > SIZE_MAX / count)) return 0;
    const auto bytes = size * count;
    return context.headers.consume(std::string_view(data, bytes)) ? bytes : 0;
}

size_t write_download(char *data, size_t size, size_t count, void *userdata) {
    auto *context = static_cast<DownloadContext *>(userdata);
    if (context->cancellation.check()) return 0;
    if (count != 0 && size > SIZE_MAX / count) {
        context->too_large = true;
        return 0;
    }
    size_t bytes = size * count;
    if (bytes > context->maximum - context->bytes) {
        context->too_large = true;
        context->bytes = context->maximum;
        return 0;
    }
    if (context->credit && !context->credit->consume(bytes)) { context->too_large = true; return 0; }
    size_t written = fwrite(data, 1, bytes, context->file);
    context->bytes += bytes;
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
                                 const std::string &temp_path, size_t maximum = MAX_RULE_DOWNLOAD_BYTES,
                                 size_t *transferred = nullptr, const HttpValidators &request = {},
                                 DownloadResponse *response = nullptr, WorkBudget::Reservation *credit = nullptr) {
    if (transferred) *transferred = 0;
    const auto timeout_ms = WorkBudget::remaining_ms(300000);
    if (!timeout_ms) return "下载时间预算已耗尽";
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
    context.credit = credit;
    context.file = file;
    context.maximum = maximum;
    char error_buffer[256] = {0};
    int option_error = 0;
    void *request_headers = nullptr;
    if (!request.empty()) {
        if (!p_slist_append || !p_slist_free_all) option_error = -1;
        else {
            const auto header = !request.etag.empty() ? "If-None-Match: " + request.etag : "If-Modified-Since: " + request.last_modified;
            request_headers = p_slist_append(nullptr, header.c_str());
            if (!request_headers) option_error = -1;
            else option_error |= p_easy_setopt(easy, CURLOPT_HTTPHEADER_, request_headers);
        }
    }
    option_error |= configure_transfer_cancellation(easy, context.cancellation);
    option_error |= p_easy_setopt(easy, CURLOPT_URL_, url.c_str());
    // CURLOPT_WRITEFUNCTION takes a function pointer, not an object pointer.
    // Passing the exact callback type matters for the variadic API ABI.
    option_error |= p_easy_setopt(easy, CURLOPT_WRITEFUNCTION_, write_download);
    option_error |= p_easy_setopt(easy, CURLOPT_WRITEDATA_, &context);
    option_error |= p_easy_setopt(easy, CURLOPT_HEADERFUNCTION_, capture_download_header);
    option_error |= p_easy_setopt(easy, CURLOPT_HEADERDATA_, &context);
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
    option_error |= p_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS_, static_cast<long>(std::min<uint64_t>(15000, timeout_ms)));
    option_error |= p_easy_setopt(easy, CURLOPT_TIMEOUT_MS_, static_cast<long>(timeout_ms));
    option_error |= p_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT_, 1L);
    option_error |= p_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME_, 60L);
    option_error |= p_easy_setopt(easy, CURLOPT_MAXFILESIZE_LARGE_,
                                  static_cast<int64_t>(maximum));
    const std::string user_agent = std::string(MODULE_ID) + "/" +
                                   MODULE_VERSION + " (rules downloader)";
    option_error |= p_easy_setopt(easy, CURLOPT_USERAGENT_, user_agent.c_str());

    int perform_result = option_error == 0 ? p_easy_perform(easy) : -1;
    // libcurl can reject an unknown-length body before invoking our writer.
    // Charge the cap on CURLE_FILESIZE_EXCEEDED instead of refunding it as 0.
    if (perform_result == 63) context.bytes = maximum;
    long response_code = 0;
    char *effective_url = nullptr;
    int response_info = p_easy_getinfo(easy, CURLINFO_RESPONSE_CODE_, &response_code);
    int url_info = p_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL_, &effective_url);
    // CURLINFO_EFFECTIVE_URL 由 easy handle 持有,cleanup 前必须复制。
    std::string final_url = effective_url ? effective_url : "";
    p_easy_cleanup(easy);
    if (request_headers) p_slist_free_all(request_headers);
    int close_result = fclose(file);
    if (transferred) *transferred = context.bytes;

    std::string effective_error;
    bool effective_valid = !final_url.empty() &&
        validate_download_url(final_url, &effective_error);
    const bool header_valid = context.headers.valid() && context.headers.status() == response_code;
    const bool not_modified = response_code == 304 && !request.empty() && context.bytes == 0;
    if (response) {
        response->http_status = response_code;
        response->effective_url = final_url;
        response->validators = context.headers.validators();
        response->bytes = context.bytes;
        response->not_modified = not_modified;
        response->transient = !option_error && !context.cancellation.cancelled && !context.cancellation.expired &&
            (perform_result == 7 || perform_result == 18 || perform_result == 56 ||
             response_code == 502 || response_code == 503 || response_code == 504);
    }
    bool success = option_error == 0 && perform_result == 0 &&
        response_info == 0 && url_info == 0 && header_valid && effective_valid && !context.too_large &&
        !context.write_failed && close_result == 0 && (not_modified ||
        (response_code >= 200 && response_code < 300 && valid_download_file(temp_path)));
    if (success) { if (not_modified) unlink(temp_path.c_str()); return ""; }

    std::string reason;
    if (context.cancellation.cancelled || operation_closing()) reason = "卸载已请求，下载已安全取消";
    else if (context.cancellation.expired || WorkBudget::expired()) reason = "下载时间预算已耗尽";
    else if (context.too_large) reason = "响应超过本次下载正文上限";
    else if (!header_valid && response_code) reason = "响应头无效、不完整或超过 16 KiB";
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

struct ExecCapture {
    std::string output, headers;
    int exit_code = -1;
    bool confirmed = false, overflow = false;
};

bool result_pipe(int (&descriptors)[2]) {
    if (pipe(descriptors)) return false;
    bool ok = true;
    for (int &fd : descriptors) {
        if (fd < 3) {
            const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
            close(fd); fd = moved;
        }
        if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC)) ok = false;
    }
    if (ok) {
        const int flags = fcntl(descriptors[0], F_GETFL);
        ok = flags >= 0 && !fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK);
    }
    if (!ok) { for (int fd : descriptors) if (fd >= 0) close(fd); }
    return ok;
}

bool run_download_command(const std::vector<std::string> &arguments, uint64_t maximum,
                          ExecCapture *capture = nullptr) {
    const auto timeout_ms = WorkBudget::remaining_ms(305000);
    if (arguments.empty() || task_unconfirmed_network_writer || operation_closing() || !timeout_ms) return false;
    std::vector<char *> argv;
    for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    int output[2] {-1, -1}, headers[2] {-1, -1};
    if (capture) {
        if (!result_pipe(output)) return false;
        if (!result_pipe(headers)) { close(output[0]); close(output[1]); return false; }
    }
    pid_t child = fork();
    if (child < 0) {
        if (capture) { for (int fd : output) close(fd); for (int fd : headers) close(fd); }
        return false;
    }
    if (child == 0) {
        if (setpgid(0, 0) || !inherit_task_lease()) _exit(127);
        struct rlimit file_limit;
        file_limit.rlim_cur = maximum;
        file_limit.rlim_max = maximum;
        if (setrlimit(RLIMIT_FSIZE, &file_limit)) _exit(127);

        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        if (capture) {
            if (dup2(output[1], STDOUT_FILENO) < 0 || dup2(headers[1], STDERR_FILENO) < 0) _exit(127);
            for (int fd : output) close(fd);
            for (int fd : headers) close(fd);
        }

        execv(argv[0], argv.data());
        _exit(127);
    }

    setpgid(child, child);
    const bool group_owned = getpgid(child) == child;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool owned = true, interrupted = false;
    if (capture) { close(output[1]); close(headers[1]); }
    auto drain = [&] {
        if (!capture) return;
        for (int index = 0; index < 2; ++index) {
            auto &text = index ? capture->headers : capture->output;
            const auto limit = index ? HttpResponseHeaders::MAX_BYTES : size_t(8192);
            while (!capture->overflow) {
                char buffer[4096];
                const auto count = read(index ? headers[0] : output[0], buffer, sizeof(buffer));
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) break;
                if (static_cast<size_t>(count) > limit - text.size()) { capture->overflow = true; break; }
                text.append(buffer, static_cast<size_t>(count));
            }
        }
    };
    while (true) {
        drain();
        siginfo_t info {};
        const int result = waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT);
        if (result == 0 && info.si_pid == child) break;
        if (result < 0 && errno != EINTR) { owned = false; interrupted = true; break; }
        if ((capture && capture->overflow) || operation_closing() || std::chrono::steady_clock::now() >= deadline) { interrupted = true; break; }
        usleep(20 * 1000);
    }
    const auto finished = finish_owned_child(child, owned, group_owned);
    drain();
    if (capture) {
        close(output[0]); close(headers[0]);
        capture->confirmed = finished.reaped && finished.group_gone;
        capture->exit_code = finished.exit_code;
    }
    if (!finished.reaped || !finished.group_gone) task_unconfirmed_network_writer = true;
    return !interrupted && (!capture || !capture->overflow) && finished.reaped && finished.group_gone && finished.exit_code == 0;
}

bool try_exec_backend(const std::vector<std::string> &arguments,
                      const std::string &temp_path, WorkBudget::Reservation &body, ExecCapture *capture = nullptr) {
    int fd = create_temp_file(temp_path);
    if (fd < 0) { body.settle(0); return false; }
    close(fd);
    bool success = run_download_command(arguments, body.amount(), capture);
    const bool valid_file = valid_download_file(temp_path);
    if (!task_unconfirmed_network_writer) {
        struct stat info {};
        if (!capture && success && !lstat(temp_path.c_str(), &info) && S_ISREG(info.st_mode) && info.st_size >= 0 &&
            static_cast<uint64_t>(info.st_size) <= body.amount()) body.settle(info.st_size);
        if (!success) unlink(temp_path.c_str());
    }
    return success && (capture || valid_file);
}

std::string download_via_exec(const std::string &url,
                              const std::string &temp_path, const HttpValidators &request = {},
                              DownloadResponse *response = nullptr) {
    WorkBudget::Reservation body(MAX_RULE_DOWNLOAD_BYTES);
    const auto timeout_ms = WorkBudget::remaining_ms(300000);
    if (!timeout_ms) { body.settle(0); return "下载时间预算已耗尽"; }
    if (!body.amount()) return "累计下载正文预算已耗尽";
    static const char *CURL_PATHS[] = {
#if defined(ADK_NFQ_TEST_CURL_PROGRAM)
        ADK_NFQ_TEST_CURL_PROGRAM,
#else
        "/system/bin/curl", "/system/xbin/curl", "/vendor/bin/curl",
#endif
    };
    if (const char *curl = find_executable(
            CURL_PATHS, sizeof(CURL_PATHS) / sizeof(CURL_PATHS[0]))) {
        auto arguments = build_curl_download_argv(curl, url, temp_path, timeout_ms, body.amount());
        std::vector<std::string> extra = {"--dump-header", "/dev/stderr", "--write-out", "%{http_code}\n%{url_effective}\n%{size_download}\n"};
        if (!request.empty()) {
            extra.push_back("--header");
            extra.push_back(!request.etag.empty() ? "If-None-Match: " + request.etag : "If-Modified-Since: " + request.last_modified);
        }
        arguments.insert(arguments.end() - 2, extra.begin(), extra.end());
        ExecCapture capture;
        const bool success = try_exec_backend(arguments, temp_path, body, &capture);
        std::istringstream fields(capture.output);
        std::string status_text, effective, bytes_text, extra_line;
        uint64_t status = 0, transferred = 0;
        const bool reported = capture.confirmed && !capture.overflow &&
            static_cast<bool>(std::getline(fields, status_text)) && static_cast<bool>(std::getline(fields, effective)) &&
            static_cast<bool>(std::getline(fields, bytes_text)) && !std::getline(fields, extra_line) &&
            parse_u64(status_text, status) && status <= 599 && parse_u64(bytes_text, transferred);
        if (reported) body.settle(std::min<uint64_t>(transferred, body.amount()));
        HttpResponseHeaders received_headers;
        size_t offset = 0;
        bool headers_valid = true;
        while (offset < capture.headers.size()) {
            const auto end = capture.headers.find('\n', offset);
            if (end == std::string::npos || !received_headers.consume(std::string_view(capture.headers).substr(offset, end + 1 - offset))) { headers_valid = false; break; }
            offset = end + 1;
        }
        headers_valid = headers_valid && received_headers.valid() && received_headers.status() == static_cast<long>(status);
        const bool not_modified = reported && status == 304 && !request.empty() && !transferred;
        if (response) {
            response->http_status = reported ? static_cast<long>(status) : 0;
            response->effective_url = reported ? effective : "";
            response->bytes = reported ? static_cast<size_t>(std::min<uint64_t>(transferred, body.amount())) : 0;
            response->validators = received_headers.validators();
            response->not_modified = not_modified;
            response->transient = reported && !capture.overflow &&
                (capture.exit_code == 7 || capture.exit_code == 18 || capture.exit_code == 56 ||
                 (headers_valid && (status == 502 || status == 503 || status == 504)));
        }
        if (success && reported && headers_valid && validate_download_url(effective) && transferred <= body.amount() &&
            (not_modified || (status >= 200 && status < 300 && valid_download_file(temp_path)))) {
            if (not_modified) unlink(temp_path.c_str());
            return "";
        }
        if (task_unconfirmed_network_writer) return "无法确认下载进程退出，保留临时文件并停止后续下载";
        if (operation_closing()) return "卸载已请求，下载已安全取消";
        if (WorkBudget::expired()) return "下载时间预算已耗尽";
        if (capture.overflow) return "下载响应头或元数据超过上限";
        if (reported && status && (status < 200 || status >= 300)) return "HTTP 状态码 " + std::to_string(status);
        // Select a backend before the request. HTTP/TLS/content failures are
        // not evidence that another backend should repeat the same request.
        return "curl 下载失败，未切换后端重试";
    }
    if (task_unconfirmed_network_writer) return "无法确认下载进程退出，保留临时文件并停止后续下载";
    if (operation_closing()) return "卸载已请求，下载已安全取消";

    // BusyBox wget has no --max-redirect option and its internal TLS backend
    // need not validate certificates. Do not silently use it as a secure
    // fallback. A compatible GNU wget must accept the required options below.
    static const char *WGET_PATHS[] = {
#if defined(ADK_NFQ_TEST_WGET_PROGRAM)
        ADK_NFQ_TEST_WGET_PROGRAM,
#else
        "/system/bin/wget", "/system/xbin/wget", "/vendor/bin/wget",
#endif
    };
    if (const char *wget = find_executable(
            WGET_PATHS, sizeof(WGET_PATHS) / sizeof(WGET_PATHS[0]))) {
        if (try_exec_backend(build_wget_download_argv(
                                 wget, url, temp_path), temp_path, body)) {
            if (response) {
                response->http_status = 200; response->effective_url = url;
                struct stat info {};
                if (!stat(temp_path.c_str(), &info) && info.st_size > 0) response->bytes = info.st_size;
            }
            return "";
        }
        if (WorkBudget::expired()) return "下载时间预算已耗尽";
        return "wget 下载或文件校验失败";
    }
    body.settle(0);
    return "无可用的安全下载后端或下载校验失败";
}

// Only the synchronous CLI may run library work after fork. A previously
// initialized parent library or an unknown/multiple-thread state selects the
// exec backend instead. The child never starts another download process group.
bool can_fork_libcurl() {
    if (g_curl_ready || g_curl_lib) return false;
#if defined(__linux__)
    DIR *directory = opendir("/proc/self/task");
    if (!directory) return false;
    size_t count = 0;
    errno = 0;
    while (auto *entry = readdir(directory)) {
        if (entry->d_name[0] != '.') ++count;
    }
    const bool ok = errno == 0 && count == 1;
    closedir(directory);
    return ok;
#elif defined(__APPLE__)
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return false;
    for (mach_msg_type_number_t i = 0; i < count; ++i) mach_port_deallocate(mach_task_self(), threads[i]);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_t));
    return count == 1;
#else
    return false;
#endif
}

struct LibraryReply {
    uint32_t magic = 0x41444b31, available = 0;
    uint64_t bytes = 0;
    uint32_t http_status = 0, flags = 0;
    char error[384] {};
    char effective_url[4097] {}, etag[257] {}, last_modified[129] {};
};

std::string supervised_libcurl(const std::string &url, const std::string &temp_path, bool &unavailable,
                              const HttpValidators &request, DownloadResponse &response) {
    unavailable = false;
    WorkBudget::Reservation body(MAX_RULE_DOWNLOAD_BYTES, true);
    if (WorkBudget::expired()) { body.settle(0); return "下载时间预算已耗尽"; }
    if (!body.amount()) return "累计下载正文预算已耗尽";
    int descriptors[2];
    if (pipe(descriptors)) { body.settle(0); return "无法创建下载结果管道"; }
    bool pipe_ok = true;
    for (int &fd : descriptors) {
        if (fd < 3) {
            const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
            close(fd); fd = moved;
        }
        if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC)) pipe_ok = false;
    }
    if (!pipe_ok) {
        for (int fd : descriptors) if (fd >= 0) close(fd);
        body.settle(0); return "无法保护下载结果管道";
    }
    const int flags = fcntl(descriptors[0], F_GETFL);
    if (flags < 0 || fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK)) {
        close(descriptors[0]); close(descriptors[1]); body.settle(0);
        return "无法设置下载结果管道";
    }
    const pid_t parent = getpid();
    const pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]); close(descriptors[1]); body.settle(0);
        return "无法创建下载子进程";
    }
    if (!child) {
        close(descriptors[0]);
        if (setpgid(0, 0) || !inherit_task_lease()) _exit(127);
#if defined(__linux__)
        // Library work has no independent parent watchdog after an owner
        // crash. End this download, including resolver threads, with its owner.
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(127);
#else
        (void)parent;
#endif
        LibraryReply reply;
        reply.available = net_curl_available() ? 1 : 0;
        if (reply.available) {
            size_t transferred = 0;
            DownloadResponse received;
            const auto error = download_via_libcurl(url, temp_path, body.amount(), &transferred, request, &received, &body);
            reply.bytes = transferred;
            reply.http_status = received.http_status;
            reply.flags = (received.not_modified ? 1u : 0u) | (received.transient ? 2u : 0u);
            std::snprintf(reply.effective_url, sizeof(reply.effective_url), "%s", received.effective_url.c_str());
            std::snprintf(reply.etag, sizeof(reply.etag), "%s", received.validators.etag.c_str());
            std::snprintf(reply.last_modified, sizeof(reply.last_modified), "%s", received.validators.last_modified.c_str());
            if (!error.empty()) std::snprintf(reply.error, sizeof(reply.error), "%s",
                error.size() < sizeof(reply.error) ? error.c_str() : "下载失败（错误信息过长）");
        }
        size_t total = 0;
        while (total < sizeof(reply)) {
            const auto written = write(descriptors[1], reinterpret_cast<const char *>(&reply) + total, sizeof(reply) - total);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            total += static_cast<size_t>(written);
        }
        close(descriptors[1]);
        _exit(total == sizeof(reply) ? 0 : 127);
    }
    close(descriptors[1]);
    setpgid(child, child);
    const bool group_owned = getpgid(child) == child;
    bool owned = true, interrupted = false;
    LibraryReply reply;
    size_t received = 0;
    bool overflow = false;
    auto drain = [&] {
        while (!overflow) {
            char extra;
            const auto count = received < sizeof(reply) ?
                read(descriptors[0], reinterpret_cast<char *>(&reply) + received, sizeof(reply) - received) :
                read(descriptors[0], &extra, 1);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            if (received == sizeof(reply)) { overflow = true; break; }
            received += static_cast<size_t>(count);
        }
    };
    while (true) {
        drain();
        siginfo_t info {};
        const int result = waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT);
        if (!result && info.si_pid == child) break;
        if (result < 0 && errno != EINTR) { owned = false; interrupted = true; break; }
        if (overflow || operation_closing() || WorkBudget::expired()) { interrupted = true; break; }
        usleep(20000);
    }
    const auto finished = finish_owned_child(child, owned, group_owned);
    drain();
    close(descriptors[0]);
    if (!finished.reaped || !finished.group_gone) {
        task_unconfirmed_network_writer = true;
        return "无法确认下载进程退出，保留临时文件并停止后续下载";
    }
    const bool reported = finished.exit_code == 0 && !overflow && received == sizeof(reply) &&
        reply.magic == 0x41444b31 && reply.available <= 1 && reply.bytes <= body.amount() &&
        reply.flags <= 3 && reply.http_status <= 599 &&
        std::memchr(reply.error, 0, sizeof(reply.error)) &&
        std::memchr(reply.effective_url, 0, sizeof(reply.effective_url)) &&
        std::memchr(reply.etag, 0, sizeof(reply.etag)) &&
        std::memchr(reply.last_modified, 0, sizeof(reply.last_modified));
    if (reported) body.settle(reply.bytes);
    if (operation_closing()) return "卸载已请求，下载已安全取消";
    if (WorkBudget::expired()) return "下载时间预算已耗尽";
    if (interrupted || !reported) return "下载子进程结果未能核实";
    unavailable = !reply.available;
    response.bytes = reply.bytes;
    response.http_status = reply.http_status;
    response.effective_url = reply.effective_url;
    response.validators = {reply.etag, reply.last_modified};
    response.not_modified = reply.flags & 1;
    response.transient = reply.flags & 2;
    return reply.error;
}

}  // namespace

namespace {
struct PrefixContext {
    std::string bytes; size_t limit = 4096; bool limited = false, whole = false; long http_status = 0;
    TransferCancellation cancellation;
};

size_t write_prefix(char *data, size_t size, size_t count, void *user) {
    auto &context = *static_cast<PrefixContext *>(user);
    if (context.cancellation.check()) return 0;
    if (count && size > SIZE_MAX / count) return 0;
    const size_t incoming = size * count;
    const size_t take = std::min(incoming, context.limit - context.bytes.size());
    context.bytes.append(data, take);
    if (incoming > take || (!context.whole && context.bytes.size() == context.limit)) {
        context.limited = true;
        return 0; // intentional CURLE_WRITE_ERROR, even if Range was ignored
    }
    return incoming;
}

std::string prefix_libcurl(const std::string &url, PrefixContext &context) {
    const auto timeout_ms = WorkBudget::remaining_ms(15000);
    if (!timeout_ms) return "下载时间预算已耗尽";
    void *easy = p_easy_init();
    if (!easy) return "curl 初始化失败";
    int options = configure_transfer_cancellation(easy, context.cancellation);
    options |= p_easy_setopt(easy, CURLOPT_URL_, url.c_str());
    const std::string range = "0-" + std::to_string(context.limit - 1);
    if (!context.whole) options |= p_easy_setopt(easy, CURLOPT_RANGE_, range.c_str());
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
    options |= p_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS_, static_cast<long>(std::min<uint64_t>(5000, timeout_ms)));
    options |= p_easy_setopt(easy, CURLOPT_TIMEOUT_MS_, static_cast<long>(timeout_ms));
    const int performed = options ? -1 : p_easy_perform(easy);
    long status = 0;
    char *effective = nullptr;
    const bool info = p_easy_getinfo(easy, CURLINFO_RESPONSE_CODE_, &status) == 0 &&
                      p_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL_, &effective) == 0;
    const bool valid = info && effective && validate_download_url(effective) &&
        (context.whole ? status == 200 : status >= 200 && status < 300);
    context.http_status = status;
    p_easy_cleanup(easy);
    if (context.cancellation.cancelled || operation_closing()) return "卸载已请求，预览已安全取消";
    if (!options && valid && (performed == 0 || (performed == 23 && context.limited))) return "";
    return "预览 HTTP/TLS 请求失败（状态 " + std::to_string(status) + "）";
}

std::string prefix_exec(const std::string &url, PrefixContext &context) {
    const auto timeout_ms = WorkBudget::remaining_ms(15000);
    if (!timeout_ms) return "下载时间预算已耗尽";
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
        "--connect-timeout", "5", "--max-time", std::to_string(timeout_ms / 1000) + "." + std::to_string(1000 + timeout_ms % 1000).substr(1),
        // Small prefixes must reach the pipe without waiting for a stdio block
        // to fill (the server may deliberately stream the tail very slowly).
        "--include", "--suppress-connect-headers", "--no-buffer", "--output", "-", "--", url};
    if (!context.whole) arguments.insert(arguments.end() - 2, {"--range", "0-" + std::to_string(context.limit - 1)});
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
                if (status < 200 || status >= 300 || (context.whole && status != 200)) { error = "预览 HTTP 状态 " + std::to_string(status); break; }
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
        task_unconfirmed_network_writer = true;
        return "无法确认预览进程退出，停止后续下载";
    }
    if (error.empty() && (!body || (!context.limited && finished.exit_code != 0)))
        error = "预览 HTTP/TLS 请求失败";
    return error;
}

// A library call may block in initialization or a synchronous resolver. Only
// the fresh, verified single-threaded CLI forks library work; the parent owns
// the deadline, pipe cap and still-unreaped process group.
struct DocumentReply {
    uint32_t magic = 0x41444b32, available = 0, status = 0, limited = 0, bytes = 0;
    char error[512] {};
};
std::string supervised_document(const std::string &url, PrefixContext &context, bool &unavailable) {
    unavailable = false;
    int descriptors[2];
    if (!result_pipe(descriptors)) return "无法创建更新信息管道";
    const pid_t parent = getpid(), child = fork();
    if (child < 0) { close(descriptors[0]); close(descriptors[1]); return "无法创建更新信息子进程"; }
    if (!child) {
        close(descriptors[0]);
        if (setpgid(0, 0) || !inherit_task_lease()) _exit(127);
#if defined(__linux__)
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(127);
#else
        (void)parent;
#endif
        DocumentReply reply;
        reply.available = net_curl_available();
        if (reply.available) {
            const auto error = prefix_libcurl(url, context);
            reply.status = static_cast<uint32_t>(context.http_status);
            reply.limited = context.limited; reply.bytes = static_cast<uint32_t>(context.bytes.size());
            if (!error.empty()) std::snprintf(reply.error, sizeof(reply.error), "%s",
                error.size() < sizeof(reply.error) ? error.c_str() : "更新信息下载失败");
        }
        const auto send = [&](const char *bytes, size_t size) {
            size_t sent = 0;
            while (sent < size) {
                const auto count = write(descriptors[1], bytes + sent, size - sent);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) return false;
                sent += static_cast<size_t>(count);
            }
            return true;
        };
        const bool sent = send(reinterpret_cast<const char *>(&reply), sizeof(reply)) &&
            send(context.bytes.data(), context.bytes.size());
        close(descriptors[1]); _exit(sent ? 0 : 127);
    }
    close(descriptors[1]); setpgid(child, child);
    const bool group_owned = getpgid(child) == child;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(WorkBudget::remaining_ms(20000));
    std::string wire, error;
    bool owned = true, eof = false;
    const auto maximum = sizeof(DocumentReply) + context.limit;
    while (true) {
        if (operation_closing()) { error = "卸载已请求，更新检查已取消"; break; }
        if (WorkBudget::expired() || std::chrono::steady_clock::now() >= deadline) { error = "更新检查时间预算已耗尽"; break; }
        char chunk[8192];
        const auto count = read(descriptors[0], chunk, sizeof(chunk));
        if (count > 0) {
            if (static_cast<size_t>(count) > maximum - wire.size()) { error = "更新信息子进程输出过大"; break; }
            wire.append(chunk, static_cast<size_t>(count));
        } else if (!count) eof = true;
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) { error = "更新信息管道读取失败"; break; }
        siginfo_t info {};
        if (waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
            if (errno == EINTR) continue;
            owned = errno != ECHILD; error = "更新信息子进程身份无法确认"; break;
        }
        if (eof && info.si_pid == child) break;
        if (count <= 0) { pollfd descriptor{descriptors[0], POLLIN | POLLHUP, 0}; poll(&descriptor, 1, 10); }
    }
    close(descriptors[0]);
    const auto finished = finish_owned_child(child, owned, group_owned);
    if (!finished.reaped || !finished.group_gone) {
        task_unconfirmed_network_writer = true;
        return "无法确认更新信息子进程退出，停止后续请求";
    }
    if (!error.empty()) return error;
    DocumentReply reply;
    if (wire.size() < sizeof(reply)) return "更新信息子进程结果不完整";
    std::memcpy(&reply, wire.data(), sizeof(reply));
    if (finished.exit_code || reply.magic != 0x41444b32 || reply.available > 1 || reply.status > 599 || reply.limited > 1 ||
        reply.bytes > context.limit || wire.size() != sizeof(reply) + reply.bytes || !std::memchr(reply.error, 0, sizeof(reply.error)))
        return "更新信息子进程结果无效";
    unavailable = !reply.available;
    context.bytes.assign(wire.data() + sizeof(reply), reply.bytes);
    context.http_status = reply.status; context.limited = reply.limited;
    return reply.error;
}
} // namespace

std::string download_prefix(const std::string &url, std::string &prefix, bool &limited, size_t byte_limit) {
    long status = 0;
    return download_prefix_status(url,prefix,limited,byte_limit,status);
}

std::string download_prefix_status(const std::string &url, std::string &prefix, bool &limited, size_t byte_limit, long &http_status) {
    http_status = 0;
    prefix.clear(); limited = false;
    if (task_unconfirmed_network_writer) return "先前下载进程尚未确认退出";
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

std::string download_document_status(const std::string &url, std::string &body, size_t byte_limit, long &http_status) {
    body.clear(); http_status = 0;
    if (!byte_limit || byte_limit > 512u * 1024u) return "更新信息大小限制非法";
    if (task_unconfirmed_network_writer) return "先前下载进程尚未确认退出";
    if (operation_closing() || WorkBudget::expired()) return "更新检查已取消或时间预算耗尽";
    std::string error;
    if (!validate_download_url(url, &error)) return error;
    WorkBudget::Reservation allowance(byte_limit);
    if (!allowance.amount()) return "更新检查累计下载量已达上限";
    PrefixContext context;
    context.limit = static_cast<size_t>(allowance.amount()); context.whole = true;
    bool unavailable = true;
    if (can_fork_libcurl()) error = supervised_document(url, context, unavailable);
    if (unavailable && error.empty()) error = prefix_exec(url, context);
    http_status = context.http_status;
    if (!task_unconfirmed_network_writer && error.empty()) allowance.settle(context.bytes.size());
    if (WorkBudget::expired()) return "更新检查时间预算已耗尽";
    if (context.limited) return "更新信息超过本次大小上限";
    if (error.empty()) body = std::move(context.bytes);
    return error;
}

static DownloadResponse download_once(const std::string &url, const std::string &path, const HttpValidators &request) {
    DownloadResponse response;
    const auto fail = [&](const std::string &error) { response.error = error; return response; };
    if (task_unconfirmed_network_writer) return fail("先前下载进程尚未确认退出");
    if (operation_closing()) return fail("卸载已请求，未开始下载");
    if (WorkBudget::expired()) return fail("下载时间预算已耗尽");
    if (!valid_http_validators(request)) return fail("条件请求验证器非法");
    std::string policy_error;
    if (!validate_download_url(url, &policy_error)) return fail(policy_error);

    std::string temp_path = unique_temp_path(path);
    unlink(temp_path.c_str());
    std::string error;
    if (WorkBudget::active) {
        bool unavailable = false;
        if (can_fork_libcurl()) error = supervised_libcurl(url, temp_path, unavailable, request, response);
        else unavailable = true;
        if (unavailable && error.empty()) error = download_via_exec(url, temp_path, request, &response);
    } else if (net_curl_available()) {
        log_line("libcurl", "使用受限 HTTP(S) 策略下载: %s", url.c_str());
        error = download_via_libcurl(url, temp_path, MAX_RULE_DOWNLOAD_BYTES, nullptr, request, &response);
        if (!error.empty())
            log_line("libcurl", "下载失败: %s", error.c_str());
    } else {
        log_line("libcurl", "系统未找到可用 libcurl,尝试无 shell 后端");
        error = download_via_exec(url, temp_path, request, &response);
    }
    if (!error.empty()) {
        if (!task_unconfirmed_network_writer) unlink(temp_path.c_str());
        return fail(error);
    }
    if (operation_closing()) {
        unlink(temp_path.c_str());
        return fail("卸载已请求，下载结果未发布");
    }
    if (WorkBudget::expired()) {
        unlink(temp_path.c_str());
        return fail("下载时间预算已耗尽，结果未发布");
    }
    if (response.not_modified) { unlink(temp_path.c_str()); return response; }
    if (!valid_download_file(temp_path)) {
        unlink(temp_path.c_str());
        return fail("下载文件为空、过大或不是普通文件");
    }
    if (rename(temp_path.c_str(), path.c_str()) != 0) {
        unlink(temp_path.c_str());
        return fail("重命名失败");
    }
    return response;
}

DownloadResponse download_file_response(const std::string &url, const std::string &path, const HttpValidators &request) {
    auto response = download_once(url, path, request);
    if (!response.error.empty() && response.transient && WorkBudget::active &&
        WorkBudget::remaining_ms(1000) > 400 && WorkBudget::remaining_bytes(1) &&
        !task_unconfirmed_network_writer && !operation_closing()) {
        const auto first_bytes = response.bytes;
        log_line("更新", "瞬时下载错误，预算内最多重试一次: %s", url.c_str());
        for (int tick = 0; tick < 10; ++tick) {
            if (WorkBudget::expired() || operation_closing()) return response;
            usleep(20000);
        }
        response = download_once(url, path, request);
        response.bytes += std::min<size_t>(first_bytes, SIZE_MAX - response.bytes);
    }
    return response;
}

std::string download_file(const std::string &url, const std::string &path) {
    return download_file_response(url, path).error;
}
