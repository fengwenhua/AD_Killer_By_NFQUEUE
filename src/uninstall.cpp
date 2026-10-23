#include "uninstall.h"
#include "operation_gate.h"
#include "safe_tree.h"
#include "update_job.h"
#include "cli_utils.h"
#include "exclusive_file_lock.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
struct Descriptor {
    int fd = -1;
    explicit Descriptor(int value = -1) : fd(value) {}
    ~Descriptor() { if (fd >= 0) close(fd); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
};
bool identity(const struct stat &a, const struct stat &b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
bool owned(int fd, bool directory = false) {
    struct stat st {};
    return fd >= 0 && !fstat(fd, &st) && st.st_uid == geteuid() && !(st.st_mode & 0022) &&
        (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode) && st.st_nlink == 1);
}
bool installation_idle(const std::string &data, std::string &error) {
    const auto path = data + "/update_jobs";
    Descriptor fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
    if (fd.fd < 0) { if (errno == ENOENT) return true; error = "无法核实后台任务目录"; return false; }
    struct stat st {};
    if (fstat(fd.fd, &st) || st.st_uid != geteuid()) { error = "任务目录归属异常"; return false; }
    DIR *directory = fdopendir(dup(fd.fd));
    if (!directory) { error = "无法枚举后台任务"; return false; }
    bool ok = true; unsigned count = 0;
    while (ok) {
        errno = 0; const auto *entry = readdir(directory);
        if (!entry) { if (errno) { error = "后台任务枚举不完整"; ok = false; } break; }
        const std::string name = entry->d_name;
        if (++count > 4096) { error = "后台任务数量超过核验上限"; ok = false; break; }
        if (name.compare(0, 7, "update-") || name.size() < 13 || name.compare(name.size() - 6, 6, ".state")) continue;
        const auto id = name.substr(7, name.size() - 13);
        const auto observed = observe_update_job(path, id);
        if (observed.status == "invalid_job" || observed.status == "invalid_state" || observed.status == "not_found" ||
            observed.state.status.empty() || (observed.state.legacy && observed.state.status == "running")) {
            error = "后台任务记录无法核实，未关闭入口或清理数据"; ok = false; break;
        }
        if (observed.reason == "installer_completion_unconfirmed" ||
            (observed.state.status == "running" && observed.state.phase == "installing" && observed.status != "interrupted")) {
            error = "模块安装尚未完成或结果未知，请待管理器完成后重试卸载；不会强杀安装器"; ok = false; break;
        }
    }
    closedir(directory); return ok;
}
bool no_data_mounts(const std::string &data, std::string &error) {
    std::ifstream input("/proc/self/mountinfo");
    if (!input) { error = "无法检查数据目录挂载边界"; return false; }
    std::string line; size_t total = 0;
    while (std::getline(input, line)) {
        if ((total += line.size()) > 4 * 1024 * 1024 || line.size() > 65536) { error = "挂载表超过核验上限"; return false; }
        std::istringstream row(line); std::string id, parent, device, root, point;
        if (!(row >> id >> parent >> device >> root >> point)) { error = "挂载表格式无法核实"; return false; }
        // The fixed data path has no whitespace/backslash, and mountinfo only
        // escapes those characters. Bind mounts are rejected even on same dev.
        if (point == data || point.compare(0, data.size() + 1, data + "/") == 0) {
            error = "数据目录内存在挂载点，未递归删除"; return false;
        }
    }
    if (!input.eof()) { error = "挂载表读取不完整"; return false; }
    return true;
}
// Covers old module images/tools that predate operation leases. Observation
// only: never send signals using a PID discovered here. Unknown => retain data.
bool legacy_writers_gone(const std::string &data, std::string &error) {
    DIR *processes = opendir("/proc");
    if (!processes) { error = "无法检查旧进程"; return false; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    unsigned processes_seen = 0, descriptors_seen = 0; bool ok = true;
    while (ok) {
        errno = 0; const auto *entry = readdir(processes);
        if (!entry) { if (errno) { error = "进程枚举不完整"; ok = false; } break; }
        const std::string pid = entry->d_name;
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos || pid == std::to_string(getpid())) continue;
        if (++processes_seen > 8192 || std::chrono::steady_clock::now() >= deadline) { error = "旧进程核验超过预算"; ok = false; break; }
        const auto base = "/proc/" + pid;
        char image[4096];
        const auto n = readlink((base + "/exe").c_str(), image, sizeof(image));
        if (n < 0 && errno != ENOENT) { error = "旧进程映像不可读取"; ok = false; break; }
        std::string executable = n > 0 ? std::string(image, static_cast<size_t>(n)) : "";
        if (n == static_cast<ssize_t>(sizeof(image))) { error = "进程映像路径超过上限"; ok = false; break; }
        const auto slash = executable.rfind('/');
        const auto name = executable.substr(slash == std::string::npos ? 0 : slash + 1);
        if (name == "AD_Killer_By_NFQUEUE" || name == "AD_Killer_By_NFQUEUE (deleted)" || name == "AD_Killer_By_NFQUEUEd") {
            error = "仍有本模块旧进程或查询在运行，请等待后重试"; ok = false; break;
        }
        std::string arguments;
        const auto read = read_regular_bounded(base + "/cmdline", arguments, 65536);
        if (read != BoundedRead::OK && read != BoundedRead::MISSING) { error = "旧进程参数不可核实"; ok = false; break; }
        if (arguments.find(data + "/") != std::string::npos) { error = "仍有进程引用模块数据，未删除"; ok = false; break; }
        DIR *files = opendir((base + "/fd").c_str());
        if (!files) { if (errno == ENOENT) continue; error = "旧进程文件句柄不可核实"; ok = false; break; }
        while (ok) {
            errno = 0; const auto *file = readdir(files);
            if (!file) { if (errno) { error = "文件句柄枚举不完整"; ok = false; } break; }
            if (file->d_name[0] == '.') continue;
            if (++descriptors_seen > 131072 || std::chrono::steady_clock::now() >= deadline) { error = "文件句柄核验超过预算"; ok = false; break; }
            char target[4096];
            const auto length = readlink((base + "/fd/" + file->d_name).c_str(), target, sizeof(target));
            if (length < 0) { if (errno == ENOENT) continue; error = "文件句柄不可读取"; ok = false; break; }
            if (length == static_cast<ssize_t>(sizeof(target))) { error = "文件句柄路径超过上限"; ok = false; break; }
            const std::string path(target, static_cast<size_t>(length));
            if (path.compare(0, data.size() + 1, data + "/")) continue;
            std::string info;
            if (read_regular_bounded(base + "/fdinfo/" + file->d_name, info, 8192) != BoundedRead::OK) {
                // The descriptor may have just closed. Retry the inspection,
                // rather than treating an unreadable writer as safely gone.
                error = "数据句柄访问模式未能核实，请重试"; ok = false; break;
            }
            std::istringstream rows(info); std::string key, value; bool flags = false;
            while (rows >> key >> value) {
                if (key != "flags:") continue;
                errno = 0; char *end = nullptr; const auto bits = strtoul(value.c_str(), &end, 8);
                if (errno || !end || *end || value.empty() || value.find_first_not_of("01234567") != std::string::npos) break;
                flags = true;
                if ((bits & O_ACCMODE) != O_RDONLY) { error = "旧进程仍持有数据写入句柄"; ok = false; }
                break;
            }
            if (!flags) { error = "数据句柄访问模式无效"; ok = false; }
        }
        closedir(files);
    }
    closedir(processes); return ok;
}
bool named_inode(int parent, const std::string &name, int fd) {
    struct stat opened {}, named {};
    return !fstat(fd, &opened) && !fstatat(parent, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) && identity(opened, named);
}
int gate_open(int parent, bool create) {
    int fd = openat(parent, ".AD_Killer_By_NFQUEUE.operations.lock",
        O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (create ? O_CREAT : 0), 0600);
    if (fd >= 0 && fd <= STDERR_FILENO) { const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3); close(fd); fd = moved; }
    if (!owned(fd)) { if (fd >= 0) close(fd); return -1; } return fd;
}
}

