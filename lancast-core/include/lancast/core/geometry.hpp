#pragma once

// Geometría del stream: tamaño de codificación, rectángulo de presentación y mapeo de coordenadas
// ventana del cliente -> stream -> escritorio virtual del host -> coordenadas absolutas de SendInput.

#include <cstdint>
#include <optional>

#include "lancast/core/settings.hpp"
#include "lancast/core/types.hpp"

namespace lancast {

struct PointF {
    float x = 0;
    float y = 0;
};

// Mayor tamaño (par) que cabe en `box` manteniendo la proporción de `source`. Nunca agranda.
[[nodiscard]] Size fit_stream_size(Size source, Size box) noexcept;

// Rectángulo (en píxeles de la ventana) donde se dibuja el stream según el modo de escalado.
[[nodiscard]] Rect view_rect(Size stream, Size window, ScalingMode mode) noexcept;

// Punto de la ventana -> coordenadas normalizadas [0,1] del stream. nullopt si cae fuera de la imagen.
[[nodiscard]] std::optional<PointF> window_to_normalized(PointF window_point, const Rect& view) noexcept;

// Coordenadas normalizadas del stream -> píxel del escritorio virtual del host.
[[nodiscard]] PointF normalized_to_desktop(PointF normalized, const Rect& source) noexcept;

// Píxel del escritorio virtual -> coordenadas absolutas 0..65535 de SendInput (MOUSEEVENTF_VIRTUALDESK).
struct AbsolutePoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
};
[[nodiscard]] AbsolutePoint desktop_to_absolute(PointF desktop, const Rect& virtual_screen) noexcept;

}  // namespace lancast
