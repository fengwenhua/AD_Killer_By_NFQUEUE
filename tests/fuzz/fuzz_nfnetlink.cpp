#include "nfnetlink.h"

#include <cstddef>
#include <cstdint>

namespace {

bool accept_packet(const NfqPacket &, void *) {
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    nfq_parse_packet_messages(data, size, accept_packet, nullptr);
    int32_t error = 0;
    nfq_parse_ack_messages(data, size, 1, error);
    return 0;
}
