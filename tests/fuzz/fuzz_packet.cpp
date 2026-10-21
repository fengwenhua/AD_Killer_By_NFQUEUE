#include "packet.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    TransportView view;
    decode_ip_transport(data, size, ETH_PROTO_IPV4, view);
    decode_ip_transport(data, size, ETH_PROTO_IPV6, view);
    dns_negative_packet(data, size, ETH_PROTO_IPV4);
    dns_negative_packet(data, size, ETH_PROTO_IPV6);
    udp_unreachable_packet(data, size, ETH_PROTO_IPV4);
    udp_unreachable_packet(data, size, ETH_PROTO_IPV6);
    tcp_reset_packet(view);
    dns_extract_qname(data, size, false);
    dns_extract_qname(data, size, true);
    dns_extract_qnames(data, size, false);
    dns_extract_qnames(data, size, true);
    tls_extract_sni(data, size);
    http_extract_host(data, size);
    extract_domain_from_stream(data, size);
    return 0;
}
