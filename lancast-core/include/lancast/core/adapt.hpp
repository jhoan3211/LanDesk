#pragma once

// Ajuste automático de calidad: cuándo el codificador del host "no da abasto" y cuál es el siguiente escalón.

#include <algorithm>
#include <cstdint>
#include <optional>

#include "lancast/core/types.hpp"

namespace lancast::adapt {

// Una ventana de estadísticas (1 s) está sobrecargada si el codificador realmente no da abasto: se saltan más del
// 10 % de los cuadros, o los cuadros se acumulan (la latencia pasa de 2,5 intervalos). Que un cuadro tarde más que
// el intervalo NO es sobrecarga: el codificador trabaja en cadena (varios cuadros a la vez) y en una GPU AMD tarda
// ~7-8 ms tanto a 1080p como a 1440p, aun a 165 fps. Con pocos cuadros (pantalla quieta) no se juzga.
[[nodiscard]] inline bool overloaded(double encode_ms, double interval_ms, double skipped_percent,
                                     std::uint64_t frames) noexcept {
    return frames >= 10 && interval_ms > 0 && (skipped_percent > 10.0 || encode_ms > 2.5 * interval_ms);
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

// Recuperación: deshace un escalón (primero los FPS, después la resolución) hasta volver a lo pedido.
struct Level {
    double scale;           // 1 = resolución pedida.
    std::uint32_t fps_cap;  // 0 = sin límite (los FPS pedidos).
};
[[nodiscard]] inline std::optional<Level> restore_step(Level now, std::uint32_t requested_fps) noexcept {
    if (now.fps_cap != 0) {
        const std::uint32_t up = now.fps_cap * 4 / 3 + 1;
        return Level{now.scale, up >= requested_fps ? 0u : up};
    }
    if (now.scale < 0.999) return Level{std::min(1.0, now.scale / 0.8), 0};
    return std::nullopt;  // Ya está en la calidad pedida.
}

// Ventana tranquila: sin sobrecarga y casi sin cuadros saltados (la pantalla quieta también cuenta).
[[nodiscard]] inline bool calm(double encode_ms, double interval_ms, double skipped_percent, std::uint64_t frames) noexcept {
    return !overloaded(encode_ms, interval_ms, skipped_percent, frames) && skipped_percent < 1.0;
}

// Segundos tranquilos antes de subir la calidad: 20; si al subir vuelve a sobrecargarse enseguida, se duplica
// (hasta ~5 min) para no oscilar.
inline constexpr int kRestoreAfterSeconds = 20;
inline constexpr int kRestoreAfterMax = 320;

}  // namespace lancast::adapt
