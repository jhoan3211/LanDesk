#include "lancast/core/json_reader.hpp"

#include <charconv>
#include <cstdint>

namespace lancast::json {
namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    std::optional<Value> run() {
        auto v = value(0);
        skip_ws();
        if (!v || pos_ != s_.size()) return std::nullopt;
        return v;
    }

private:
    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }

    bool consume(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    std::optional<Value> value(int depth) {
        if (depth > kMaxDepth) return std::nullopt;
        skip_ws();
        if (pos_ >= s_.size()) return std::nullopt;
        switch (s_[pos_]) {
            case '{':
                return object(depth);
            case '[':
                return array(depth);
            case '"': {
                auto str = string();
                if (!str) return std::nullopt;
                return Value(std::move(*str));
            }
            case 't':
                return consume("true") ? std::optional<Value>(Value(true)) : std::nullopt;
            case 'f':
                return consume("false") ? std::optional<Value>(Value(false)) : std::nullopt;
            case 'n':
                return consume("null") ? std::optional<Value>(Value(nullptr)) : std::nullopt;
            default:
                return number();
        }
    }

    std::optional<Value> number() {
        const std::size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' ||
                                    s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) {
            ++pos_;
        }
        double d = 0;
        const auto [p, ec] = std::from_chars(s_.data() + start, s_.data() + pos_, d);
        if (ec != std::errc{} || p != s_.data() + pos_ || pos_ == start) return std::nullopt;
        return Value(d);
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::optional<std::uint32_t> hex4() {
        if (pos_ + 4 > s_.size()) return std::nullopt;
        std::uint32_t v = 0;
        const auto [p, ec] = std::from_chars(s_.data() + pos_, s_.data() + pos_ + 4, v, 16);
        if (ec != std::errc{} || p != s_.data() + pos_ + 4) return std::nullopt;
        pos_ += 4;
        return v;
    }

    std::optional<std::string> string() {
        ++pos_;  // '"'
        std::string out;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) return std::nullopt;
            const char e = s_[pos_++];
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(e);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    auto cp = hex4();
                    if (!cp) return std::nullopt;
                    if (*cp >= 0xD800 && *cp <= 0xDBFF) {  // Par sustituto UTF-16.
                        if (!consume("\\u")) return std::nullopt;
                        auto lo = hex4();
                        if (!lo || *lo < 0xDC00 || *lo > 0xDFFF) return std::nullopt;
                        *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*lo - 0xDC00);
                    }
                    append_utf8(out, *cp);
                    break;
                }
                default:
                    return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> array(int depth) {
        ++pos_;  // '['
        Array out;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return Value(std::move(out));
        }
        while (true) {
            auto v = value(depth + 1);
            if (!v) return std::nullopt;
            out.push_back(std::move(*v));
            skip_ws();
            if (pos_ >= s_.size()) return std::nullopt;
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == ']') {
                ++pos_;
                return Value(std::move(out));
            }
            return std::nullopt;
        }
    }

    std::optional<Value> object(int depth) {
        ++pos_;  // '{'
        Object out;
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return Value(std::move(out));
        }
        while (true) {
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') return std::nullopt;
            auto key = string();
            if (!key) return std::nullopt;
            skip_ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return std::nullopt;
            ++pos_;
            auto v = value(depth + 1);
            if (!v) return std::nullopt;
            out.insert_or_assign(std::move(*key), std::move(*v));
            skip_ws();
            if (pos_ >= s_.size()) return std::nullopt;
            if (s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (s_[pos_] == '}') {
                ++pos_;
                return Value(std::move(out));
            }
            return std::nullopt;
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

const Value& null_value() {
    static const Value v;
    return v;
}

}  // namespace

std::optional<double> Value::number() const noexcept {
    if (const auto* d = std::get_if<double>(&data_)) return *d;
    return std::nullopt;
}

std::optional<bool> Value::boolean() const noexcept {
    if (const auto* b = std::get_if<bool>(&data_)) return *b;
    return std::nullopt;
}

const Array* Value::array() const noexcept {
    if (const auto* a = std::get_if<std::shared_ptr<Array>>(&data_)) return a->get();
    return nullptr;
}

const Object* Value::object() const noexcept {
    if (const auto* o = std::get_if<std::shared_ptr<Object>>(&data_)) return o->get();
    return nullptr;
}

const Value& Value::operator[](std::string_view key) const noexcept {
    const Object* o = object();
    if (o == nullptr) return null_value();
    const auto it = o->find(key);
    return it == o->end() ? null_value() : it->second;
}

std::optional<Value> parse(std::string_view text) { return Parser(text).run(); }

}  // namespace lancast::json
