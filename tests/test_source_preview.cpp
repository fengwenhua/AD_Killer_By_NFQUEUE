#include "source_preview.h"
#include "logging.h"
#include <cassert>
#include <string>

std::string download_prefix(const std::string &, std::string &, bool &, size_t) { return "unit stub"; }

int main() {
    auto plain = format_source_preview(std::string(550, 'x'), false);
    assert(plain.error.empty() && plain.characters == 500 && plain.text.size() == 500 && plain.truncated);
    std::string unicode;
    for (int i = 0; i < 3001; ++i) unicode += "中😀";
    auto text = format_source_preview("\xef\xbb\xbf" + unicode, false);
    assert(text.error.empty() && text.characters == 500 && text.text.size() == 1750 && text.truncated);
    std::string compressed;
    assert(gz_compress_string(unicode, compressed));
    auto gzip = format_source_preview(compressed, false);
    assert(gzip.error.empty() && gzip.text == text.text && gzip.characters == 500);
    for (size_t limit : {100u, 1000u, 5000u}) {
        auto custom = format_source_preview(unicode, false, limit);
        auto packed = format_source_preview(compressed, false, limit);
        assert(custom.error.empty() && custom.characters == limit && custom.truncated);
        assert(packed.error.empty() && packed.text == custom.text && packed.characters == limit);
    }
    for (size_t limit : {0u, 99u, 5001u, 999999u}) {
        assert(!format_source_preview("text", false, limit).error.empty());
        assert(!preview_source("https://example.org", limit).error.empty());
    }
    assert(format_source_preview("", false).characters == 0);
    assert(format_source_preview("hi", false).truncated == false);
    assert(format_source_preview("hi", true).truncated);
    for (const auto &invalid : {std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"), std::string("x\0y", 3), std::string("\xff")})
        assert(!format_source_preview(invalid, false).error.empty());
    assert(!format_source_preview("\xe4\xb8", false).error.empty());
    auto partial = format_source_preview("hi\xe4\xb8", true);
    assert(partial.error.empty() && partial.text == "hi" && partial.truncated);
    assert(!format_source_preview("\x1f\x8b" + std::string(10, 'x'), true).error.empty());
}
