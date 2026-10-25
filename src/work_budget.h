#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

// Synchronous CLI scopes. A source/retry shares every enclosing task's
// monotonic deadline and accepted-body allowance. No wall-clock dependence.
class WorkBudget {
public:
    using Clock = std::chrono::steady_clock;
    inline static thread_local WorkBudget *active = nullptr;
    WorkBudget(uint64_t milliseconds, uint64_t bytes)
        : parent_(active), deadline_(Clock::now() + std::chrono::milliseconds(
              std::min<uint64_t>(milliseconds, 86400000))), limit_(bytes) { active = this; }
    ~WorkBudget() { active = parent_; }
    WorkBudget(const WorkBudget &) = delete;
    WorkBudget &operator=(const WorkBudget &) = delete;
    static uint64_t remaining_ms(uint64_t maximum) {
        const auto now = Clock::now();
        for (auto *scope = active; scope; scope = scope->parent_) {
            if (now >= scope->deadline_) return 0;
            maximum = std::min(maximum, static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(scope->deadline_ - now).count()));
        }
        return maximum;
    }
    static uint64_t remaining_bytes(uint64_t maximum) {
        for (auto *scope = active; scope; scope = scope->parent_)
            maximum = std::min(maximum, scope->limit_ - scope->used_);
        return maximum;
    }
    static bool expired() { return active && remaining_ms(UINT64_MAX) == 0; }

    // Must not outlive the enclosing scopes. Unreported/killed transfers keep
    // their whole reservation; only confirmed accounting refunds unused bytes.
    class Reservation {
    public:
        explicit Reservation(uint64_t wanted) : scope_(active), amount_(remaining_bytes(wanted)) {
            for (auto *scope = scope_; scope; scope = scope->parent_) scope->used_ += amount_;
        }
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        void settle(uint64_t used) {
            if (settled_ || used > amount_) return;
            for (auto *scope = scope_; scope; scope = scope->parent_) scope->used_ -= amount_ - used;
            settled_ = true;
        }
        uint64_t amount() const { return amount_; }
    private:
        WorkBudget *scope_;
        uint64_t amount_;
        bool settled_ = false;
    };
private:
    WorkBudget *parent_;
    Clock::time_point deadline_;
    uint64_t limit_, used_ = 0;
};
