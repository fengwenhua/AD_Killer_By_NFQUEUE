#include "nfnetlink.h"
#include "common.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

struct ProbeState {
    int fd = -1;
    uint16_t queue_num = 0;
    bool seen_ipv4 = false;
    bool seen_ipv6 = false;
    bool failed = false;
    bool saturation = false;
    bool stale_seen = false;
    const char *marked_file = nullptr;
};

bool contains_magic(const uint8_t *data, size_t len) {
    static constexpr char magic[] = "ad-killer-by-nfqueue-nfqueue-probe";
    if (!data || len < sizeof(magic) - 1) return false;
    for (size_t offset = 0; offset + sizeof(magic) - 1 <= len; ++offset) {
        if (memcmp(data + offset, magic, sizeof(magic) - 1) == 0) return true;
    }
    return false;
}

bool inspect_packet(const NfqPacket &packet, void *userdata) {
    auto *state = static_cast<ProbeState *>(userdata);
    const bool valid = packet.id != 0 && packet.queue_num == state->queue_num &&
                       packet.payload != nullptr && packet.payload_len != 0 &&
                       !packet.truncated &&
                       contains_magic(packet.payload, packet.payload_len);
    const bool first_ipv4 = valid && packet.hw_proto == 0x0800 &&
                            !state->seen_ipv4;
    if (first_ipv4) state->seen_ipv4 = true;
    else if (valid && packet.hw_proto == 0x86dd && !state->seen_ipv6)
        state->seen_ipv6 = true;
    else
        state->failed = true;

    // 即使断言失败也必须给内核中的包 verdict，避免测试故障影响 runner 网络。
    bool verdict_ok = false;
    if (state->saturation) {
        verdict_ok = nfq_set_verdict(
            state->fd, state->queue_num, packet.id, NF_ACCEPT_);
    } else if (first_ipv4) {
        verdict_ok = nfq_set_verdict_with_connmark(
            state->fd, state->queue_num, packet.id, NF_ACCEPT_,
            BLOCK_CONNMARK, BLOCK_CONNMARK_MASK);
    } else if (valid && packet.hw_proto == 0x86dd) {
        verdict_ok = nfq_set_verdict_with_connmark(
            state->fd, state->queue_num, packet.id, NF_ACCEPT_,
            PASS_CONNMARK, BLOCK_CONNMARK_MASK);
    } else {
        verdict_ok = nfq_set_verdict(
            state->fd, state->queue_num, packet.id, NF_ACCEPT_);
    }
    if (!verdict_ok) {
        state->failed = true;
        return false;
    }
    if (first_ipv4 && !state->saturation &&
        !nfq_set_verdict(state->fd, state->queue_num, packet.id, NF_ACCEPT_)) {
        state->failed = true;
        return false;
    }
    if (first_ipv4 && !state->saturation) {
        FILE *marked = std::fopen(state->marked_file, "w");
        if (!marked) {
            state->failed = true;
            return false;
        }
        std::fputs("marked\n", marked);
        std::fclose(marked);
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--mark-specs") == 0) {
        // The test seeds network/StrictController bits before queueing. Match
        // the complete result to prove verdicts preserve all non-module bits.
        constexpr uint32_t seed = 0x030abcdeu;
        std::printf("0x%x/0xffffffff 0x%x/0xffffffff\n",
                    BLOCK_CONNMARK | seed, PASS_CONNMARK | seed);
        return EXIT_SUCCESS;
    }
    const bool saturation = argc == 5 && std::strcmp(argv[4], "--saturate") == 0;
    if (argc != 4 && !saturation) {
        std::fprintf(stderr,
                     "usage: nfqueue_probe QUEUE_NUM READY_FILE MARKED_FILE\n");
        return EXIT_FAILURE;
    }
    char *end = nullptr;
    long parsed = std::strtol(argv[1], &end, 10);
    if (!end || *end != '\0' || parsed <= 0 || parsed > 65535) {
        std::fprintf(stderr, "invalid queue number\n");
        return EXIT_FAILURE;
    }
    const uint16_t queue_num = static_cast<uint16_t>(parsed);

    int fd = nfq_socket_open();
    if (fd < 0) {
        std::perror("nfq_socket_open");
        return EXIT_FAILURE;
    }
    nfq_pf_bind(fd, true);
    const char *failed_step = nullptr;
    if (!nfq_queue_bind(fd, queue_num))
        failed_step = "nfq_queue_bind";
    else if (!nfq_set_params(fd, queue_num, NfqCopyMode::PACKET, 4096,
                             saturation ? 1 : 64))
        failed_step = "nfq_set_params";
    else if (!nfq_set_fail_open(fd, queue_num, true))
        failed_step = "nfq_set_fail_open";
    else if (!nfq_set_conntrack_metadata(fd, queue_num, true))
        failed_step = "nfq_enable_conntrack_metadata";
    else if (!nfq_set_conntrack_metadata(fd, queue_num, false))
        failed_step = "nfq_disable_conntrack_metadata";
    if (failed_step) {
        const int setup_errno = errno;
        std::fprintf(stderr, "NFQUEUE setup step %s failed: %s (%d)\n",
                     failed_step, std::strerror(setup_errno), setup_errno);
        nfq_queue_unbind(fd, queue_num);
        close(fd);
        return EXIT_FAILURE;
    }

    FILE *ready = std::fopen(argv[2], "w");
    if (!ready) {
        std::perror("ready file");
        nfq_queue_unbind(fd, queue_num);
        close(fd);
        return EXIT_FAILURE;
    }
    std::fputs("ready\n", ready);
    std::fclose(ready);

    ProbeState state;
    state.fd = fd;
    state.queue_num = queue_num;
    state.marked_file = argv[3];
    state.saturation = saturation;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);
    // With maxlen=1, hold the first queued packet until Python has verified
    // that the second packet bypassed the full queue through FAIL_OPEN.
    while (saturation && access(argv[3], F_OK) != 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::chrono::steady_clock::time_point verification_deadline;
    while (std::chrono::steady_clock::now() < deadline &&
           !state.failed) {
        if (state.seen_ipv4 && (saturation || state.seen_ipv6)) {
            if (verification_deadline.time_since_epoch().count() == 0)
                verification_deadline = std::chrono::steady_clock::now() +
                                        std::chrono::seconds(2);
            if (std::chrono::steady_clock::now() >= verification_deadline) break;
        }
        int received = nfq_recv_packets(fd, inspect_packet, &state);
        state.stale_seen = state.stale_seen || nfq_receive_report().stale_verdicts > 0;
        if (received < 0) {
            state.failed = true;
            break;
        }
        if (received == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    nfq_queue_unbind(fd, queue_num);
    nfq_pf_bind(fd, false);
    close(fd);
    unlink(argv[2]);
    unlink(argv[3]);

    if (state.failed || !state.seen_ipv4 || (!saturation && (!state.seen_ipv6 || !state.stale_seen))) {
        std::fprintf(stderr, "NFQUEUE probe failed: ipv4=%d ipv6=%d protocol_error=%d\n",
                     state.seen_ipv4, state.seen_ipv6, state.failed);
        return EXIT_FAILURE;
    }
    std::puts(saturation ? "real NFQUEUE queue saturation fail-open test passed" :
        "real NFQUEUE IPv4/IPv6 verdict, PASS/BLOCK connmark preservation and expired-verdict recovery test passed");
    return EXIT_SUCCESS;
}
