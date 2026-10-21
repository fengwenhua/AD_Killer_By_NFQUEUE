#pragma once

#include <cstddef>
#include <functional>
#include <string>

constexpr size_t MAX_RULE_INPUT_BYTES = 64u * 1024u * 1024u;
constexpr size_t MAX_RULE_DECOMPRESSED_BYTES = 256u * 1024u * 1024u;
constexpr size_t MAX_RULE_LINES = 5u * 1000u * 1000u;
constexpr size_t MAX_RULE_LINE_BYTES = 1024u;

struct RuleStreamLimits {
    size_t input_bytes = MAX_RULE_INPUT_BYTES;
    size_t output_bytes = MAX_RULE_DECOMPRESSED_BYTES;
    size_t lines = MAX_RULE_LINES;
    size_t line_bytes = MAX_RULE_LINE_BYTES;
};

struct RuleStreamResult {
    bool gzip = false;
    size_t input_bytes = 0;
    size_t output_bytes = 0;
    size_t lines = 0;
    size_t overlong_lines = 0;
    std::string error;
};

using RuleLineCallback =
    std::function<void(const std::string &line, bool overlong)>;

// 以固定大小缓冲区读取普通文本或 gzip 文件，并逐行回调。超长行只保留前
// line_bytes 字节并标记 overlong，不会随攻击者输入继续增长内存。
bool stream_rule_file(const char *path, bool require_gzip,
                      const RuleLineCallback &callback,
                      RuleStreamResult *result = nullptr,
                      const RuleStreamLimits &limits = {});
