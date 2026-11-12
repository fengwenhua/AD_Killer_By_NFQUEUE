// Actual receive loop with real socket readiness; no netlink/root/firewall I/O.
#include "../src/engine.cpp"
#include <cassert>
#include <sys/socket.h>

namespace {
std::atomic<unsigned> attempts[2], packets[2];
int queue_fds[2] = {-1, -1};
std::atomic<bool> hold_empty{false}, empty_entered{false}, release_empty{false};
std::atomic<bool> close_on_empty{false};
std::atomic<bool> hangup_on_empty{false};
int peer_fd = -1;
std::atomic<bool> closed_fd{false};
std::string recorded_error;

template<class Predicate> void await(Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(done());
}
void reset() {
    g_stop.store(false); g_fault.store(false); g_signal_stop = 0;
    hold_empty.store(false); empty_entered.store(false); release_empty.store(false);
    close_on_empty.store(false); hangup_on_empty.store(false); closed_fd.store(false); recorded_error.clear();
    for (unsigned q = 0; q < 2; ++q) { attempts[q].store(0); packets[q].store(0); }
}
bool packet(const NfqPacket &, void *context) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(context));
    packets[fd == queue_fds[0] ? 0 : 1].fetch_add(1);
    return true;
}
struct Socket {
    int fd[2];
    explicit Socket(unsigned q) { assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0); queue_fds[q] = fd[0]; }
    ~Socket() { if (fd[0] >= 0) close(fd[0]); close(fd[1]); }
    void send(char value) { assert(::send(fd[1], &value, 1, 0) == 1); }
};
void interrupted(int) {}
}

FastResponseSender::~FastResponseSender() {}
void log_line(const char *, const char *, ...) {}
bool write_file_atomic(const char *, const std::string &text) { recorded_error = text; return true; }
std::string read_conf_value(const char *, const char *) { return {}; }
NfqReceiveReport &nfq_receive_report() { static thread_local NfqReceiveReport report; return report; }
int nfq_recv_packets(int fd, NfqPacketCb callback, void *context) {
    const unsigned q = fd == queue_fds[0] ? 0 : 1;
    attempts[q].fetch_add(1);
    auto &report = nfq_receive_report(); report = {};
    char value = 0;
    const auto n = recv(fd, &value, 1, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (hold_empty.load()) {
            empty_entered.store(true);
            await([] { return release_empty.load() || g_stop.load(); });
        }
        report.idle = true;
        if (close_on_empty.exchange(false)) { close(fd); closed_fd.store(true); }
        if (hangup_on_empty.exchange(false)) close(peer_fd);
        return 0;
    }
    if (n == 1) {
        if (value == 'A') return 0; // ACK-only frame, not a drained receive.
        if (value == 'I') { errno = EINTR; return 0; }
        if (value == 'F') { report.kind = NfqReceiveKind::KERNEL; report.error = ENOBUFS; return -1; }
        return callback(NfqPacket{}, context) ? 1 : -1;
    }
    report.kind = NfqReceiveKind::SOCKET; report.error = n == 0 ? ECONNRESET : errno;
    return -1;
}

