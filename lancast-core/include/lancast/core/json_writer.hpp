#pragma once

// Escritor JSON mínimo (solo salida) para `--list --json`, sin dependencias externas.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lancast {

class JsonWriter {
public:
    explicit JsonWriter(int indent = 2) : indent_(indent) {}

    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(std::string_view name);

    JsonWriter& value(std::string_view v);
    JsonWriter& value(const char* v) { return value(std::string_view{v}); }
    JsonWriter& value(bool v);
    JsonWriter& value(std::int64_t v);
    JsonWriter& value(std::uint64_t v);
    JsonWriter& value(std::int32_t v) { return value(static_cast<std::int64_t>(v)); }
    JsonWriter& value(std::uint32_t v) { return value(static_cast<std::uint64_t>(v)); }
    JsonWriter& value(double v);
    JsonWriter& null();

    template <typename T>
    JsonWriter& field(std::string_view name, const T& v) {
        key(name);
        return value(v);
    }

    [[nodiscard]] const std::string& str() const noexcept { return out_; }

    static std::string escape(std::string_view s);

private:
    void before_value();
    void newline();

    struct Level {
        bool is_object;
        bool empty;
    };

    std::string out_;
    std::vector<Level> stack_;
    int indent_;
    bool after_key_ = false;
};

}  // namespace lancast
