#pragma once

// Lector JSON mínimo (RFC 8259) para respuestas de servicios web (p. ej. la API de GitHub Releases).

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lancast::json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value, std::less<>>;

class Value {
public:
    Value() = default;
    explicit Value(std::nullptr_t) {}
    explicit Value(bool b) : data_(b) {}
    explicit Value(double d) : data_(d) {}
    explicit Value(std::string s) : data_(std::move(s)) {}
    explicit Value(Array a) : data_(std::make_shared<Array>(std::move(a))) {}
    explicit Value(Object o) : data_(std::make_shared<Object>(std::move(o))) {}

    [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data_); }
    [[nodiscard]] const std::string* string() const noexcept { return std::get_if<std::string>(&data_); }
    [[nodiscard]] std::optional<double> number() const noexcept;
    [[nodiscard]] std::optional<bool> boolean() const noexcept;
    [[nodiscard]] const Array* array() const noexcept;
    [[nodiscard]] const Object* object() const noexcept;

    // Acceso a un campo de objeto; devuelve un valor nulo si no existe.
    [[nodiscard]] const Value& operator[](std::string_view key) const noexcept;
    [[nodiscard]] std::string string_or(std::string fallback) const { return string() ? *string() : std::move(fallback); }

private:
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> data_;
};

// nullopt si el texto no es JSON válido (o supera la profundidad máxima).
[[nodiscard]] std::optional<Value> parse(std::string_view text);

}  // namespace lancast::json
