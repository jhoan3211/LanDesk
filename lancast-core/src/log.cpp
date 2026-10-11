#include "lancast/core/log.hpp"

#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>

namespace lancast::log {
namespace {

struct State {
    std::mutex mutex;
    std::deque<Entry> history;
    std::ofstream file;
    Level min_level = Level::Info;
};

State& state() {
    static State s;
    return s;
}

constexpr std::size_t kHistory = 500;

}  // namespace

std::string_view level_name(Level level) noexcept {
    switch (level) {
        case Level::Debug:
            return "DEBUG";
        case Level::Info:
            return "INFO";
        case Level::Warn:
            return "WARN";
        case Level::Error:
            return "ERROR";
    }
    return "?";
}

void set_file(const std::filesystem::path& path) {
    State& s = state();
    std::scoped_lock lock(s.mutex);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    // Rotación simple: si el log pasa de 5 MB se reemplaza por uno nuevo.
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 5u * 1024 * 1024) {
        std::filesystem::rename(path, std::filesystem::path{path}.replace_extension(".old.log"), ec);
    }
    s.file.open(path, std::ios::app);
}

void set_min_level(Level level) {
    State& s = state();
    std::scoped_lock lock(s.mutex);
    s.min_level = level;
}

void write(Level level, std::string message) {
    State& s = state();
    const auto now = std::chrono::system_clock::now();
    std::scoped_lock lock(s.mutex);
    if (level < s.min_level) return;

    if (s.file.is_open()) {
        const auto local = std::chrono::zoned_time{std::chrono::current_zone(), std::chrono::floor<std::chrono::milliseconds>(now)};
        s.file << std::format("{:%Y-%m-%d %H:%M:%S} [{}] {}\n", local, level_name(level), message);
        s.file.flush();
    }
#ifndef NDEBUG
    std::fprintf(stderr, "[%s] %s\n", level_name(level).data(), message.c_str());
#endif
    s.history.push_back(Entry{now, level, std::move(message)});
    if (s.history.size() > kHistory) s.history.pop_front();
}

std::vector<Entry> recent(std::size_t max) {
    State& s = state();
    std::scoped_lock lock(s.mutex);
    const std::size_t n = std::min(max, s.history.size());
    return {s.history.end() - static_cast<std::ptrdiff_t>(n), s.history.end()};
}

}  // namespace lancast::log
