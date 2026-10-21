// Reproducible library microbenchmarks, NOT an app-start/network latency test.
#include "packet.h"
#include "rules.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static volatile size_t sink = 0;

template <typename Work>
static void measure(const char *name, size_t iterations, Work work) {
    for (size_t i = 0; i < 1000; ++i) sink += work(i);
    std::vector<double> samples;
    samples.reserve(iterations);
    const auto start = Clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        const auto begin = Clock::now();
        sink += work(i);
        samples.push_back(std::chrono::duration<double, std::nano>(Clock::now() - begin).count());
    }
    const double total = std::chrono::duration<double>(Clock::now() - start).count();
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](double q) { return samples[static_cast<size_t>(q * (samples.size() - 1))]; };
    std::printf("{\"case\":\"%s\",\"iterations\":%zu,\"p50_ns\":%.0f,\"p95_ns\":%.0f,\"p99_ns\":%.0f,\"throughput_per_second\":%.0f}\n",
        name, iterations, percentile(.50), percentile(.95), percentile(.99), iterations / total);
}

static void u16(std::vector<uint8_t> &bytes, size_t value) {
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

static std::vector<uint8_t> dns(const std::string &host) {
    std::vector<uint8_t> bytes = {0,1,1,0,0,1,0,0,0,0,0,0};
    size_t start = 0;
    while (start < host.size()) {
        const auto dot = host.find('.', start);
        const auto end = dot == std::string::npos ? host.size() : dot;
        bytes.push_back(static_cast<uint8_t>(end - start));
        bytes.insert(bytes.end(), host.begin() + start, host.begin() + end);
        start = end + 1;
    }
    bytes.insert(bytes.end(), {0,0,1,0,1});
    return bytes;
}

static std::vector<uint8_t> tls(const std::string &host) {
    std::vector<uint8_t> body = {3,3};
    body.resize(34, 0);
    body.insert(body.end(), {0,0,2,0x13,1,1,0});
    u16(body, host.size() + 9);
    u16(body, 0); u16(body, host.size() + 5); u16(body, host.size() + 3);
    body.push_back(0); u16(body, host.size());
    body.insert(body.end(), host.begin(), host.end());
    std::vector<uint8_t> message = {1,0,static_cast<uint8_t>(body.size() >> 8),static_cast<uint8_t>(body.size())};
    message.insert(message.end(), body.begin(), body.end());
    std::vector<uint8_t> record = {0x16,3,3}; u16(record, message.size());
    record.insert(record.end(), message.begin(), message.end());
    return record;
}

int main(int argc, char **argv) {
    const size_t count = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 420000;
    const size_t iterations = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 100000;
    if (count < 2 || count > 2000000 || iterations < 1000 || iterations > 1000000) return 2;
    auto database = std::make_shared<RuleDB>();
    const auto begin = Clock::now();
    for (size_t i = 0; i < count; ++i) {
        const std::string host = "rule" + std::to_string(i) + ".example.test";
        if (i % 2) database->black.add_suffix(host);
        else database->black.add_exact(host);
    }
    database->white.add_suffix("allow.example.test");
    database->ok = true;
    publish_rules_snapshot(database);
    std::printf("{\"rules\":%zu,\"hash_storage_bytes\":%zu,\"build_ms\":%.3f}\n", database->black.size(),
        database->black.storage_bytes() + database->white.storage_bytes(),
        std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
    std::vector<std::string> exact, suffix, misses;
    for (size_t i = 0; i < 4096; ++i) {
        const size_t index = (i * 2654435761ULL) % (count / 2);
        exact.push_back("rule" + std::to_string(index * 2) + ".example.test");
        suffix.push_back("one.two.rule" + std::to_string(index * 2 + 1) + ".example.test");
        misses.push_back("one.two.allowed" + std::to_string(i) + ".example.org");
    }
    const std::string white = "sub.allow.example.test";
    std::string deep;
    for (size_t i = 0; i < 60; ++i) deep += "abc.";
    deep += "example.test";
    if (match_domain(exact.front()) != Verdict::BLOCK_EXACT ||
        match_domain(suffix.front()) != Verdict::BLOCK_SUFFIX ||
        match_domain(misses.front()) != Verdict::PASS_DEFAULT ||
        match_domain(white) != Verdict::PASS_WHITELIST ||
        match_domain(deep) != Verdict::PASS_DEFAULT) return 3;
    measure("match_exact", iterations, [&](size_t i) { return static_cast<size_t>(match_domain(exact[i % exact.size()])); });
    measure("match_suffix", iterations, [&](size_t i) { return static_cast<size_t>(match_domain(suffix[i % suffix.size()])); });
    measure("match_miss", iterations, [&](size_t i) { return static_cast<size_t>(match_domain(misses[i % misses.size()])); });
    measure("match_whitelist", iterations, [&](size_t) { return static_cast<size_t>(match_domain(white)); });
    measure("match_deep_252_bytes", iterations, [&](size_t) { return static_cast<size_t>(match_domain(deep)); });
    const auto dns_bytes = dns(exact.front()), tls_bytes = tls(exact.front());
    const std::string http = "GET / HTTP/1.1\r\nHost: " + exact.front() + "\r\n\r\n";
    const std::string long_http = "GET / HTTP/1.1\r\nX-Padding: " + std::string(7000, 'x') + "\r\nHost: " + exact.front() + "\r\n\r\n";
    if (dns_extract_qname(dns_bytes.data(), dns_bytes.size(), false) != exact.front() ||
        tls_extract_sni(tls_bytes.data(), tls_bytes.size()) != exact.front() ||
        http_extract_host(reinterpret_cast<const uint8_t *>(http.data()), http.size()) != exact.front() ||
        http_extract_host(reinterpret_cast<const uint8_t *>(long_http.data()), long_http.size()) != exact.front()) return 3;
    measure("extract_dns", iterations, [&](size_t) { return dns_extract_qnames(dns_bytes.data(), dns_bytes.size(), false).size(); });
    measure("extract_tls", iterations, [&](size_t) { return tls_extract_sni(tls_bytes.data(), tls_bytes.size()).size(); });
    measure("extract_http", iterations, [&](size_t) { return http_extract_host(reinterpret_cast<const uint8_t *>(http.data()), http.size()).size(); });
    measure("extract_http_7k_header", iterations, [&](size_t) { return http_extract_host(reinterpret_cast<const uint8_t *>(long_http.data()), long_http.size()).size(); });
    return sink == 0 ? 4 : 0;
}
