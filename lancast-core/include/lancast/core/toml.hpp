#pragma once

// Subconjunto de TOML suficiente para la configuración de LanCast:
// tablas [a.b], arrays de tablas [[a.b]], y valores string / entero / decimal / bool / array de strings.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lancast::toml {

using Value = std::variant<std::string, std::int64_t, double, bool, std::vector<std::string>>;

struct Table {
    std::map<std::string, Value> values;

    [[nodiscard]] std::optional<std::string> get_string(const std::string& key) const;
    [[nodiscard]] std::optional<std::int64_t> get_int(const std::string& key) const;
    [[nodiscard]] std::optional<double> get_double(const std::string& key) const;
    [[nodiscard]] std::optional<bool> get_bool(const std::string& key) const;
    [[nodiscard]] std::vector<std::string> get_strings(const std::string& key) const;

    void set(const std::string& key, Value value) { values[key] = std::move(value); }
};

struct Document {
    std::map<std::string, Table> tables;                // "" = raíz
    std::map<std::string, std::vector<Table>> arrays;   // [[nombre]]

    [[nodiscard]] const Table* table(const std::string& name) const;
    Table& table_mut(const std::string& name) { return tables[name]; }

    static Document parse(std::string_view text, std::string* error = nullptr);
    [[nodiscard]] std::string serialize() const;
};

}  // namespace lancast::toml
