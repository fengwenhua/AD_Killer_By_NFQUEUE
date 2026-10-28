#include "dns_tcp_reassembly.h"
#include "state_input.h"
#include "fuzz_support.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    DnsTcpReassembler parser;
    const auto greatest = fuzz_actions(data, size, [&](unsigned op, const TcpFlowKey &key,
            uint32_t sequence, uint8_t flags, const uint8_t *payload, size_t bytes, uint64_t now) {
        switch (op) {
            case 0: {
                const auto result = parser.inspect(key, sequence, flags, payload, bytes, now);
                fuzz_require(result.consumed_bytes <= 16384);
                if (result.decision == DnsTcpDecision::MESSAGES_READY) {
                    fuzz_require(result.consumed_bytes > 0);
                    // Model the owner ACK as well as an explicit deferred finish.
                    if (flags & 64) parser.finish(key, (flags & 128) != 0, now);
                }
                break;
            }
            case 1: parser.finish(key, true, now); break;
            case 2: parser.forget(key); break;
            case 3: parser.prune(now); break;
            case 4: parser.clear(); break;
            case 5: parser.finish(key, false, now); break;
        }
        const auto retained = parser.retained();
        fuzz_require(retained.first <= 256 && retained.second <= 512 * 1024);
        if (op == 4) fuzz_require(retained.first == 0 && retained.second == 0);
    });
    parser.prune(greatest + 30001);
    fuzz_require(parser.retained().first == 0 && parser.retained().second == 0);
    return 0;
}
