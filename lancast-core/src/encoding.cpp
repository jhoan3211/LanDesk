#include "lancast/core/encoding.hpp"

#include <format>

namespace lancast {
namespace {

constexpr char kHex[] = "0123456789abcdef";
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int b64_value(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

}  // namespace

std::string to_hex(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::uint8_t b : data) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<std::uint8_t> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = hex_value(hex[2 * i]);
        const int lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return out;
}

std::string base64_encode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
    }
    if (const std::size_t rest = data.size() - i; rest > 0) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (rest == 2) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(rest == 2 ? kB64[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = b64_value(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

std::string human_bytes(std::uint64_t bytes) {
    const auto b = static_cast<double>(bytes);
    if (b >= 1024.0 * 1024 * 1024) return std::format("{:.2f} GB", b / (1024.0 * 1024 * 1024));
    if (b >= 1024.0 * 1024) return std::format("{:.1f} MB", b / (1024.0 * 1024));
    if (b >= 1024.0) return std::format("{:.0f} KB", b / 1024.0);
    return std::format("{} B", bytes);
}

std::string human_duration(double seconds) {
    if (!(seconds >= 0) || seconds > 360000) return "--";
    const auto s = static_cast<long long>(seconds + 0.5);
    if (s >= 3600) return std::format("{}h {:02}m", s / 3600, (s / 60) % 60);
    if (s >= 60) return std::format("{}m {:02}s", s / 60, s % 60);
    return std::format("{}s", s);
}

}  // namespace lancast
