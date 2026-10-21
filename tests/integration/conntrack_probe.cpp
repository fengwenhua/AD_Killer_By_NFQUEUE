#include "nfnetlink.h"
#include "packet.h"
#include "common.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// Only touches loopback tuples supplied by the disposable integration test.
// --probe-only is read-only and safe for device capability diagnosis.
int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--mark-specs") == 0) {
        std::printf("0x%x 0x%x 0x%x\n", BLOCK_CONNMARK, PASS_CONNMARK, BLOCK_CONNMARK_MASK);
        return 0;
    }
    int fd = nfq_socket_open();
    if (fd < 0) { std::perror("socket"); return 1; }
    bool ok = nfct_probe(fd);
    if (argc == 2 && std::strcmp(argv[1], "--probe-only") == 0) {
        if (!ok) std::perror("CTNETLINK read-only probe");
        close(fd);
        return ok ? 0 : 1;
    }
    if (!ok || argc != 6) { close(fd); return 2; }
    TransportView flow;
    flow.ip_version = static_cast<uint8_t>(std::strtoul(argv[1], nullptr, 10));
    flow.protocol = TransportProtocol::TCP;
    flow.src_port = static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10));
    flow.dst_port = static_cast<uint16_t>(std::strtoul(argv[3], nullptr, 10));
    inet_pton(flow.ip_version == 4 ? AF_INET : AF_INET6,
              flow.ip_version == 4 ? "127.0.0.1" : "::1", flow.src_addr.data());
    flow.dst_addr = flow.src_addr;
    uint32_t mark = static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 0));
    uint32_t mask = static_cast<uint32_t>(std::strtoul(argv[5], nullptr, 0));
    ok = nfct_set_mark(fd, flow, mark, mask);
    if (!ok) std::fprintf(stderr, "CT update: errno=%d %s\n", errno, std::strerror(errno));
    close(fd);
    return ok ? 0 : 1;
}
