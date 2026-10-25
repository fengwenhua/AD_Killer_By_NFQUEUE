#include "async_log.h"
#include <cassert>
#include <future>
#include <iostream>
#include <string>

int main() {
    AsyncLog log;
    assert(!log.enqueue("offline\n", 8) && log.stats().dropped == 1);
    assert(!log.start({}));
    std::string output, expected;
    assert(log.start([&](const char *data, size_t size) { output.append(data, size); return true; }));
    assert(!log.start([](const char *, size_t) { return true; }));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (unsigned i = 0; i < 4096; ++i) {
        const auto line = std::to_string(i) + ':' + std::string(i % 600, 'x') + '\n';
        while (!log.enqueue(line.data(), line.size())) {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expected += line;
    }
    log.stop();
    const auto complete = log.stats();
    assert(output == expected && complete.written == 4096 && complete.enqueued == 4096);
    assert(complete.batches > 1 && complete.batches < 512 && complete.pending == 0 && !complete.active);

    std::mutex gate;
    std::condition_variable wake;
    bool entered = false, released = false;
    assert(log.start([&](const char *, size_t) {
        std::unique_lock<std::mutex> lock(gate);
        entered = true; wake.notify_all();
        wake.wait(lock, [&] { return released; });
        return true;
    }));
    while (!log.enqueue("first\n", 6)) std::this_thread::yield();
    {
        std::unique_lock<std::mutex> lock(gate);
        assert(wake.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    const auto baseline = log.stats();
    auto producer = std::async(std::launch::async, [&] {
        const std::string line(AsyncLog::MAX_LINE, 's');
        for (unsigned i = 0; i < 500; ++i) log.enqueue(line.data(), line.size());
    });
    const bool nonblocking = producer.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    if (!nonblocking) { std::lock_guard<std::mutex> lock(gate); released = true; wake.notify_all(); }
    producer.get(); assert(nonblocking);
    const auto full = log.stats();
    assert(full.pending_bytes <= AsyncLog::CAPACITY && full.pending <= AsyncLog::RECORDS && full.dropped > baseline.dropped);
    std::thread stopper([&] { log.stop(); });
    while (log.stats().active) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    { std::lock_guard<std::mutex> lock(gate); released = true; }
    wake.notify_all(); stopper.join();
    const auto stopped = log.stats();
    assert(stopped.written == 1 && stopped.dropped - baseline.dropped == 500 && stopped.pending_bytes == 0);

    assert(log.start([](const char *, size_t) -> bool { throw 1; }));
    size_t accepted = 0;
    for (unsigned i = 0; i < 50; ++i) accepted += log.enqueue("failure\n", 8) ? 1 : 0;
    log.stop();
    const auto failed = log.stats();
    assert(accepted > 0 && failed.written == 0 && failed.dropped == 50 && failed.write_failures > 0 && failed.pending == 0);
    std::cout << "async logs: byte-exact wrap/FIFO, batched writes, bounded slow-writer pressure, nonblocking producers, stop budget and failed writes passed\n";
}
