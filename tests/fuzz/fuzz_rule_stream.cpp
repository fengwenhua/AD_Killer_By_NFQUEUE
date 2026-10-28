#include "rule_stream.h"
#include "fuzz_support.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    FuzzFile file(data + 2, size - 2);
    RuleStreamLimits limits;
    limits.input_bytes = 131072;
    limits.output_bytes = (data[1] & 1) ? 65536 : 262144;
    limits.lines = (data[1] & 2) ? 8 : 4096;
    limits.line_bytes = (data[1] & 4) ? 32 : 1024;
    size_t lines = 0, overlong = 0;
    RuleStreamResult result;
    const bool ok = stream_rule_file(file.path, (data[0] & 1) != 0,
        [&](const std::string &line, bool long_line) {
            ++lines; overlong += long_line;
            fuzz_require(line.size() <= limits.line_bytes && lines <= limits.lines);
            if (long_line) fuzz_require(line.size() == limits.line_bytes);
        }, &result, limits);
    fuzz_require(result.output_bytes <= limits.output_bytes && result.lines == lines && result.overlong_lines == overlong);
    fuzz_require(ok ? result.error.empty() : !result.error.empty());
    return 0;
}
