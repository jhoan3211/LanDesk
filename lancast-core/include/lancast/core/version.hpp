#pragma once

#include <compare>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#ifndef LANCAST_VERSION
#define LANCAST_VERSION "0.0.0"
#endif

namespace lancast {

// Versión de esta compilación (viene de project(VERSION ...) en CMake).
inline constexpr std::string_view kAppVersion = LANCAST_VERSION;
// Repositorio de GitHub donde se publican las versiones (actualización automática).
inline constexpr std::string_view kUpdateRepo = "jhoan3211/LanDesk";

struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    friend auto operator<=>(const Version&, const Version&) = default;
};

// "v1.3.0" o "1.3.0".
[[nodiscard]] inline std::optional<Version> parse_version(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    Version v;
    std::uint32_t* parts[] = {&v.major, &v.minor, &v.patch};
    std::size_t part = 0;
    bool digit = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            *parts[part] = *parts[part] * 10 + static_cast<std::uint32_t>(c - '0');
            digit = true;
        } else if (c == '.' && digit && part < 2) {
            ++part;
            digit = false;
        } else {
            return std::nullopt;
        }
    }
    if (!digit || part != 2) return std::nullopt;
    return v;
}

[[nodiscard]] inline std::string to_string(const Version& v) { return std::format("{}.{}.{}", v.major, v.minor, v.patch); }

}  // namespace lancast
