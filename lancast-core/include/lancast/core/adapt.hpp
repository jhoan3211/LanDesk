#pragma once

// Ajuste automático de calidad: cuándo el codificador del host "no da abasto" y cuál es el siguiente escalón.

#include <algorithm>
#include <cstdint>
#include <optional>

#include "lancast/core/types.hpp"

namespace lancast::adapt {

// Una ventana de estadísticas (1 s) está sobrecargada si el codificador tarda más que el intervalo entre
// cuadros o si se saltan más del 10 % de los cuadros. Con pocos cuadros (pantalla quieta) no se juzga.
[[nodiscard]] inline bool overloaded(double encode_ms, double interval_ms, double skipped_percent,
                                     std::uint64_t frames) noexcept {
    return frames >= 10 && interval_ms > 0 && (encode_ms > interval_ms || skipped_percent > 10.0);
}

struct Step {
    double scale;       // Escala acumulada sobre la resolución pedida.
    std::uint32_t fps;  // 0 = sin cambio.
};

// Primero baja la resolución (máx. 20 % por paso, hasta 720p) y después los FPS (25 % por paso, hasta 30).
[[nodiscard]] inline std::optional<Step> next_step(Size stream, std::uint32_t fps, double scale) noexcept {
    if (stream.height > 720) {
        const double factor = std::max(0.8, 720.0 / static_cast<double>(stream.height));
        return Step{scale * factor, 0};
    }
    if (fps > 60) return Step{scale, std::max<std::uint32_t>(60, fps * 3 / 4)};
    if (fps > 30) return Step{scale, std::max<std::uint32_t>(30, fps * 3 / 4)};
    return std::nullopt;
}

}  // namespace lancast::adapt
