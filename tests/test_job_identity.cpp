#include "update_job.h"
#include "exclusive_file_lock.h"
#include "task_context.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void put(const std::string &path, const std::string &text) { std::ofstream file(path); file << text; check(file.good(), "fixture write"); }
struct Fixture {
    std::string root, proc, jobs;
    Fixture() {
        char name[] = "/tmp/adk-jobs.XXXXXX"; auto *created = mkdtemp(name); check(created, "mkdtemp");
        root = created; proc = root + "/proc"; jobs = root + "/jobs";
        fs::create_directories(proc + "/sys/kernel/random"); fs::create_directory(proc + "/42"); fs::create_directory(jobs);
        put(proc + "/sys/kernel/random/boot_id", "12345678-1234-1234-1234-123456789abc\n");
        put(root + "/executable", "module image"); check(!symlink((root + "/executable").c_str(), (proc + "/42/exe").c_str()), "exe link");
        start(987);
    }
    ~Fixture() { fs::remove_all(root); }
    void start(unsigned ticks) {
        std::string stat = "42 (worker with ) spaces) S";
        for (int field = 4; field <= 21; ++field) stat += " 0";
        put(proc + "/42/stat", stat + " " + std::to_string(ticks) + " 0\n");
    }
    UpdateJobState state() {
        UpdateJobState state; state.legacy = false; state.id = "123-456"; state.type = "source";
        state.target = "https://example.org/rules?a=b"; state.channel = "stable"; state.phase = "downloading";
        state.status = "running"; state.created_at = 100; state.updated_at = 101;
        check(capture_update_job_owner(state, 42, proc), "capture synthetic identity"); return state;
    }
    std::string base(const std::string &id = "123-456") { return jobs + "/update-" + id; }
};
void identity_and_corruption() {
    Fixture f; auto state = f.state(); const auto journal = serialize_update_job(state);
    UpdateJobState parsed;
    check(!journal.empty() && parse_update_job_state(journal, parsed) && !parsed.legacy && parsed.target == state.target, "v2 roundtrip");
    check(update_job_life(parsed, state.id, f.proc) == UpdateJobLife::ALIVE, "boot/PID/start/executable all match");
    check(update_job_life(parsed, "456-789", f.proc) == UpdateJobLife::INTERRUPTED, "renamed record cannot impersonate another job");
    f.start(988); check(update_job_life(parsed, state.id, f.proc) == UpdateJobLife::INTERRUPTED, "reused PID rejected"); f.start(987);
    put(f.proc + "/sys/kernel/random/boot_id", "12345678-1234-1234-1234-123456789abd\n");
    check(update_job_life(parsed, state.id, f.proc) == UpdateJobLife::INTERRUPTED, "reboot cannot revive stale PID");
    put(f.proc + "/sys/kernel/random/boot_id", "12345678-1234-1234-1234-123456789abc\n");
    put(f.proc + "/sys/kernel/random/boot_id", "\n");
    check(update_job_life(parsed, state.id, f.proc) == UpdateJobLife::UNKNOWN, "malformed boot observation is not a verified reboot");
    put(f.proc + "/sys/kernel/random/boot_id", "12345678-1234-1234-1234-123456789abc\n");
    fs::rename(f.root + "/executable", f.root + "/old-image"); put(f.root + "/executable", "unrelated");
    check(update_job_life(parsed, state.id, f.proc) == UpdateJobLife::INTERRUPTED, "same PID executing unrelated image rejected");
    fs::remove(f.root + "/executable"); fs::rename(f.root + "/old-image", f.root + "/executable");
    fs::remove(f.proc + "/42/stat"); check(update_job_life(parsed, state.id, f.proc) == UpdateJobLife::UNKNOWN, "unreadable proc is unknown"); f.start(987);
    for (const auto &bad : {journal + "id=456-789\n", journal.substr(0, journal.size() - 1), std::string(9000, 'x'), std::string("ADK_JOB_2\nid=123-456\n")})
        check(!parse_update_job_state(bad, parsed), "truncated, duplicate, oversized or incomplete journal rejected");
    state.target = "url\nstatus=success"; check(serialize_update_job(state).empty(), "metadata cannot inject fields");
    check(parse_update_job_state("running:42\n", parsed) && update_job_life(parsed, "123-456", f.proc) == UpdateJobLife::INTERRUPTED, "legacy PID alone never proves alive");
}
void leases_and_prune() {
    Fixture f; auto state = f.state(); put(f.base() + ".state", serialize_update_job(state));
    check(observe_update_job(f.jobs, state.id, f.proc).status == "unverified", "missing v2 lease is unknown");
    {
        ExclusiveFileLock lease((f.base() + ".lease").c_str()); check(lease.ok(), "worker lease");
        check(observe_update_job(f.jobs, state.id, f.proc).status == "running", "exact owner with held lease is running");
        f.start(988);
        auto observation = observe_update_job(f.jobs, state.id, f.proc);
        check(observation.status == "unverified" && observation.lease == 1, "orphaned descendant lease is not quiescent");
        check(prune_update_jobs(f.jobs, 0, f.proc) == 0 && fs::exists(f.base() + ".state"), "held lease never pruned");
        state.status = "success"; state.exit_code = 0; state.phase = "finished";
        put(f.base() + ".state", serialize_update_job(state));
        observation = observe_update_job(f.jobs, state.id, f.proc);
        check(observation.status == "running" && observation.state.phase == "finishing", "terminal journal waits for all lease holders");
        state.status = "running"; state.exit_code = -1; state.phase = "installing";
        put(f.base() + ".state", serialize_update_job(state));
    }
    auto uncertain = observe_update_job(f.jobs, state.id, f.proc);
    check(uncertain.status == "unverified" && uncertain.reason == "installer_completion_unconfirmed", "dead installing owner is not proof the installer exited");
    put(f.proc + "/sys/kernel/random/boot_id", "12345678-1234-1234-1234-123456789abd\n");
    check(observe_update_job(f.jobs, state.id, f.proc).status == "interrupted", "a verified reboot ends the old installer instance");
    check(prune_update_jobs(f.jobs, 0, f.proc) == 1 && !fs::exists(f.base() + ".lease"), "quiescent interrupted job bounded by retention");
    f.start(987); state.phase = "downloading";
    put(f.base() + ".state", serialize_update_job(state)); put(f.base() + ".lease", "");
    put(f.jobs + "/.active-rules", state.id + "\n");
    check(prune_update_jobs(f.jobs, 0) == 0, "backend recovery index pins its record"); fs::remove(f.jobs + "/.active-rules");
    fs::remove(f.base() + ".state");
    check(!mkfifo((f.base() + ".state").c_str(), 0600), "FIFO fixture");
    check(observe_update_job(f.jobs, state.id, f.proc).status == "unverified", "FIFO read is bounded/nonblocking"); fs::remove(f.base() + ".state");
    put(f.root + "/foreign", "success:0\n");
    check(!symlink((f.root + "/foreign").c_str(), (f.base() + ".state").c_str()), "state symlink");
    check(observe_update_job(f.jobs, state.id, f.proc).status == "unverified" && prune_update_jobs(f.jobs, 0) == 0, "symlink not read or removed");
}
}
int main() {
    try { identity_and_corruption(); leases_and_prune(); }
    catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    std::cout << "job identities: boot/PID/start/image/job binding, bounded corruption, unknown descendants and safe retention passed\n";
}
