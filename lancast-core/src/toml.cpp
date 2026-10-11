#include "lancast/core/toml.hpp"

#include <charconv>
#include <cmath>
#include <format>

namespace lancast::toml {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string escape(std::string_view s) {
    std::string out = "\"";
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
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

// Lee un string entre comillas desde s[pos] (que debe ser '"'). Avanza pos tras la comilla final.
std::optional<std::string> parse_quoted(std::string_view s, std::size_t& pos) {
    if (pos >= s.size() || s[pos] != '"') return std::nullopt;
    std::string out;
    for (++pos; pos < s.size(); ++pos) {
        const char c = s[pos];
        if (c == '"') {
            ++pos;
            return out;
        }
        if (c == '\\' && pos + 1 < s.size()) {
            const char e = s[++pos];
            switch (e) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                default:
                    out.push_back(e);
            }
        } else {
            out.push_back(c);
        }
    }
    return std::nullopt;
}

std::optional<Value> parse_value(std::string_view s) {
    s = trim(s);
    if (s.empty()) return std::nullopt;
    if (s.front() == '"') {
        std::size_t pos = 0;
        return parse_quoted(s, pos);
    }
    if (s.front() == '[') {
        std::vector<std::string> items;
        std::size_t pos = 1;
        while (pos < s.size()) {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == ',' || s[pos] == '\t')) ++pos;
            if (pos < s.size() && s[pos] == ']') return items;
            auto item = parse_quoted(s, pos);
            if (!item) return std::nullopt;
            items.push_back(std::move(*item));
        }
        return std::nullopt;
    }
    if (s == "true") return true;
    if (s == "false") return false;

    const bool is_float = s.find_first_of(".eE") != std::string_view::npos;
    if (is_float) {
        double d = 0;
        const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), d);
        if (ec == std::errc{} && p == s.data() + s.size()) return d;
        return std::nullopt;
    }
    std::int64_t i = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), i);
    if (ec == std::errc{} && p == s.data() + s.size()) return i;
    return std::nullopt;
}

// Quita el comentario final respetando comillas.
std::string_view strip_comment(std::string_view line) {
    bool in_string = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && in_string) {
            ++i;
        } else if (line[i] == '"') {
            in_string = !in_string;
        } else if (line[i] == '#' && !in_string) {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string format_value(const Value& v) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return escape(x);
            } else if constexpr (std::is_same_v<T, bool>) {
                return x ? "true" : "false";
            } else if constexpr (std::is_same_v<T, double>) {
                std::string s = std::format("{}", x);
                if (s.find_first_of(".eE") == std::string::npos && std::isfinite(x)) s += ".0";
                return s;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return std::to_string(x);
            } else {
                std::string out = "[";
                for (std::size_t i = 0; i < x.size(); ++i) {
                    if (i > 0) out += ", ";
                    out += escape(x[i]);
                }
                return out + "]";
            }
        },
        v);
}

void write_table(std::string& out, const Table& t) {
    for (const auto& [k, v] : t.values) out += std::format("{} = {}\n", k, format_value(v));
}

}  // namespace

std::optional<std::string> Table::get_string(const std::string& key) const {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    if (const auto* s = std::get_if<std::string>(&it->second)) return *s;
    return std::nullopt;
}

std::optional<std::int64_t> Table::get_int(const std::string& key) const {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    if (const auto* i = std::get_if<std::int64_t>(&it->second)) return *i;
    if (const auto* d = std::get_if<double>(&it->second)) return static_cast<std::int64_t>(*d);
    return std::nullopt;
}

std::optional<double> Table::get_double(const std::string& key) const {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    if (const auto* d = std::get_if<double>(&it->second)) return *d;
    if (const auto* i = std::get_if<std::int64_t>(&it->second)) return static_cast<double>(*i);
    return std::nullopt;
}

std::optional<bool> Table::get_bool(const std::string& key) const {
    const auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    if (const auto* b = std::get_if<bool>(&it->second)) return *b;
    return std::nullopt;
}

std::vector<std::string> Table::get_strings(const std::string& key) const {
    const auto it = values.find(key);
    if (it == values.end()) return {};
    if (const auto* v = std::get_if<std::vector<std::string>>(&it->second)) return *v;
    return {};
}

const Table* Document::table(const std::string& name) const {
    const auto it = tables.find(name);
    return it == tables.end() ? nullptr : &it->second;
}

Document Document::parse(std::string_view text, std::string* error) {
    Document doc;
    Table* current = &doc.tables[""];
    int line_no = 0;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);  // BOM UTF-8 (Bloc de notas, PowerShell 5).
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++line_no;

        line = trim(strip_comment(line));
        if (line.empty()) continue;

        if (line.starts_with("[[") && line.ends_with("]]")) {
            const std::string name{trim(line.substr(2, line.size() - 4))};
            auto& arr = doc.arrays[name];
            arr.emplace_back();
            current = &arr.back();
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            current = &doc.tables[std::string{trim(line.substr(1, line.size() - 2))}];
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (error) *error = std::format("línea {}: se esperaba 'clave = valor'", line_no);
            continue;
        }
        std::string key{trim(line.substr(0, eq))};
        if (key.size() >= 2 && key.front() == '"' && key.back() == '"') key = key.substr(1, key.size() - 2);
        if (auto value = parse_value(line.substr(eq + 1))) {
            current->values[key] = std::move(*value);
        } else if (error) {
            *error = std::format("línea {}: valor inválido para '{}'", line_no, key);
        }
    }
    return doc;
}

std::string Document::serialize() const {
    std::string out;
    if (const Table* root = table(""); root != nullptr) write_table(out, *root);
    for (const auto& [name, t] : tables) {
        if (name.empty()) continue;
        out += std::format("\n[{}]\n", name);
        write_table(out, t);
    }
    for (const auto& [name, arr] : arrays) {
        for (const Table& t : arr) {
            out += std::format("\n[[{}]]\n", name);
            write_table(out, t);
        }
    }
    return out;
}

}  // namespace lancast::toml
