#include "update_job.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

}  // namespace

int main() {
    expect(valid_update_job_id("1730000000000000-1234"),
           "generated numeric job ID should be valid");
    expect(!valid_update_job_id("../../state") &&
               !valid_update_job_id("12;rm") &&
               !valid_update_job_id("-12"),
           "job ID must not permit path or shell metacharacters");

    UpdateJobState state;
    expect(parse_update_job_state("running:321\n", state) &&
               state.status == "running" && state.pid == 321,
           "running job state should preserve child PID");
    expect(parse_update_job_state("success:0\n", state) &&
               state.status == "success" && state.exit_code == 0,
           "zero exit should parse as success");
    expect(parse_update_job_state("failed:2\n", state) &&
               state.status == "failed" && state.exit_code == 2,
           "nonzero exit should parse as failure");
    expect(!parse_update_job_state("success:1", state) &&
               !parse_update_job_state("failed:0", state) &&
               !parse_update_job_state("running:-1", state),
           "contradictory or invalid states must be rejected");

    expect(running_update_job_state(42) == "running:42\n",
           "running state serialization should be stable");
    expect(finished_update_job_state(0) == "success:0\n" &&
               finished_update_job_state(7) == "failed:7\n",
           "finished state serialization should reflect exit status");

    char jobs_template[] = "/tmp/ad_killer_by_nfqueue_jobs.XXXXXX";
    char *jobs_dir = mkdtemp(jobs_template);
    expect(jobs_dir != nullptr, "temporary job directory should be created");
    if (jobs_dir) {
        std::string directory = jobs_dir;
        for (const char *id : {"100-1", "200-1", "300-1"}) {
            std::ofstream(directory + "/update-" + id + ".state") << "success:0\n";
            std::ofstream(directory + "/update-" + id + ".log") << id << "\n";
        }
        std::string running_id = "400-1";
        std::ofstream(directory + "/update-" + running_id + ".state")
            << running_update_job_state(static_cast<int>(getpid()));
        std::ofstream(directory + "/update-" + running_id + ".log") << "running\n";

        expect(prune_update_jobs(directory, 2) == 1,
               "pruning should remove finished jobs beyond retention count");
        expect(access((directory + "/update-100-1.state").c_str(), F_OK) != 0 &&
                   access((directory + "/update-200-1.state").c_str(), F_OK) == 0 &&
                   access((directory + "/update-300-1.state").c_str(), F_OK) == 0,
               "pruning should retain the newest finished jobs");
        expect(access((directory + "/update-400-1.state").c_str(), F_OK) == 0,
               "pruning must never remove a live running job");

        for (const char *id : {"100-1", "200-1", "300-1", "400-1"}) {
            unlink((directory + "/update-" + id + ".state").c_str());
            unlink((directory + "/update-" + id + ".log").c_str());
        }
        rmdir(directory.c_str());
    }

    if (failures != 0) {
        std::cerr << failures << " update job test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "update job tests passed\n";
    return EXIT_SUCCESS;
}
