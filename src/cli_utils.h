#pragma once

#include <cerrno>
#include <climits>
#include <cstdlib>

// 严格解析完整的十进制整数；拒绝空串、尾随字符和 int 溢出。
inline bool parse_integer_arg(const char *text, int &value) {
    if (!text || !*text) return false;
    errno = 0;
    char *end = nullptr;
    long parsed = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || !end || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    value = (int)parsed;
    return true;
}

inline bool valid_mode(int mode) {
    return mode == 1 || mode == 2;
}

inline bool valid_scope(int scope) {
    return scope == 0 || scope == 1;
}
