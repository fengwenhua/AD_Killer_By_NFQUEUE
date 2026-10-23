#include "policy_barrier.h"
#include <cassert>
#include <future>
#include <thread>

int main() {
    PolicyBarrier policy;
    {
        PolicyBarrier::Read read(policy);
        assert(read.active());
        auto blocked = std::async(std::launch::async, [&] {
            PolicyBarrier::Change change(policy, 20);
            return change.ok();
        });
        assert(!blocked.get()); // old verdict must finish before cutover
    }
    std::atomic<bool> attempted{false};
    std::atomic<size_t> bypass{0}, classified{0};
    {
        PolicyBarrier::Change change(policy);
        assert(change.ok());
        std::thread worker([&] {
            for (int i = 0; i < 10000; ++i) {
                PolicyBarrier::Read read(policy);
                if (read.active()) ++classified;
                else ++bypass;
            }
            attempted.store(true);
        });
        worker.join();
        assert(attempted && bypass == 10000 && classified == 0);
        auto epoch = change.seal();
        policy.observe_idle(0, epoch - 1);
        policy.observe_idle(1, epoch);
        assert(!policy.drained(epoch)); // stale EAGAIN is not an ACK
        policy.observe_idle(0, epoch);
        assert(policy.drained(epoch));
        auto next = change.seal();
        assert(!policy.drained(next)); // both workers must acknowledge again
        policy.observe_idle(0, next);
        assert(!policy.drained(next));
        policy.observe_idle(1, next);
        assert(policy.drained(next));
        change.resume();
    }
    { PolicyBarrier::Read read(policy); assert(read.active()); }
    try {
        PolicyBarrier::Change change(policy);
        assert(change.ok());
        throw 1;
    } catch (int) {}
    { PolicyBarrier::Read read(policy); assert(!read.active()); }
    // Only the owner may recover from an unconfirmed/exceptional cutover.
    { PolicyBarrier::Change change(policy); assert(change.ok()); change.resume(); }
    { PolicyBarrier::Read read(policy); assert(read.active()); }
}
