#include "source_prefetch.h"
#include "builtin_sources.h"
#include "owned_child.h"
#include "task_context.h"
#include "logging.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

int sync_concurrency_value(const std::string &value) {
    if (value.empty() || value.size() > 2) return SYNC_CONCURRENCY_DEFAULT;
    int count = 0;
    for (char ch : value) { if (ch < '0' || ch > '9') return SYNC_CONCURRENCY_DEFAULT; count = count * 10 + ch - '0'; }
    return count >= 1 && count <= SYNC_CONCURRENCY_MAX ? count : SYNC_CONCURRENCY_DEFAULT;
}

namespace {
int setting_number(const std::string &text, int fallback, int maximum) {
    if (text.empty() || text.size() > 4) return fallback;
    int value = 0;
    for (char c : text) { if (c < '0' || c > '9') return fallback; value = value * 10 + c - '0'; }
    return value <= maximum ? value : fallback;
}
}
int sync_interval_value(const std::string &value) { return setting_number(value, SYNC_INTERVAL_DEFAULT, SYNC_INTERVAL_MAX); }
int sync_retries_value(const std::string &value) { return setting_number(value, SYNC_RETRIES_DEFAULT, SYNC_RETRIES_MAX); }

namespace {
struct Reply {
    uint64_t bytes = 0;
    long status = 0;
    bool not_modified = false, transient = false;
    char error[512]{}, url[4097]{}, etag[257]{}, modified[129]{};
};
struct Slot {
    std::atomic<bool> complete{false};
    bool unconfirmed = false;
    size_t count = 0;
    uint64_t elapsed_ms = 0;
    Reply replies[2];
};
template<size_t N> void copy(char (&out)[N], const std::string &text) { std::snprintf(out, N, "%s", text.c_str()); }
}

