#include "lancast/core/json_writer.hpp"

#include <cmath>
#include <cstdio>
#include <format>

namespace lancast {

std::string JsonWriter::escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                } else {
                    out.push_back(c);  // UTF-8 se copia tal cual.
                }
        }
    }
    out.push_back('"');
    return out;
}

void JsonWriter::newline() {
    if (indent_ <= 0) return;
    out_.push_back('\n');
    out_.append(stack_.size() * static_cast<std::size_t>(indent_), ' ');
}

void JsonWriter::before_value() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (!stack_.empty()) {
        if (!stack_.back().empty) out_.push_back(',');
        stack_.back().empty = false;
        newline();
    }
}

JsonWriter& JsonWriter::begin_object() {
    before_value();
    out_.push_back('{');
    stack_.push_back({true, true});
    return *this;
}

JsonWriter& JsonWriter::end_object() {
    const bool empty = stack_.back().empty;
    stack_.pop_back();
    if (!empty) newline();
    out_.push_back('}');
    return *this;
}

JsonWriter& JsonWriter::begin_array() {
    before_value();
    out_.push_back('[');
    stack_.push_back({false, true});
    return *this;
}

JsonWriter& JsonWriter::end_array() {
    const bool empty = stack_.back().empty;
    stack_.pop_back();
    if (!empty) newline();
    out_.push_back(']');
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    before_value();
    out_ += escape(name);
    out_ += indent_ > 0 ? ": " : ":";
    after_key_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view v) {
    before_value();
    out_ += escape(v);
    return *this;
}

JsonWriter& JsonWriter::value(bool v) {
    before_value();
    out_ += v ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t v) {
    before_value();
    out_ += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t v) {
    before_value();
    out_ += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::value(double v) {
    before_value();
    if (std::isfinite(v)) {
        out_ += std::format("{}", v);
    } else {
        out_ += "null";
    }
    return *this;
}

JsonWriter& JsonWriter::null() {
    before_value();
    out_ += "null";
    return *this;
}

}  // namespace lancast
