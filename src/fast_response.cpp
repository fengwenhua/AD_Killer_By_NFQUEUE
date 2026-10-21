#include "fast_response.h"
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

FastResponseSender::~FastResponseSender() { close(); }

void FastResponseSender::close() {
    if (ipv4_ >= 0) ::close(ipv4_);
    if (ipv6_ >= 0) ::close(ipv6_);
    ipv4_ = ipv6_ = -1;
}

bool FastResponseSender::open() {
    close();
    ipv4_ = socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_RAW);
    ipv6_ = socket(AF_INET6, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_RAW);
    int one = 1;
    if (ipv4_ < 0 || ipv6_ < 0 ||
        setsockopt(ipv4_, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one)) != 0 ||
        setsockopt(ipv6_, IPPROTO_IPV6, IPV6_HDRINCL, &one, sizeof(one)) != 0) {
        int error = errno;
        close();
        errno = error;
        return false;
    }
    return true;
}

bool FastResponseSender::send(const std::vector<uint8_t> &packet, uint32_t interface_index) {
    if (packet.size() < 28) { errno = EINVAL; return false; }
    sockaddr_storage storage{};
    socklen_t length;
    int fd;
    if ((packet[0] >> 4) == 4) {
        auto *target = reinterpret_cast<sockaddr_in *>(&storage);
        target->sin_family = AF_INET;
        memcpy(&target->sin_addr, packet.data() + 16, 4);
        length = sizeof(*target); fd = ipv4_;
    } else if ((packet[0] >> 4) == 6 && packet.size() >= 48) {
        auto *target = reinterpret_cast<sockaddr_in6 *>(&storage);
        target->sin6_family = AF_INET6;
        memcpy(&target->sin6_addr, packet.data() + 24, 16);
        if (IN6_IS_ADDR_LINKLOCAL(&target->sin6_addr)) target->sin6_scope_id = interface_index;
        length = sizeof(*target); fd = ipv6_;
    } else { errno = EINVAL; return false; }
    ssize_t sent;
    do { sent = sendto(fd, packet.data(), packet.size(), MSG_DONTWAIT,
                       reinterpret_cast<sockaddr *>(&storage), length); }
    while (sent < 0 && errno == EINTR);
    if (sent >= 0 && static_cast<size_t>(sent) != packet.size()) errno = EIO;
    return sent >= 0 && static_cast<size_t>(sent) == packet.size();
}