struct SourcePrefetch::Impl {
    const std::vector<std::string> &urls;
    std::string directory;
    SourceCache *cache;
    size_t concurrency, cursor = 0, active = 0;
    uint64_t source_ms;
    WorkBudget &task;
    int fresh_seconds;
    unsigned retries;
    std::set<std::string> retry_urls;
    SourcePrefetchProgress progress;
    Slot *slots = nullptr;
    struct Job { pid_t pid = -1; bool started = false, ready = false; unsigned attempts = 0; uint64_t elapsed_ms = 0, bytes = 0; SourceDownload download; };
    std::vector<Job> jobs;
    Impl(const std::vector<std::string> &u, const std::string &d, SourceCache *c, int n, uint64_t ms, WorkBudget &t,
         int fresh, unsigned retry_count, const std::set<std::string> &force, SourcePrefetchProgress notify)
        : urls(u), directory(d), cache(c), concurrency(std::max(1, std::min(n, SYNC_CONCURRENCY_MAX))), source_ms(ms), task(t),
          fresh_seconds(std::max(0, std::min(fresh, SYNC_INTERVAL_MAX * 60))), retries(std::min(retry_count, static_cast<unsigned>(SYNC_RETRIES_MAX))), retry_urls(force), progress(std::move(notify)), jobs(u.size()) {
        if (u.empty() || u.size() > 65) return;
        void *memory = mmap(nullptr, sizeof(Slot) * u.size(), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (memory != MAP_FAILED) {
            slots = static_cast<Slot *>(memory);
            for (size_t i = 0; i < u.size(); ++i) new (&slots[i]) Slot;
        }
    }
    ~Impl() {
        stop();
        if (slots) { for (size_t i = 0; i < urls.size(); ++i) slots[i].~Slot(); munmap(slots, sizeof(Slot) * urls.size()); }
    }
    bool launch(size_t i) {
        if (!task_progress("downloading") || WorkBudget::expired() || operation_closing()) return false;
        auto &job = jobs[i];
        job.download = prepare_source_download(directory, urls[i], cache, i * 2);
        job.download.elapsed_ms += job.elapsed_ms;
        job.download.prior_downloaded_bytes = job.bytes;
        job.download.managed_retries = true;
        if (!job.started && fresh_seconds > 0 && !retry_urls.count(urls[i]) && job.download.cached &&
            job.download.previous.fresh(time(nullptr), fresh_seconds)) {
            job.download.reuse_fresh_cache = true; job.download.attempt = 0;
            job.started = job.ready = true; ++active;
            if (progress) progress(i, true, 0);
            return true;
        }
        if (WorkBudget::expired() || operation_closing()) return false;
        job.download.attempt = ++job.attempts;
        slots[i].complete.store(false, std::memory_order_release);
        slots[i].unconfirmed = false; slots[i].count = 0;
        log_line("更新", "并发下载 %zu/%zu（第 %u 次）: %s", i + 1, urls.size(), job.attempts, urls[i].c_str());
        const auto parent = getpid();
        const auto pid = fork();
        if (pid < 0) return false;
        if (!pid) {
            if (setpgid(0, 0) || !inherit_task_lease()) _exit(127);
#if defined(__linux__)
            if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(127);
#else
            (void)parent;
#endif
            // The coordinator alone owns the durable job phase journal.
            task_progress_callback = {};
            // Job stdout is /dev/null: only the coordinator appends/rotates
            // the bounded job log. Foreground CLI keeps ordinary stdout logs.
            configure_process_log(nullptr, 0, 0);
            WorkBudget source(source_ms - std::min(source_ms, job.download.elapsed_ms),
                (is_builtin_source(urls[i]) ? 128u * 1024u * 1024u : MAX_RULE_INPUT_BYTES) - job.bytes);
            if (job.attempts > 1) {
                const auto waiting = WorkBudget::Clock::now();
                const auto until = WorkBudget::Clock::now() + std::chrono::milliseconds(250 * (job.attempts - 1));
                while (WorkBudget::Clock::now() < until && !WorkBudget::expired() && !operation_closing()) usleep(10000);
                job.download.elapsed_ms += std::chrono::duration_cast<std::chrono::milliseconds>(WorkBudget::Clock::now() - waiting).count();
            }
            try { fetch_source_download(urls[i], job.download, cache != nullptr); }
            catch (...) { _exit(127); }
            auto &slot = slots[i];
            slot.count = job.download.responses.size();
            if (slot.count > 2) _exit(127);
            slot.elapsed_ms = job.download.elapsed_ms;
            slot.unconfirmed = task_unconfirmed_network_writer;
            for (size_t j = 0; j < slot.count; ++j) {
                const auto &r = job.download.responses[j]; auto &wire = slot.replies[j];
                if (r.effective_url.size() >= sizeof(wire.url) || r.validators.etag.size() >= sizeof(wire.etag) ||
                    r.validators.last_modified.size() >= sizeof(wire.modified)) _exit(127);
                wire.bytes = r.bytes; wire.status = r.http_status; wire.not_modified = r.not_modified; wire.transient = r.transient;
                copy(wire.error, r.error.size() < sizeof(wire.error) ? r.error : "source_download_error");
                copy(wire.url, r.effective_url); copy(wire.etag, r.validators.etag); copy(wire.modified, r.validators.last_modified);
            }
            slot.complete.store(true, std::memory_order_release);
            _exit(0);
        }
        setpgid(pid, pid);
        job.pid = pid; job.started = true; ++active;
        if (progress) progress(i, false, job.attempts);
        return true;
    }
    bool next(size_t &index, SourceDownload &download) {
        if (!slots) { task.cancel(); return false; }
        while (active < concurrency && cursor < urls.size()) {
            if (!launch(cursor)) { task.cancel(); stop(); return false; }
            ++cursor;
        }
        while (active) {
            if (WorkBudget::expired() || operation_closing()) { stop(); return false; }
            for (size_t i = 0; i < jobs.size(); ++i) {
                auto &job = jobs[i];
                if (job.ready) {
                    job.ready = false; --active;
                    download = std::move(job.download); index = i; return true;
                }
                if (job.pid < 0) continue;
                siginfo_t info{};
                const int observed = waitid(P_PID, job.pid, &info, WEXITED | WNOHANG | WNOWAIT);
                if (!observed && !info.si_pid) continue;
                if (observed < 0 && errno == EINTR) continue;
                const auto finished = finish_owned_child(job.pid, observed == 0, getpgid(job.pid) == job.pid);
                job.pid = -1; --active;
                auto &slot = slots[i];
                const bool safe = finished.reaped && finished.group_gone && finished.exit_code == 0 &&
                    slot.complete.load(std::memory_order_acquire) && slot.count <= job.download.parts.size();
                if (!safe || slot.unconfirmed) task_unconfirmed_network_writer = true;
                download = std::move(job.download);
                if (safe && !slot.unconfirmed) {
                    download.elapsed_ms = slot.elapsed_ms;
                    for (size_t j = 0; j < slot.count; ++j) {
                        const auto &r = slot.replies[j];
                        DownloadResponse response;
                        response.bytes = r.bytes; response.http_status = r.status;
                        response.not_modified = r.not_modified; response.transient = r.transient;
                        response.error = r.error; response.effective_url = r.url;
                        response.validators = {r.etag, r.modified};
                        download.responses.push_back(std::move(response));
                    }
                } else { DownloadResponse error; error.error = "source_worker_unconfirmed"; download.responses.push_back(std::move(error)); }
                index = i; return true;
            }
            usleep(10000);
        }
        return false;
    }
    bool retry(size_t index, uint64_t elapsed_ms, uint64_t bytes) {
        if (index >= jobs.size() || jobs[index].pid >= 0 || jobs[index].ready ||
            jobs[index].attempts >= retries + 1 || elapsed_ms >= source_ms ||
            source_ms - elapsed_ms <= 250 * jobs[index].attempts + 100 ||
            bytes >= (is_builtin_source(urls[index]) ? 128u * 1024u * 1024u : MAX_RULE_INPUT_BYTES) ||
            task_unconfirmed_network_writer || WorkBudget::expired() || operation_closing()) return false;
        jobs[index].elapsed_ms = elapsed_ms;
        jobs[index].bytes = bytes;
        if (launch(index)) return true;
        task.cancel(); stop(); return false;
    }
    void stop() {
        if (!active) return;
        task.cancel(); // Visible to every isolated downloader and its watchdog.
        const auto deadline = WorkBudget::Clock::now() + std::chrono::seconds(3);
        for (size_t i = 0; i < jobs.size(); ++i) if (jobs[i].pid > 0) {
            auto &job = jobs[i];
            bool owned = true, exited = false;
            while (WorkBudget::Clock::now() < deadline) {
                siginfo_t info{};
                if (waitid(P_PID, job.pid, &info, WEXITED | WNOHANG | WNOWAIT)) { if (errno == EINTR) continue; owned = false; break; }
                if (info.si_pid) { exited = true; break; }
                usleep(10000);
            }
            const auto finished = finish_owned_child(job.pid, owned, getpgid(job.pid) == job.pid);
            if (!exited || !finished.reaped || !finished.group_gone || slots[i].unconfirmed) task_unconfirmed_network_writer = true;
            job.pid = -1;
        }
        active = 0;
    }
};
SourcePrefetch::SourcePrefetch(const std::vector<std::string> &urls, const std::string &directory,
    SourceCache *cache, int concurrency, uint64_t source_ms, WorkBudget &task,
    int fresh_seconds, unsigned retries, const std::set<std::string> &retry_urls, SourcePrefetchProgress progress)
    : impl_(std::make_unique<Impl>(urls, directory, cache, concurrency, source_ms, task,
        fresh_seconds, retries, retry_urls, std::move(progress))) {}
SourcePrefetch::~SourcePrefetch() = default;
bool SourcePrefetch::next(size_t &index, SourceDownload &download) { return impl_->next(index, download); }
void SourcePrefetch::stop() { impl_->stop(); }
bool SourcePrefetch::started(size_t index) const { return index < impl_->jobs.size() && impl_->jobs[index].started; }
bool SourcePrefetch::retry(size_t index, uint64_t elapsed_ms, uint64_t bytes) { return impl_->retry(index, elapsed_ms, bytes); }
