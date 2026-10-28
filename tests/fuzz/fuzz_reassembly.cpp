#include "state_input.h"
#include "fuzz_support.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // Exercise both production limits and resource rejection with smaller caps.
    for (bool small : {false, true}) {
        ReassemblyLimits limits;
        if (small) { limits.max_flows = 8; limits.total_bytes = 16384; limits.per_flow_bytes = 4096; }
        HandshakeReassembler parser(limits);
        const auto greatest = fuzz_actions(data, size, [&](unsigned op, const TcpFlowKey &key,
                uint32_t sequence, uint8_t flags, const uint8_t *payload, size_t bytes, uint64_t now) {
            switch (op) {
                case 0: {
                    const auto result = parser.inspect(key, sequence, flags, payload, bytes, now);
                    fuzz_require(result.decision != ReassemblyDecision::DOMAIN_FOUND || !result.domain.empty());
                    break;
                }
                case 1: parser.mark_blocked(key, now); break;
                case 2: parser.forget(key); break;
                case 3: parser.prune(now); break;
                case 4: parser.clear(); break;
                case 5: parser.inspect(key, sequence, flags, nullptr, 0, now); break;
            }
            const auto retained = parser.stats();
            fuzz_require(retained.flows <= limits.max_flows && retained.bytes <= limits.total_bytes);
            if (op == 4) fuzz_require(retained.flows == 0 && retained.bytes == 0);
        });
        parser.prune(greatest + limits.blocked_timeout_ms + 1);
        fuzz_require(parser.stats().flows == 0 && parser.stats().bytes == 0);
    }
    return 0;
}
