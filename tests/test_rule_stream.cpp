#include "rule_stream.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <zlib.h>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

bool write_gzip(const char *path, const std::string &content) {
    gzFile file = gzopen(path, "wb9");
    if (!file) return false;
    int written = gzwrite(file, content.data(), static_cast<unsigned>(content.size()));
    return written == static_cast<int>(content.size()) && gzclose(file) == Z_OK;
}

}  // namespace

int main() {
    char path[] = "/tmp/ad_killer_by_nfqueue_stream.XXXXXX";
    int fd = mkstemp(path);
    expect(fd >= 0, "temporary stream file should be created");
    if (fd >= 0) close(fd);

    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "one.example\ntwo.example\n";
    }
    size_t callbacks = 0;
    RuleStreamResult result;
    expect(stream_rule_file(path, false,
                            [&](const std::string &, bool) { callbacks++; },
                            &result) &&
               callbacks == 2 && result.lines == 2 && !result.gzip,
           "plain rules should stream one callback per line");

    RuleStreamLimits tiny_input;
    tiny_input.input_bytes = 4;
    expect(!stream_rule_file(path, false, [](const std::string &, bool) {},
                             &result, tiny_input) &&
               result.error == "compressed_size_limit",
           "input bytes should be bounded before reading");

    expect(write_gzip(path, "one.example\ntwo.example\n"),
           "gzip fixture should be written");
    callbacks = 0;
    expect(stream_rule_file(path, true,
                            [&](const std::string &, bool) { callbacks++; },
                            &result) &&
               callbacks == 2 && result.gzip,
           "gzip rules should be decompressed incrementally");

    RuleStreamLimits tiny_output;
    tiny_output.output_bytes = 8;
    expect(!stream_rule_file(path, true, [](const std::string &, bool) {},
                             &result, tiny_output) &&
               result.error == "decompressed_size_limit",
           "decompressed bytes should stop at the configured limit");

    RuleStreamLimits one_line;
    one_line.lines = 1;
    expect(!stream_rule_file(path, true, [](const std::string &, bool) {},
                             &result, one_line) &&
               result.error == "line_count_limit",
           "line count should stop at the configured limit");

    expect(write_gzip(path, std::string(32, 'x') + "\n"),
           "overlong gzip fixture should be written");
    RuleStreamLimits short_line;
    short_line.line_bytes = 8;
    bool saw_overlong = false;
    size_t retained = 0;
    expect(stream_rule_file(path, true,
                            [&](const std::string &line, bool overlong) {
                                retained = line.size();
                                saw_overlong = overlong;
                            }, &result, short_line) &&
               saw_overlong && retained == 8 && result.overlong_lines == 1,
           "overlong lines should be truncated without growing the buffer");

    expect(write_gzip(path, "truncated.example\n"),
           "truncated gzip fixture should be written");
    std::ifstream size_file(path, std::ios::binary | std::ios::ate);
    off_t size = static_cast<off_t>(size_file.tellg());
    size_file.close();
    expect(size > 4 && truncate(path, size - 4) == 0,
           "gzip trailer should be truncated");
    expect(!stream_rule_file(path, true, [](const std::string &, bool) {},
                             &result) &&
               result.error == "gzip_decode_failed",
           "truncated gzip streams should fail structural validation");

    unlink(path);
    if (failures != 0) {
        std::cerr << failures << " rule stream test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "rule stream tests passed\n";
    return EXIT_SUCCESS;
}
