#pragma once

// Logger mínimo, thread-safe, con archivo opcional y un historial en memoria para la interfaz.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace lancast::log {

enum class Level : std::uint8_t { Debug, Info, Warn, Error };

struct Entry {
    std::chrono::system_clock::time_point time;
    Level level;
    std::string message;
};

void set_file(const std::filesystem::path& path);
void set_min_level(Level level);
void write(Level level, std::string message);

// Últimas `max` entradas (más recientes al final).
[[nodiscard]] std::vector<Entry> recent(std::size_t max);
[[nodiscard]] std::string_view level_name(Level level) noexcept;

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace lancast::log