UninstallResult uninstall_module(const std::string &data, const std::string &module,
        const std::function<bool()> &stop, const std::function<bool()> &verify) {
    UninstallResult result;
    const auto fail = [&](const std::string &message, const char *status = "uninstall_incomplete") {
        result.status = status; result.message = message; return result;
    };
    if (geteuid() != 0 || data != "/data/adb/AD_Killer_By_NFQUEUE" || module != "/data/adb/modules/AD_Killer_By_NFQUEUE")
        return fail("完整卸载仅支持 root 和固定模块目录");
    Descriptor parent(open("/data/adb", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
    Descriptor module_fd(open(module.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
    if (!owned(parent.fd, true) || !owned(module_fd.fd, true) || flock(module_fd.fd, LOCK_EX | LOCK_NB))
        return fail("模块目录无法核实或其他卸载仍在运行");
    std::string prop;
    Descriptor prop_fd(openat(module_fd.fd, "module.prop", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!owned(prop_fd.fd) || read_regular_bounded(module + "/module.prop", prop, 8192) != BoundedRead::OK ||
        ("\n" + prop + "\n").find("\nid=AD_Killer_By_NFQUEUE\n") == std::string::npos)
        return fail("模块身份文件不可核实，未删除数据");
    Descriptor shared(gate_open(parent.fd, true)), exclusive(gate_open(parent.fd, false));
    if (shared.fd < 0 || exclusive.fd < 0 || flock(shared.fd, LOCK_SH | LOCK_NB) ||
        !named_inode(parent.fd, ".AD_Killer_By_NFQUEUE.operations.lock", shared.fd)) return fail("无法核实操作租约");
    // No RAII unlock: a descendant must retain its independent open-file lease.
    struct Context {
        int old_fd = operation_lease_fd; std::string old_marker = operation_marker_path;
        ~Context() { operation_lease_fd = old_fd; operation_marker_path = old_marker; }
    } context;
    operation_lease_fd = shared.fd; operation_marker_path = module + "/.uninstalling";
    struct stat original_data {};
    const bool existed = !fstatat(parent.fd, "AD_Killer_By_NFQUEUE", &original_data, AT_SYMLINK_NOFOLLOW);
    if (!existed && errno != ENOENT) return fail("无法核实数据目录");
    if (existed && (!S_ISDIR(original_data.st_mode) || original_data.st_uid != geteuid())) return fail("数据目录归属或类型异常");
    std::string error;
    if (!no_data_mounts(data, error)) return fail(error);
    const auto close_admission = [&] {
        Descriptor created(openat(module_fd.fd, ".uninstalling", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (created.fd < 0) return errno == EEXIST;
        const std::string text = "ADK_UNINSTALL_1\n";
        size_t offset = 0;
        while (offset < text.size()) {
            const auto count = write(created.fd, text.data() + offset, text.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                // No stop/delete has begun. Roll back only our own incomplete
                // marker, so a short write does not permanently strand retry.
                if (named_inode(module_fd.fd, ".uninstalling", created.fd)) unlinkat(module_fd.fd, ".uninstalling", 0);
                fsync(module_fd.fd);
                return false;
            }
            offset += static_cast<size_t>(count);
        }
        return true;
    };
    if (existed) {
        ExclusiveFileLock admission((data + "/.job-admission.lock").c_str(), true, 1000);
        if (!admission.ok() || !installation_idle(data, error)) return fail(error.empty() ? "任务入口繁忙" : error, "uninstall_busy");
        // The installer takes this same admission lock before its durable
        // installing transition. It must observe this marker before dispatch.
        if (!close_admission()) return fail("无法关闭模块写入入口，请检查存储后重试");
    } else if (!close_admission()) return fail("无法关闭模块写入入口，请检查存储后重试");
    Descriptor marker(openat(module_fd.fd, ".uninstalling", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    std::string marker_text;
    if (!owned(marker.fd) || read_regular_bounded(operation_marker_path, marker_text, 128) != BoundedRead::OK || marker_text != "ADK_UNINSTALL_1\n")
        return fail("卸载关闭标记无法核实，保留数据");
    if (fsync(marker.fd) || fsync(module_fd.fd)) return fail("关闭标记未能持久化，未停止守护或删除数据；修复存储后重试");
    // Idempotent retry may need transient control files to verify/clean routes.
    // Admission is already closed and these are removed with the data tree.
    if (!existed && mkdirat(parent.fd, "AD_Killer_By_NFQUEUE", 0755) && errno != EEXIST) return fail("无法建立清理控制目录");
    if (!stop()) return fail("守护、双栈路由或队列尚未核实清理；入口保持关闭，保留数据，请检查诊断后重试");
    close(shared.fd); shared.fd = -1; operation_lease_fd = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (flock(exclusive.fd, LOCK_EX | LOCK_NB)) {
        if ((errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) || std::chrono::steady_clock::now() >= deadline)
            return fail("仍有已接受的后台写入或租约未释放，未删除数据", "uninstall_busy");
        usleep(10000);
    }
    if (!named_inode(parent.fd, ".AD_Killer_By_NFQUEUE.operations.lock", exclusive.fd) || !installation_idle(data, error))
        return fail(error.empty() ? "操作租约映像已改变" : error);
    if (!verify()) return fail("后台写入者排空后，进程/双栈路由/队列复查尚未通过；保留数据，请重试清理");
    if (!legacy_writers_gone(data, error) || !no_data_mounts(data, error)) return fail(error);
    Descriptor install(openat(parent.fd, ".AD_Killer_By_NFQUEUE.install.lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if ((install.fd < 0 && errno != ENOENT) || (install.fd >= 0 && (!owned(install.fd) || flock(install.fd, LOCK_EX | LOCK_NB))))
        return fail("安装锁仍被占用或无法核实，保留数据");
    SafeTreeRemoval tree;
    if (!tree.prepare(parent.fd, "AD_Killer_By_NFQUEUE", error)) return fail(error);
    struct stat current_module {}, opened_module {}, current_data {};
    if (stat(module.c_str(), &current_module) || fstat(module_fd.fd, &opened_module) || !identity(current_module, opened_module) ||
        !named_inode(module_fd.fd, ".uninstalling", marker.fd) ||
        (existed && (fstatat(parent.fd, "AD_Killer_By_NFQUEUE", &current_data, AT_SYMLINK_NOFOLLOW) || !identity(current_data, original_data))) ||
        !no_data_mounts(data, error)) return fail("模块/数据身份或挂载边界发生变化，未执行删除");
    if (!tree.remove(error)) return fail(error);
    if (install.fd >= 0 && (!named_inode(parent.fd, ".AD_Killer_By_NFQUEUE.install.lock", install.fd) ||
        unlinkat(parent.fd, ".AD_Killer_By_NFQUEUE.install.lock", 0))) return fail("数据已删除，安装锁清理未完成");
    if (!named_inode(parent.fd, ".AD_Killer_By_NFQUEUE.operations.lock", exclusive.fd) ||
        unlinkat(parent.fd, ".AD_Killer_By_NFQUEUE.operations.lock", 0) || fsync(parent.fd))
        return fail("数据已删除，操作锁清理或父目录同步未能确认");
    result.ok = true; result.status = "uninstalled";
    result.message = "本模块进程、队列与路由已清理，全部数据已删除；卸载标记保留至管理器移除模块目录";
    return result;
}
