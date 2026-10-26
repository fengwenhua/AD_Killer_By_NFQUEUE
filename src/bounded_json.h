#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Small bounded DOM for untrusted release documents. JSON grammar, UTF-8 and
// surrogate pairs are checked even in unused fields. No duplicate object keys,
// partial documents, floating-point coercion or unbounded recursion/allocation.
namespace bounded_json {
constexpr size_t MAX_BYTES = 512u * 1024u, MAX_NODES = 25000, MAX_DEPTH = 32;
enum class Kind { NIL, BOOLEAN, NUMBER, STRING, ARRAY, OBJECT };
struct Value {
    Kind kind = Kind::NIL;
    bool boolean = false;
    std::string text;
    std::vector<Value> items;
    std::map<std::string, Value> fields;
    const Value *get(const char *key) const {
        const auto found = fields.find(key);
        return kind == Kind::OBJECT && found != fields.end() ? &found->second : nullptr;
    }
    bool integer(uint64_t &number) const {
        number = 0;
        if (kind != Kind::NUMBER || text.empty()) return false;
        for (unsigned char ch : text) {
            if (ch < '0' || ch > '9' || number > (UINT64_MAX - (ch - '0')) / 10) return false;
            number = number * 10 + ch - '0';
        }
        return true;
    }
};
class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}
    bool parse(Value &out) {
        Value candidate;
        if (input_.size() > MAX_BYTES || !value(candidate, 0)) return false;
        spaces();
        if (at_ != input_.size()) return false;
        out = std::move(candidate); return true;
    }
private:
    std::string_view input_;
    size_t at_ = 0, nodes_ = 0;
    void spaces() { while (at_ < input_.size() && (input_[at_] == ' ' || input_[at_] == '\t' || input_[at_] == '\r' || input_[at_] == '\n')) ++at_; }
    bool take(char ch) { if (at_ == input_.size() || input_[at_] != ch) return false; ++at_; return true; }
    bool literal(std::string_view word) {
        if (input_.substr(at_, word.size()) != word) return false;
        at_ += word.size(); return true;
    }
    bool hex(uint32_t &point) {
        point = 0;
        if (input_.size() - at_ < 4) return false;
        for (unsigned i = 0; i < 4; ++i) {
            const char ch = input_[at_++];
            const int digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (digit < 0) return false;
            point = point * 16 + static_cast<unsigned>(digit);
        }
        return true;
    }
    static void encode(std::string &out, uint32_t point) {
        if (point < 0x80) out += static_cast<char>(point);
        else if (point < 0x800) { out += static_cast<char>(0xc0 | point >> 6); out += static_cast<char>(0x80 | (point & 63)); }
        else if (point < 0x10000) { out += static_cast<char>(0xe0 | point >> 12); out += static_cast<char>(0x80 | ((point >> 6) & 63)); out += static_cast<char>(0x80 | (point & 63)); }
        else { out += static_cast<char>(0xf0 | point >> 18); out += static_cast<char>(0x80 | ((point >> 12) & 63)); out += static_cast<char>(0x80 | ((point >> 6) & 63)); out += static_cast<char>(0x80 | (point & 63)); }
    }
    bool string(std::string &out) {
        if (!take('"')) return false;
        while (at_ < input_.size()) {
            const auto ch = static_cast<unsigned char>(input_[at_++]);
            if (ch == '"') return true;
            if (ch < 0x20) return false;
            if (ch == '\\') {
                if (at_ == input_.size()) return false;
                const char escape = input_[at_++];
                if (escape == '"' || escape == '\\' || escape == '/') out += escape;
                else if (escape == 'b') out += '\b';
                else if (escape == 'f') out += '\f';
                else if (escape == 'n') out += '\n';
                else if (escape == 'r') out += '\r';
                else if (escape == 't') out += '\t';
                else if (escape == 'u') {
                    uint32_t point = 0;
                    if (!hex(point)) return false;
                    if (point >= 0xd800 && point <= 0xdbff) {
                        uint32_t low = 0;
                        if (!take('\\') || !take('u') || !hex(low) || low < 0xdc00 || low > 0xdfff) return false;
                        point = 0x10000 + ((point - 0xd800) << 10) + low - 0xdc00;
                    } else if (point >= 0xdc00 && point <= 0xdfff) return false;
                    encode(out, point);
                } else return false;
            } else if (ch < 0x80) out += static_cast<char>(ch);
            else {
                const size_t length = ch >= 0xc2 && ch <= 0xdf ? 2 : ch >= 0xe0 && ch <= 0xef ? 3 : ch >= 0xf0 && ch <= 0xf4 ? 4 : 0;
                if (!length || input_.size() - at_ < length - 1) return false;
                uint32_t point = ch & ((1u << (7 - length)) - 1);
                const size_t begin = at_ - 1;
                for (size_t i = 1; i < length; ++i) {
                    const auto next = static_cast<unsigned char>(input_[at_++]);
                    if ((next & 0xc0) != 0x80) return false;
                    point = (point << 6) | (next & 63);
                }
                if ((length == 2 && point < 0x80) || (length == 3 && point < 0x800) || (length == 4 && point < 0x10000) ||
                    point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
                out.append(input_.substr(begin, length));
            }
        }
        return false;
    }
    bool number(std::string &out) {
        const size_t begin = at_;
        take('-');
        if (!take('0')) {
            if (at_ == input_.size() || input_[at_] < '1' || input_[at_] > '9') return false;
            while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9') ++at_;
        }
        const auto digits = [&] {
            const auto start = at_;
            while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9') ++at_;
            return at_ != start;
        };
        if (take('.') && !digits()) return false;
        if (at_ < input_.size() && (input_[at_] == 'e' || input_[at_] == 'E')) {
            ++at_; if (!take('+')) take('-');
            if (!digits()) return false;
        }
        out.assign(input_.substr(begin, at_ - begin)); return true;
    }
    bool value(Value &out, size_t depth) {
        if (++nodes_ > MAX_NODES || depth > MAX_DEPTH) return false;
        spaces(); if (at_ == input_.size()) return false;
        switch (input_[at_]) {
        case 'n': return literal("null");
        case 't': out.kind = Kind::BOOLEAN; out.boolean = true; return literal("true");
        case 'f': out.kind = Kind::BOOLEAN; return literal("false");
        case '"': out.kind = Kind::STRING; return string(out.text);
        case '[':
            out.kind = Kind::ARRAY; ++at_; spaces(); if (take(']')) return true;
            while (true) {
                Value item; if (!value(item, depth + 1)) return false;
                out.items.push_back(std::move(item)); spaces();
                if (take(']')) return true;
                if (!take(',')) return false;
            }
        case '{':
            out.kind = Kind::OBJECT; ++at_; spaces(); if (take('}')) return true;
            while (true) {
                spaces(); std::string key; if (!string(key)) return false;
                spaces(); if (!take(':')) return false;
                Value item; if (!value(item, depth + 1) || !out.fields.emplace(std::move(key), std::move(item)).second) return false;
                spaces(); if (take('}')) return true;
                if (!take(',')) return false;
            }
        default: out.kind = Kind::NUMBER; return number(out.text);
        }
    }
};
inline bool parse(std::string_view input, Value &out) { return Parser(input).parse(out); }
} // namespace bounded_json
