#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lancast {

[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> data);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> from_hex(std::string_view hex);
[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> data);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text);

// "1.5 GB", "320 MB/s"...
[[nodiscard]] std::string human_bytes(std::uint64_t bytes);
[[nodiscard]] std::string human_duration(double seconds);

}  // namespace lancast
