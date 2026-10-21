// Bounded loopback-only black-box check for an already running engine.
// It never writes rules/configuration and never starts/stops an engine.
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

struct Fd {
    int value;
    explicit Fd(int v) : value(v) { if (v < 0) throw std::runtime_error(std::strerror(errno)); }
    ~Fd() { ::close(value); }
    Fd(const Fd &) = delete;
};
static void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static void timeout(int fd) {
    timeval t{1, 0};
    require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t)) == 0, "receive timeout");
    require(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t)) == 0, "send timeout");
}
static sockaddr_storage target(int family, int port) {
    sockaddr_storage storage{};
    if (family == AF_INET) {
        auto *a = reinterpret_cast<sockaddr_in *>(&storage);
        a->sin_family = AF_INET; a->sin_port = htons(port);
        inet_pton(family, "127.0.0.1", &a->sin_addr);
    } else {
        auto *a = reinterpret_cast<sockaddr_in6 *>(&storage);
        a->sin6_family = AF_INET6; a->sin6_port = htons(port);
        inet_pton(family, "::1", &a->sin6_addr);
    }
    return storage;
}
static void setup(int fd, int family, int port, bool bound) {
    auto a = target(family, port);
    socklen_t n = family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    timeout(fd);
    int result = bound ? bind(fd, reinterpret_cast<sockaddr *>(&a), n)
                       : connect(fd, reinterpret_cast<sockaddr *>(&a), n);
    require(result == 0, bound ? "loopback port busy; probe aborted" : "loopback connect failed");
}
static double elapsed(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
static double udp_test(int family, const std::string &domain, bool dns) {
    Fd server(socket(family, SOCK_DGRAM, 0)), client(socket(family, SOCK_DGRAM, 0));
    setup(server.value, family, dns ? 53 : 443, true);
    setup(client.value, family, dns ? 53 : 443, false);
    std::vector<uint8_t> query(12, 0);
    query[0] = 0x13; query[1] = 0x81; query[2] = 1; query[5] = 1;
    size_t begin = 0;
    while (begin < domain.size()) {
        auto end = domain.find('.', begin);
        if (end == std::string::npos) end = domain.size();
        require(end > begin && end - begin <= 63, "invalid probe name");
        query.push_back(static_cast<uint8_t>(end - begin));
        query.insert(query.end(), domain.begin() + begin, domain.begin() + end);
        begin = end + 1;
    }
    query.insert(query.end(), {0,0,1,0,1});
    auto start = std::chrono::steady_clock::now();
    require(send(client.value, query.data(), query.size(), 0) == static_cast<ssize_t>(query.size()), "UDP send failed");
    uint8_t reply[4096];
    ssize_t n = recv(client.value, reply, sizeof(reply), 0);
    if (dns) require(n >= 12 && reply[0] == 0x13 && reply[1] == 0x81 &&
                     (reply[2] & 0x80) && (reply[3] & 15) == 3, "DNS did not return NXDOMAIN within 1s");
    else require(n < 0 && errno == ECONNREFUSED, "QUIC did not fail with port unreachable within 1s");
    return elapsed(start);
}
static double tcp_test(int family, const std::string &domain, bool blocked) {
    Fd listener(socket(family, SOCK_STREAM, 0)), client(socket(family, SOCK_STREAM, 0));
    int one = 1;
    setsockopt(listener.value, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setup(listener.value, family, 80, true);
    require(listen(listener.value, 1) == 0, "listen failed");
    setup(client.value, family, 80, false);
    Fd peer(accept(listener.value, nullptr, nullptr)); timeout(peer.value);
    std::string payload = "GET / HTTP/1.1\r\nHost: " + domain + "\r\n\r\n";
    auto start = std::chrono::steady_clock::now();
    ssize_t sent = send(client.value, payload.data(), payload.size(), MSG_NOSIGNAL);
    uint8_t received[1024];
    if (blocked) {
        ssize_t n = sent < 0 ? sent : recv(client.value, received, sizeof(received), 0);
        require(n < 0 && errno == ECONNRESET, "blocked HTTP did not promptly reset client");
    } else {
        require(sent == static_cast<ssize_t>(payload.size()), "allowed HTTP send failed");
        std::string actual;
        while (actual.size() < payload.size()) {
            ssize_t n = recv(peer.value, received, sizeof(received), 0);
            require(n > 0, "allowed HTTP did not arrive");
            actual.append(reinterpret_cast<char *>(received), static_cast<size_t>(n));
        }
        require(actual == payload, "allowed HTTP payload changed");
    }
    return elapsed(start);
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: device_latency_probe BLOCKED_DOMAIN (engine running, QUIC enabled)");
        std::string blocked = argv[1];
        require(!blocked.empty() && blocked.size() <= 253, "invalid probe name");
        for (int family : {AF_INET, AF_INET6}) {
            double dns = udp_test(family, blocked, true);
            double tcp = tcp_test(family, blocked, true);
            double quic = udp_test(family, blocked, false);
            double pass = tcp_test(family, "safe.example.org", false);
            std::printf("IPv%d DNS NXDOMAIN %.2fms; TCP reset %.2fms; QUIC unreachable %.2fms; HTTP pass %.2fms\n",
                        family == AF_INET ? 4 : 6, dns, tcp, quic, pass);
        }
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "latency probe failed: %s (errno=%d)\n", error.what(), errno);
        return 1;
    }
}
