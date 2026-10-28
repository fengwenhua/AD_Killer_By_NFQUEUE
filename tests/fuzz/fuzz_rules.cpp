#include "rules.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    std::string line;
    if (size) line.assign(reinterpret_cast<const char *>(data), size);
    RuleDB hosts;
    parse_hosts_line(line, hosts);
    hosts.black.classify(line);

    RuleDB adguard;
    parse_adguard_line(line, adguard);
    adguard.black.classify(line);
    adguard.white.classify(line);
    return 0;
}
