#include "module_update.h"
#include "common.h"
#include "net.h"
#include "logging.h"
#include "exclusive_file_lock.h"
#include "source_config.h"
#include "sha256.h"
#include "module_package.h"
#include "task_context.h"
#include "release_catalog.h"
#include "bounded_json.h"
#include "work_budget.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>

namespace module_update {
namespace {
#if defined(ADK_NFQ_TEST_RELEASE_REPO) && defined(ADK_NFQ_TEST_RELEASE_API)
constexpr const char *REPO = ADK_NFQ_TEST_RELEASE_REPO;
constexpr const char *API = ADK_NFQ_TEST_RELEASE_API;
#else
constexpr const char *REPO = "https://github.com/fengwenhua/AD_Killer_By_NFQUEUE";
constexpr const char *API = "https://api.github.com/repos/fengwenhua/AD_Killer_By_NFQUEUE/releases";
#endif
constexpr const char *DIRECTORY = DATA_DIR "/module_updates";
constexpr size_t MAX_PACKAGE_BYTES = 64u * 1024u * 1024u;

bool valid_digest(const std::string &value) {
    if (value.size() != 64) return false;
    for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
bool private_directory(const std::string &path) {
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == geteuid() && chmod(path.c_str(), 0700) == 0;
}
std::string receipt_digest(const std::string &directory, std::string &version, std::string &channel) {
    std::string receipt;
    if (!read_file((directory+"/ready").c_str(), receipt) || receipt.size() > 128) return "";
    size_t separator = receipt.find('\n');
    if (separator == std::string::npos) return "";
    version = receipt.substr(0, separator);
    size_t end = receipt.find('\n', separator+1);
    if (end == std::string::npos) return "";
    auto digest = receipt.substr(separator+1, end-separator-1);
    auto tail = receipt.substr(end+1);
    channel = tail == "stable\n" ? "stable" : tail == "preview\n" ? "preview" : "unknown";
    if (!tail.empty() && channel == "unknown") return "";
    return valid_version(version) && valid_digest(digest) ? digest : "";
}
std::string install_package(const std::string &path) {
    struct Installer { const char *binary; bool magisk; };
    const Installer choices[] = {{"/data/adb/ksud",false}, {"/data/adb/apd",false},
                                 {"/data/adb/magisk/magisk",true}};
    const Installer *selected = nullptr;
    for (const auto &choice : choices) {
        if (access(choice.binary, X_OK) != 0) continue;
        if (selected) return "检测到多个根管理器，请在管理器中手动安装 Release ZIP";
        selected = &choice;
    }
    if (!selected) return "未找到支持的根管理器安装接口，请在管理器中手动安装 Release ZIP";
    int output[2];
    if (pipe(output) != 0) return "无法创建安装输出管道";
    pid_t child = fork();
    if (child < 0) { close(output[0]); close(output[1]); return "无法启动安装器"; }
    if (child == 0) {
        if (!inherit_task_lease()) _exit(127);
        setpgid(0,0);
        close(output[0]);
        dup2(output[1], STDOUT_FILENO); dup2(output[1], STDERR_FILENO); close(output[1]);
        if (selected->magisk)
            execl(selected->binary, selected->binary, "--install-module", path.c_str(), (char *)nullptr);
        else
            execl(selected->binary, selected->binary, "module", "install", path.c_str(), (char *)nullptr);
        _exit(127);
    }
    close(output[1]);
    setpgid(child,child);
    fcntl(output[0], F_SETFL, O_NONBLOCK);
#if defined(ADK_NFQ_TEST_INSTALL_WARNING_MS)
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::milliseconds(ADK_NFQ_TEST_INSTALL_WARNING_MS);
#else
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::minutes(5);
#endif
    int result = 0;
    bool finished = false, warned = false, pipe_open = true;
    while (true) {
        char buffer[2048];
        ssize_t count = pipe_open ? read(output[0], buffer, sizeof(buffer)-1) : 0;
        if (count > 0) { buffer[count] = 0; log_line("模块安装", "%s", buffer); }
        if (pipe_open && (count == 0 || (count < 0 && errno != EINTR && errno != EAGAIN))) {
            close(output[0]); pipe_open = false;
        }
        if (!finished) {
            pid_t waited = waitpid(child, &result, WNOHANG);
            if (waited == child) finished = true;
            else if (waited < 0 && errno != EINTR) break;
        }
        if (finished && count <= 0) break;
        if (!warned && std::chrono::steady_clock::now() >= deadline) {
            warned = true;
            log_line("模块安装", "安装器运行时间较长，继续等待管理器完成；不会自动终止安装或清理安装包");
        }
        pollfd descriptor{pipe_open ? output[0] : -1, POLLIN, 0};
        poll(&descriptor, 1, 100);
    }
    if (pipe_open) close(output[0]);
    if (!finished) {
        task_installer_completion_unknown = true;
        return "无法确认安装器是否完成，保留安装阶段和安装包，请检查管理器状态";
    }
    return WIFEXITED(result) && WEXITSTATUS(result)==0 ? "" : "管理器安装失败，请查看任务日志";
}
}

std::string package_name(const std::string &version) {
    return valid_version(version) ? "AD_Killer_By_NFQUEUE-v"+version+".zip" : "";
}
std::string asset_url(const std::string &version) {
    return valid_version(version) ? std::string(REPO)+"/releases/download/v"+version+"/"+package_name(version) : "";
}
std::string verify_package(const std::string &path, const std::string &digest) {
    if (!valid_digest(digest)) return "SHA-256 格式非法";
    int fd = open(path.c_str(), O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd < 0) return "无法打开安装包";
    struct stat st{};
    if (fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size < 4 ||
        static_cast<uint64_t>(st.st_size) > MAX_PACKAGE_BYTES) {
        close(fd); return "安装包不是普通 ZIP 文件或超过 64 MiB";
    }
    Sha256 hash;
    char buffer[8192];
    bool first = true;
    size_t total = 0;
    for (;;) {
        ssize_t count = read(fd,buffer,sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { close(fd); return "读取安装包失败"; }
        if (!count) break;
        if (first && (count < 4 || memcmp(buffer,"PK\003\004",4)!=0)) {
            close(fd); return "下载内容不是 ZIP 安装包";
        }
        first = false; total += static_cast<size_t>(count);
        if (total > MAX_PACKAGE_BYTES) { close(fd); return "安装包超过 64 MiB"; }
        hash.update(buffer,static_cast<size_t>(count));
    }
    close(fd);
    return hash.finish()==digest ? "" : "SHA-256 校验失败，禁止安装";
}
std::string download(const std::string &version, const std::string &directory, const Fetch &fetch, const Prefix &prefix, bool preview) {
    if (!valid_version(version)) return "版本号非法";
    if (!private_directory(directory)) return "无法创建私有模块更新目录";
    const std::string package = directory+"/package.zip", staged = directory+"/candidate.zip";
    unlink((directory+"/ready").c_str());
    unlink(staged.c_str());
    std::string checksum;
    bool limited = false;
    std::string error = prefix(asset_url(version)+".sha256", checksum, limited, 512);
    if (!error.empty()) return error;
    if (limited || checksum.size() < 64 || checksum.substr(64) != "  "+package_name(version)+"\n" ||
        !valid_digest(checksum.substr(0,64))) return "Release 校验和文件格式或文件名不匹配";
    log_line("模块更新", "下载 v%s",version.c_str());
    error = fetch(asset_url(version),staged);
    if (error.empty()) error = verify_package(staged,checksum.substr(0,64));
    if (error.empty() && rename(staged.c_str(),package.c_str()) != 0) error = "保存安装包失败";
    unlink(staged.c_str());
    if (error.empty() && !write_file_atomic((directory+"/ready").c_str(),version+"\n"+checksum.substr(0,64)+"\n"+(preview ? "preview\n" : "stable\n")))
        error = "保存安装包校验记录失败";
    return error;
}
std::string ready_version(const std::string &directory) {
    std::string version, channel;
    auto digest = receipt_digest(directory,version,channel);
    return !digest.empty() && verify_package(directory+"/package.zip",digest).empty() ? version : "";
}
std::string ready_channel(const std::string &directory) {
    std::string version, channel;
    auto digest = receipt_digest(directory,version,channel);
    return !digest.empty() && verify_package(directory+"/package.zip",digest).empty() ? channel : "unknown";
}
static bool valid_metadata(const std::string &body) {
    bounded_json::Value parsed;
    if (!bounded_json::parse(body, parsed)) return false;
    const auto text = [&](const char *key) -> std::string {
        const auto value = parsed.get(key);
        return value && value->kind == bounded_json::Kind::STRING ? value->text : "";
    };
    const auto version = text("version");
    if (version.empty() || version.front() != 'v' || !valid_version(version.substr(1))) return false;
    const auto code = parsed.get("versionCode"); uint64_t number = 0;
    const auto channel = parsed.get("releaseChannel");
    return text("module") == MODULE_ID && code && code->integer(number) && number > 0 && number <= INT32_MAX &&
        (!channel || text("releaseChannel") == "stable") && valid_digest(text("sha256")) &&
        text("zipUrl") == asset_url(version.substr(1)) &&
        text("changelog") == std::string(REPO) + "/releases/download/" + version + "/changelog.md";
}
static int release_response(bool preview, bool notes) {
#if defined(ADK_NFQ_TEST_RELEASE_TIME_MS)
    WorkBudget total(ADK_NFQ_TEST_RELEASE_TIME_MS, release_catalog::TOTAL_BYTES);
#else
    WorkBudget total(release_catalog::TIME_MS, release_catalog::TOTAL_BYTES);
#endif
    std::string body, output, error;
    bool metadata = false, api_request = false;
    long http_status = 0;
    if (!preview && !notes) {
        error = download_document_status(std::string(REPO)+"/releases/latest/download/update.json", body, 32768, http_status);
        if (error.empty() && !valid_metadata(body)) error = "正式版更新信息格式无效";
        metadata = error.empty();
        if (metadata) output = "{\"update\":" + body + "}";
    }
    if (preview) {
        api_request = true;
        const auto result = release_catalog::scan(API, REPO, download_document_status);
        error = result.error; http_status = result.http_status;
        if (result.ok) output = result.json;
    } else if (notes || !error.empty()) {
        api_request = true;
        error = download_document_status(std::string(API) + (notes ? "?per_page=1" : "/latest"), body, release_catalog::PAGE_BYTES, http_status);
        if (error.empty()) {
            const auto page = release_catalog::project(body, REPO, notes, !notes);
            if (!page.valid) error = page.error;
            else if (!page.candidates) error = notes ? "GitHub 暂无可用的发布日志" : "暂未找到包含有效安装包与校验和的正式版";
            else output = page.json;
        }
    }
    if (WorkBudget::expired()) error = "更新检查时间预算已耗尽，请稍后重试";
    if (!error.empty()) {
        if (api_request && (http_status == 403 || http_status == 429)) {
            error = "GitHub API 访问受限（HTTP " + std::to_string(http_status) + "），当前网络可能已用完匿名请求配额。请稍后重试或切换网络。";
            if (preview) error += "正式版检查可关闭“包含预览版”。";
        }
        else if (http_status >= 400)
            error = "GitHub 请求失败（HTTP " + std::to_string(http_status) + "），请稍后重试。";
        printf("{\"status\":\"error\",\"http_status\":%ld,\"message\":%s}\n",http_status,source_json_string(error).c_str());
        return 1;
    }
    puts(output.c_str()); return 0;
}
int release_info(bool preview) { return release_response(preview,false); }
int release_notes() { return release_response(false,true); }
int worker(const std::string &version, bool install, bool preview) {
    const std::string channel = preview ? "preview" : "stable";
    if (!upgrade_allowed(version,MODULE_VERSION,MODULE_RELEASE_CHANNEL,channel)) { log_line("模块更新","版本或发布渠道不是可用的升级"); return 1; }
    if (!private_directory(DIRECTORY)) return 1;
    ExclusiveFileLock lock(DATA_DIR "/.module-update.lock");
    if (!lock.ok()) { log_line("模块更新","已有下载或安装正在运行"); return 1; }
    if (!task_progress(install ? "verifying" : "downloading")) return 1;
    std::string error;
    if (install) {
        if (ready_version(DIRECTORY) != version) error = "安装包未下载或校验失败，请重新下载";
        else if (ready_channel(DIRECTORY) != channel) error = "安装包渠道未核实或与请求不一致，请重新下载";
        else {
            error = validate_module_identity(std::string(DIRECTORY)+"/package.zip",version,channel,MODULE_VERSION_CODE);
            if (error.empty() && !task_progress("installing")) error = "无法记录安装阶段，未派发安装器";
            if (error.empty()) error = install_package(std::string(DIRECTORY)+"/package.zip");
        }
        if (error.empty() && !write_file_atomic((std::string(DIRECTORY)+"/installed").c_str(),version))
            error = "安装已返回成功，但无法记录重启状态，请检查管理器";
    } else {
        unlink((std::string(DIRECTORY)+"/installed").c_str());
        net_curl_init();
        error = download(version,DIRECTORY,download_file,download_prefix,preview);
        net_curl_shutdown();
        if (error.empty()) error = validate_module_identity(std::string(DIRECTORY)+"/package.zip",version,channel,MODULE_VERSION_CODE);
        if (!error.empty()) unlink((std::string(DIRECTORY)+"/ready").c_str());
    }
    log_line("模块更新","%s",error.empty() ? (install ? "安装成功，请手动重启设备生效" : "下载完成，SHA-256 校验通过") : error.c_str());
    return error.empty() ? 0 : 1;
}
int status() {
    std::string ready = ready_version(DIRECTORY), installed;
    std::string channel = ready_channel(DIRECTORY);
    read_file((std::string(DIRECTORY)+"/installed").c_str(),installed);
    printf("{\"version\":%s,\"channel\":%s,\"path\":%s,\"rebootRequired\":%s}\n",
           source_json_string(ready).c_str(),source_json_string(channel).c_str(),source_json_string(ready.empty() ? "" : std::string(DIRECTORY)+"/package.zip").c_str(),
           !ready.empty() && ready==installed && upgrade_allowed(ready,MODULE_VERSION,MODULE_RELEASE_CHANNEL,channel) ? "true" : "false");
    return 0;
}
}
