#pragma once

#include <cstddef>
#include <string>
#include <string_view>

struct HttpValidators {
    std::string etag, last_modified;
    bool empty() const { return etag.empty() && last_modified.empty(); }
};

inline bool safe_http_value(std::string_view value, size_t maximum) {
    if (value.empty() || value.size() > maximum) return false;
    for (unsigned char ch : value) if (ch < 0x20 || ch > 0x7e) return false;
    return true;
}
inline bool valid_etag(std::string_view value) {
    if (!safe_http_value(value, 256)) return false;
    if (value.substr(0, 2) == "W/") value.remove_prefix(2);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return false;
    value.remove_prefix(1); value.remove_suffix(1);
    for (unsigned char ch : value) if (ch == '"' || ch == ' ') return false;
    return true;
}
inline bool valid_http_validators(const HttpValidators &validators) {
    return (validators.etag.empty() || valid_etag(validators.etag)) &&
           (validators.last_modified.empty() || safe_http_value(validators.last_modified, 128));
}

// One bounded header line at a time, shared by the library callback and curl
// stderr capture. Redirect/auth/1xx blocks replace validators; trailers never
// replace the selected representation's validators. Duplicates are unusable.
class HttpResponseHeaders {
public:
    static constexpr size_t MAX_BYTES = 16384;
    bool consume(std::string_view line) {
        if (failed_ || line.size() > MAX_BYTES - bytes_) { failed_ = true; return false; }
        bytes_ += line.size();
        if (line.empty() || line.back() != '\n') { failed_ = true; return false; }
        line.remove_suffix(1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        for (unsigned char ch : line) if ((ch < 0x20 && ch != '\t') || ch == 0x7f) { failed_ = true; return false; }
        if (line.substr(0, 5) == "HTTP/") {
            const auto space = line.find(' ');
            if (space == std::string_view::npos || space + 4 > line.size() ||
                (space + 4 < line.size() && line[space + 4] != ' ')) { failed_ = true; return false; }
            const auto code = line.substr(space + 1, 3);
            if (code.find_first_not_of("0123456789") != std::string_view::npos) { failed_ = true; return false; }
            status_ = (code[0] - '0') * 100 + (code[1] - '0') * 10 + code[2] - '0';
            if (status_ < 100 || status_ > 599) { failed_ = true; return false; }
            validators_ = {}; seen_etag_ = seen_modified_ = false; in_headers_ = true; complete_ = false;
            return true;
        }
        if (!in_headers_) return true; // ignore trailers/curl diagnostic text
        if (line.empty()) { in_headers_ = false; complete_ = true; return true; }
        if (line.front() == ' ' || line.front() == '\t') { failed_ = true; return false; }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) { failed_ = true; return false; }
        std::string name(line.substr(0, colon));
        for (char &ch : name) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        auto value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        if (name == "etag") {
            if (seen_etag_ || !valid_etag(value)) validators_.etag.clear();
            else validators_.etag = value;
            seen_etag_ = true;
        } else if (name == "last-modified") {
            if (seen_modified_ || !safe_http_value(value, 128)) validators_.last_modified.clear();
            else validators_.last_modified = value;
            seen_modified_ = true;
        }
        return true;
    }
    bool valid() const { return !failed_ && complete_; }
    int status() const { return status_; }
    const HttpValidators &validators() const { return validators_; }
private:
    size_t bytes_ = 0;
    int status_ = 0;
    bool failed_ = false, complete_ = false, in_headers_ = false;
    bool seen_etag_ = false, seen_modified_ = false;
    HttpValidators validators_;
};
