#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

struct AsyncLogStats {
    uint64_t enqueued = 0, written = 0, dropped = 0, write_failures = 0, batches = 0;
    uint64_t pending = 0, pending_bytes = 0;
    bool active = false;
};

// One owner starts/stops, any producer may enqueue. Producers allocate nothing,
// never wait on this mutex and never call the sink. Loss includes records whose
// write could not be confirmed (a failed append may have partially written).
class AsyncLog {
public:
    static constexpr size_t CAPACITY = 64 * 1024, RECORDS = 1024, MAX_LINE = 640, BATCH = 16 * 1024;
    using Sink = std::function<bool(const char *, size_t)>;
    ~AsyncLog() { stop(); }
    bool start(Sink sink) {
        if (worker_.joinable() || !sink) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        read_ = write_ = head_ = count_ = bytes_ = 0;
        enqueued_ = written_ = dropped_ = failures_ = batches_ = pending_ = pending_bytes_ = 0;
        stopping_ = false; sink_ = std::move(sink);
        try { worker_ = std::thread([this] { run(); }); }
        catch (...) { accepting_ = false; failures_ = 1; return false; }
        accepting_.store(true, std::memory_order_release);
        return true;
    }
    bool enqueue(const char *data, size_t size) {
        if (!data || !size || size > MAX_LINE || !accepting_.load(std::memory_order_acquire)) return drop();
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return drop();
        if (!accepting_.load(std::memory_order_relaxed) || count_ == RECORDS || size > CAPACITY - bytes_) return drop();
        const size_t first = std::min(size, CAPACITY - write_);
        std::memcpy(buffer_.data() + write_, data, first);
        std::memcpy(buffer_.data(), data + first, size - first);
        write_ = (write_ + size) % CAPACITY;
        lengths_[(head_ + count_) % RECORDS] = static_cast<uint16_t>(size);
        ++count_; bytes_ += size; ++enqueued_;
        pending_ = count_; pending_bytes_ = bytes_;
        const bool ready = bytes_ >= BATCH || count_ >= RECORDS / 2;
        lock.unlock();
        if (ready) wake_.notify_one();
        return true;
    }
    void stop() {
        if (!worker_.joinable()) return;
        accepting_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            drain_deadline_ = Clock::now() + std::chrono::milliseconds(250);
        }
        wake_.notify_one();
        // An in-flight regular-file syscall may be uninterruptible. The owner
        // calls this only AFTER listeners/routes release, and retains its
        // operation lease until exit. Never detach a writer over deleted data.
        worker_.join();
    }
    AsyncLogStats stats() const {
        return {enqueued_.load(), written_.load(), dropped_.load(), failures_.load(), batches_.load(),
            pending_.load(), pending_bytes_.load(), accepting_.load()};
    }
private:
    using Clock = std::chrono::steady_clock;
    bool drop() { ++dropped_; return false; }
    void run() {
        std::array<char, BATCH> batch {};
        unsigned retry_ms = 250;
        while (true) {
            size_t size = 0, records = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(250), [this] {
                    return stopping_ || bytes_ >= BATCH || count_ >= RECORDS / 2;
                });
                if (stopping_ && (count_ == 0 || Clock::now() >= drain_deadline_)) {
                    dropped_ += count_; count_ = bytes_ = 0;
                    pending_ = pending_bytes_ = 0; break;
                }
                while (count_ && lengths_[head_] <= BATCH - size) {
                    const size_t length = lengths_[head_], first = std::min(length, CAPACITY - read_);
                    std::memcpy(batch.data() + size, buffer_.data() + read_, first);
                    std::memcpy(batch.data() + size + first, buffer_.data(), length - first);
                    size += length; read_ = (read_ + length) % CAPACITY;
                    head_ = (head_ + 1) % RECORDS; --count_; bytes_ -= length; ++records;
                }
                pending_ = count_; pending_bytes_ = bytes_;
            }
            if (!size) continue;
            bool ok = false;
            try { ok = sink_(batch.data(), size); } catch (...) { /* logging never faults the data plane */ }
            if (ok) { written_ += records; ++batches_; retry_ms = 250; }
            else {
                dropped_ += records; ++failures_;
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(retry_ms), [this] { return stopping_; });
                retry_ms = std::min(retry_ms * 2, 8000u);
            }
        }
    }
    std::array<char, CAPACITY> buffer_ {};
    std::array<uint16_t, RECORDS> lengths_ {};
    size_t read_ = 0, write_ = 0, head_ = 0, count_ = 0, bytes_ = 0;
    std::atomic<uint64_t> enqueued_{0}, written_{0}, dropped_{0}, failures_{0}, batches_{0}, pending_{0}, pending_bytes_{0};
    std::atomic<bool> accepting_{false};
    bool stopping_ = false;
    Clock::time_point drain_deadline_;
    Sink sink_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
};
