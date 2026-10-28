#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

// CLI scopes. Isolated download workers can share a lock-free mmap ledger.
// A source/retry shares every enclosing task's
// monotonic deadline and accepted-body allowance. No wall-clock dependence.
class WorkBudget {
public:
    using Clock = std::chrono::steady_clock;
    inline static thread_local WorkBudget *active = nullptr;
    WorkBudget(uint64_t milliseconds, uint64_t bytes, bool shared = false)
        : parent_(active), deadline_(Clock::now() + std::chrono::milliseconds(
              std::min<uint64_t>(milliseconds, 86400000))), limit_(bytes) {
        if (shared) {
            void *memory = mmap(nullptr, sizeof(State), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            if (memory == MAP_FAILED) deadline_ = Clock::now();
            else { state_ = new (memory) State; shared_ = true; }
        }
        active = this;
    }
    ~WorkBudget() { active = parent_; if (shared_) { state_->~State(); munmap(state_, sizeof(State)); } }
    WorkBudget(const WorkBudget &) = delete;
    WorkBudget &operator=(const WorkBudget &) = delete;
    static uint64_t remaining_ms(uint64_t maximum) {
        const auto now = Clock::now();
        for (auto *scope = active; scope; scope = scope->parent_) {
            if (scope->state_->cancelled.load() || now >= scope->deadline_) return 0;
            maximum = std::min(maximum, static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(scope->deadline_ - now).count()));
        }
        return maximum;
    }
    static uint64_t remaining_bytes(uint64_t maximum) {
        for (auto *scope = active; scope; scope = scope->parent_)
            maximum = std::min(maximum, scope->limit_ - scope->state_->used.load());
        return maximum;
    }
    static bool expired() { return active && remaining_ms(UINT64_MAX) == 0; }
    void cancel() { state_->cancelled.store(true); }

    // Must not outlive the enclosing scopes. Unreported exec transfers keep
    // their reservation. Streaming library writes charge shared credit before
    // each write and never refund already consumed bytes, even after a crash.
    class Reservation {
    public:
        explicit Reservation(uint64_t wanted, bool streaming = false) : scope_(active) {
            for (auto *scope = scope_; scope; scope = scope->parent_) streaming_ |= streaming && scope->shared_;
            if (streaming_) { amount_ = remaining_bytes(wanted); return; }
            while (!expired()) {
                amount_ = remaining_bytes(wanted);
                uint64_t possible = wanted;
                bool pending = false;
                for (auto *scope = scope_; scope; scope = scope->parent_) {
                    const auto used = scope->state_->used.load(), reserved = scope->state_->reserved.load();
                    pending |= reserved != 0;
                    possible = std::min(possible, scope->limit_ - (used > reserved ? used - reserved : 0));
                }
                // Exec downloads reserve their complete possible file; wait for
                // concurrent transfers to refund unused credit, never reject a
                // source just because its peers temporarily reserved it.
                if (pending && amount_ < possible) { usleep(2000); continue; }
                for (auto *scope = scope_; scope; scope = scope->parent_) scope->state_->reserved.fetch_add(amount_);
                if (charge(scope_, amount_)) return;
                for (auto *scope = scope_; scope; scope = scope->parent_) scope->state_->reserved.fetch_sub(amount_);
            }
            amount_ = 0;
        }
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        ~Reservation() {
            if (!settled_ && !streaming_)
                for (auto *scope = scope_; scope; scope = scope->parent_) scope->state_->reserved.fetch_sub(amount_);
        }
        void settle(uint64_t used) {
            if (settled_ || used > amount_) return;
            if (streaming_) {
                // The isolated library child already charged shared scopes at
                // each write. Account its confirmed bytes in our local scopes.
                for (auto *scope = scope_; scope; scope = scope->parent_)
                    if (!scope->shared_) scope->state_->used.fetch_add(used);
            } else for (auto *scope = scope_; scope; scope = scope->parent_) {
                scope->state_->used.fetch_sub(amount_ - used);
                scope->state_->reserved.fetch_sub(amount_);
            }
            settled_ = true;
        }
        uint64_t amount() const { return amount_; }
        bool consume(uint64_t bytes) {
            if (!streaming_) return true;
            if (bytes > amount_ - streamed_ || !charge(scope_, bytes)) return false;
            streamed_ += bytes; return true;
        }
    private:
        WorkBudget *scope_;
        uint64_t amount_ = 0, streamed_ = 0;
        bool settled_ = false, streaming_ = false;
    };
private:
    struct State { std::atomic<uint64_t> used{0}, reserved{0}; std::atomic<bool> cancelled{false}; };
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "shared download credits need lock-free atomics");
    static_assert(std::atomic<bool>::is_always_lock_free, "shared cancellation needs lock-free atomics");
    static bool charge(WorkBudget *scope, uint64_t amount) {
        if (!scope) return true;
        auto used = scope->state_->used.load();
        do { if (amount > scope->limit_ - used) return false; }
        while (!scope->state_->used.compare_exchange_weak(used, used + amount));
        if (charge(scope->parent_, amount)) return true;
        scope->state_->used.fetch_sub(amount); return false;
    }
    WorkBudget *parent_;
    Clock::time_point deadline_;
    uint64_t limit_;
    State local_;
    State *state_ = &local_;
    bool shared_ = false;
};
