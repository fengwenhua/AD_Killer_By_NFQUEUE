#include "source_preview.h"
#include "net.h"
#include <cstdint>
#include <vector>
#include <zlib.h>

SourcePreview format_source_preview(const std::string &bytes, bool limited, size_t characters) {
    SourcePreview result;
    if (characters < SOURCE_PREVIEW_MIN || characters > SOURCE_PREVIEW_MAX) { result.error = "预览字符数须为 100–5000"; return result; }
    std::string input = bytes;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0x1f && static_cast<unsigned char>(bytes[1]) == 0x8b) {
        z_stream stream {};
        if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) { result.error = "无法初始化 gzip 预览"; return result; }
        std::vector<char> output(characters * 4 + 4);
        stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(bytes.data()));
        stream.avail_in = static_cast<uInt>(bytes.size());
        stream.next_out = reinterpret_cast<Bytef *>(output.data());
        stream.avail_out = static_cast<uInt>(output.size());
        const int status = inflate(&stream, Z_SYNC_FLUSH);
        const size_t produced = output.size() - stream.avail_out;
        const bool complete = status == Z_STREAM_END;
        inflateEnd(&stream);
        if ((status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) || (!complete && produced == 0)) {
            result.error = "gzip 前缀无法解码"; return result;
        }
        input.assign(output.data(), produced);
        limited = limited || !complete;
    }
    size_t position = input.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    const size_t begin = position;
    while (position < input.size() && result.characters < characters) {
        const uint8_t first = static_cast<uint8_t>(input[position]);
        size_t length = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 : first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (!length) { result.error = "来源前缀不是有效 UTF-8 文本"; return result; }
        if (position + length > input.size()) {
            if (limited) break;
            result.error = "来源末尾 UTF-8 不完整"; return result;
        }
        uint32_t point = first & (length == 1 ? 0x7f : (1u << (7 - length)) - 1);
        for (size_t i = 1; i < length; ++i) {
            const uint8_t next = static_cast<uint8_t>(input[position + i]);
            if ((next & 0xc0) != 0x80) { result.error = "来源前缀不是有效 UTF-8 文本"; return result; }
            point = (point << 6) | (next & 0x3f);
        }
        if ((length == 2 && point < 0x80) || (length == 3 && point < 0x800) ||
            (length == 4 && point < 0x10000) || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) {
            result.error = "来源前缀不是有效 UTF-8 文本"; return result;
        }
        if (point == 0 || (point < 0x20 && point != '\n' && point != '\r' && point != '\t')) {
            result.error = "来源前缀包含二进制控制字符"; return result;
        }
        position += length; ++result.characters;
    }
    result.text = input.substr(begin, position - begin);
    result.truncated = limited || position < input.size();
    return result;
}

SourcePreview preview_source(const std::string &url, size_t characters) {
    if (characters < SOURCE_PREVIEW_MIN || characters > SOURCE_PREVIEW_MAX) {
        SourcePreview result; result.error = "预览字符数须为 100–5000"; return result;
    }
    std::string bytes;
    bool limited = false;
    // UTF-8 worst case plus a small gzip/BOM margin, not an entire rules file.
    const std::string error = download_prefix(url, bytes, limited, characters * 4 + 64);
    if (!error.empty()) { SourcePreview result; result.error = error; return result; }
    return format_source_preview(bytes, limited, characters);
}
