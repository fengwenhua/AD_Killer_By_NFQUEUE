#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>

// A packet either holds a read lease for the complete verdict, or bypasses
// without a connmark. Never wait for a potentially slow firewall transaction
// on a receive worker: that would leave bound queues without consumers.
class PolicyBarrier {
public:
    class Read {
    public:
        explicit Read(PolicyBarrier &owner) : lock_(owner.mutex_, std::defer_lock) {
            active_ = !owner.changing_.load(std::memory_order_acquire) &&
                lock_.try_lock() && !owner.changing_.load(std::memory_order_acquire);
            if (!active_ && lock_.owns_lock()) lock_.unlock();
        }
        bool active() const { return active_; }
    private:
        std::shared_lock<std::shared_timed_mutex> lock_;
        bool active_ = false;
    };

    class Change {
    public:
        explicit Change(PolicyBarrier &owner, unsigned timeout_ms = 2000)
            : owner_(owner), lock_(owner.mutex_, std::defer_lock) {
            owner_.changing_.store(true, std::memory_order_release);
            if (!lock_.try_lock_for(std::chrono::milliseconds(timeout_ms)))
                owner_.changing_.store(false, std::memory_order_release);
        }
        ~Change() {
            // Unless resume() explicitly completed, exceptions/faults leave
            // consumers in bypass mode until the owner's protected shutdown.
            if (lock_.owns_lock()) lock_.unlock();
        }
        bool ok() const { return lock_.owns_lock(); }
        void resume() {
            if (!lock_.owns_lock()) return;
            lock_.unlock();
            owner_.changing_.store(false, std::memory_order_release);
        }
        // On unconfirmed cutover/rollback keep bypassing until owner shutdown.
        void fail_open() { owner_.changing_.store(true, std::memory_order_release); }
        uint64_t seal() { return owner_.epoch_.fetch_add(1, std::memory_order_acq_rel) + 1; }
    private:
        PolicyBarrier &owner_;
        std::unique_lock<std::shared_timed_mutex> lock_;
    };

    uint64_t epoch() const { return epoch_.load(std::memory_order_acquire); }
    // Pass the epoch sampled BEFORE recvmsg. An idle result from an earlier
    // receive call must not acknowledge a later policy commit.
    void observe_idle(unsigned queue, uint64_t epoch) {
        if (queue < idle_.size()) idle_[queue].store(epoch, std::memory_order_release);
    }
    bool drained(uint64_t epoch) const {
        return idle_[0].load(std::memory_order_acquire) >= epoch &&
               idle_[1].load(std::memory_order_acquire) >= epoch;
    }
private:
    std::shared_timed_mutex mutex_;
    std::atomic<bool> changing_{false};
    std::atomic<uint64_t> epoch_{0};
    std::array<std::atomic<uint64_t>, 2> idle_{{0, 0}};
};
