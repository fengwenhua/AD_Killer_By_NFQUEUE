#include "rules.h"
#include "source_validator.h"
#include "logging.h"
#include "fuzz_support.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

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

    // Exercise ownership, sorted views and compression across source seams.
    std::string canonical = line;
    if (!canonical.empty() && canonical.back() != '\n') canonical.push_back('\n');
    const auto middle = canonical.find('\n', canonical.size() / 2);
    const auto split = middle == std::string::npos ? canonical.size() : middle + 1;
    std::string first = canonical.substr(0, split), second = canonical.substr(split);
    CanonicalRuleChunks chunks(canonical.size() * 2 + 1);
    fuzz_require(chunks.append(std::string(first)) && chunks.append(std::move(first)) &&
                 chunks.append(std::move(second)));
    deduplicate_rule_text(canonical);
    std::string expected, actual;
    fuzz_require(gz_compress_parts({"# fuzz\n", canonical}, expected));
    fuzz_require(chunks.compress_gzip("# fuzz\n", actual) && actual == expected && chunks.empty());
    return 0;
}