int main() {
    reset();
    {
        Socket dns(0), tls(1);
        const auto idle_started = std::chrono::steady_clock::now();
        std::thread a(recv_loop, dns.fd[0], 0, packet), b(recv_loop, tls.fd[0], 1, packet);
        await([] { return attempts[0].load() && attempts[1].load(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        const auto left = attempts[0].load(), right = attempts[1].load();
        const double idle_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - idle_started).count();
        // Use actual elapsed time: a delayed runner must not fail this check.
        // Leave room for startup, but reject startup spin and 2ms polling.
        assert(left <= 20 * idle_seconds + 4 && right <= 20 * idle_seconds + 4);
        printf("idle receive attempts per second: %.2f / %.2f\n", left / idle_seconds, right / idle_seconds);
        dns.send('D'); tls.send('D');
        await([] { return packets[0].load() == 1 && packets[1].load() == 1; });
        for (unsigned i = 0; i < 12; ++i) { dns.send('D'); tls.send('D'); }
        await([] { return packets[0].load() == 13 && packets[1].load() == 13; });
        // A new epoch must drain even when both real sockets are empty and
        // both consumers were already asleep before the policy change.
        {
            PolicyBarrier::Change change(g_policy);
            assert(change.ok()); const auto epoch = change.seal();
            await([&] { return g_policy.drained(epoch); });
            change.resume();
        }
        const auto stopping = std::chrono::steady_clock::now();
        g_stop.store(true); a.join(); b.join();
        assert(std::chrono::steady_clock::now() - stopping < std::chrono::seconds(2));
        assert(!g_fault.load());
    }
    for (char frame : {'A', 'I'}) {
        reset(); Socket socket(0); socket.send(frame); hold_empty.store(true);
        PolicyBarrier::Change change(g_policy); assert(change.ok());
        const auto epoch = change.seal(); g_policy.observe_idle(1, epoch);
        std::thread worker(recv_loop, socket.fd[0], 0, packet);
        await([] { return empty_entered.load(); });
        assert(!g_policy.drained(epoch)); // Neither ACK nor EINTR acknowledged.
        release_empty.store(true);
        await([&] { return g_policy.drained(epoch); }); // Actual EAGAIN did.
        change.resume(); g_stop.store(true); worker.join(); assert(!g_fault.load());
    }
    reset();
    {
        struct sigaction action{}, saved{}; action.sa_handler = interrupted;
        assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGUSR1, &action, &saved) == 0);
        Socket socket(0); std::thread worker(recv_loop, socket.fd[0], 0, packet);
        await([] { return attempts[0].load() > 0; });
        for (unsigned i = 0; i < 5; ++i) {
            assert(pthread_kill(worker.native_handle(), SIGUSR1) == 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        socket.send('D'); await([] { return packets[0].load() == 1; });
        g_stop.store(true); worker.join(); assert(!g_fault.load());
        assert(sigaction(SIGUSR1, &saved, nullptr) == 0);
    }
    reset();
    {
        Socket socket(0); close_on_empty.store(true);
        const auto before = g_metrics.snapshot().queue_errors;
        std::thread worker(recv_loop, socket.fd[0], 0, packet);
        await([] { return g_stop.load(); }); worker.join();
        assert(closed_fd.load()); socket.fd[0] = -1;
        assert(g_fault.load() && g_metrics.snapshot().queue_errors == before + 1);
        assert(recorded_error.find("code=queue_receive_failed") != std::string::npos);
        assert(recorded_error.find("errno=" + std::to_string(EBADF)) != std::string::npos);
    }
    reset();
    {
        Socket socket(0); peer_fd = socket.fd[1]; hangup_on_empty.store(true);
        const auto before = g_metrics.snapshot().queue_errors;
        std::thread worker(recv_loop, socket.fd[0], 0, packet);
        await([] { return g_stop.load(); }); worker.join(); socket.fd[1] = -1;
        assert(g_fault.load() && g_metrics.snapshot().queue_errors == before + 1);
        assert(recorded_error.find("errno=" + std::to_string(ECONNRESET)) != std::string::npos);
    }
    reset();
    {
        Socket socket(0); socket.send('F');
        const auto before = g_metrics.snapshot().queue_errors;
        std::thread worker(recv_loop, socket.fd[0], 0, packet);
        await([] { return g_stop.load(); }); worker.join();
        assert(g_fault.load() && g_metrics.snapshot().queue_errors == before + 1);
        assert(recorded_error.find("errno=" + std::to_string(ENOBUFS)) != std::string::npos);
    }
    puts("actual queue loop: bounded idle attempts, readiness/bursts, idle reload/stop, ACK/EINTR and socket faults passed");
}
