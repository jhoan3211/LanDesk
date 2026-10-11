#pragma once

#include <cstdint>

#include "lancast/core/types.hpp"

namespace lancast {

inline constexpr std::uint32_t kMinBitrateMbps = 5;
inline constexpr std::uint32_t kMaxBitrateMbps = 1000;

// Bitrate sugerido para el modo "Automático".
// Referencia HEVC/AV1: 1080p60 = 50, 1440p60 = 85, 1440p144 = 200, 4K60 = 150, 4K144 = 350 Mbps.
// Escala de forma lineal con los FPS y se interpola por número de píxeles entre resoluciones;
// H.264 usa el valor de referencia × 1.4. El resultado se limita a [5, 1000] Mbps.
[[nodiscard]] std::uint32_t auto_bitrate_mbps(std::uint32_t width, std::uint32_t height, double fps,
                                              Codec codec) noexcept;

}  // namespace lancast
