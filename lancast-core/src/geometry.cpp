#include "lancast/core/geometry.hpp"

#include <algorithm>
#include <cmath>

namespace lancast {

Size fit_stream_size(Size source, Size box) noexcept {
    if (source.width == 0 || source.height == 0) return {0, 0};
    if (box.width == 0 || box.height == 0) box = source;
    const double scale = std::min({1.0, static_cast<double>(box.width) / source.width,
                                   static_cast<double>(box.height) / source.height});
    auto even = [](double v) {
        auto i = static_cast<std::uint32_t>(std::lround(v));
        i &= ~1u;
        return std::max<std::uint32_t>(i, 2);
    };
    return {even(source.width * scale), even(source.height * scale)};
}

Rect view_rect(Size stream, Size window, ScalingMode mode) noexcept {
    if (stream.width == 0 || stream.height == 0 || window.width == 0 || window.height == 0) return {};
    const auto ww = static_cast<double>(window.width);
    const auto wh = static_cast<double>(window.height);
    double w = 0;
    double h = 0;
    switch (mode) {
        case ScalingMode::Stretch:
            return Rect{0, 0, static_cast<std::int32_t>(window.width), static_cast<std::int32_t>(window.height)};
        case ScalingMode::Native:
            w = stream.width;
            h = stream.height;
            break;
        case ScalingMode::Fit: {
            const double s = std::min(ww / stream.width, wh / stream.height);
            w = stream.width * s;
            h = stream.height * s;
            break;
        }
    }
    const auto iw = static_cast<std::int32_t>(std::lround(w));
    const auto ih = static_cast<std::int32_t>(std::lround(h));
    return Rect{static_cast<std::int32_t>(std::lround((ww - w) / 2)), static_cast<std::int32_t>(std::lround((wh - h) / 2)),
                iw, ih};
}

std::optional<PointF> window_to_normalized(PointF p, const Rect& view) noexcept {
    if (view.width <= 0 || view.height <= 0) return std::nullopt;
    const float nx = (p.x - static_cast<float>(view.x)) / static_cast<float>(view.width);
    const float ny = (p.y - static_cast<float>(view.y)) / static_cast<float>(view.height);
    if (nx < 0.f || ny < 0.f || nx > 1.f || ny > 1.f) return std::nullopt;
    return PointF{nx, ny};
}

PointF normalized_to_desktop(PointF n, const Rect& source) noexcept {
    const float nx = std::clamp(n.x, 0.f, 1.f);
    const float ny = std::clamp(n.y, 0.f, 1.f);
    // El último píxel válido es width - 1.
    return PointF{static_cast<float>(source.x) + nx * static_cast<float>(std::max(source.width - 1, 0)),
                  static_cast<float>(source.y) + ny * static_cast<float>(std::max(source.height - 1, 0))};
}

AbsolutePoint desktop_to_absolute(PointF d, const Rect& vs) noexcept {
    if (vs.width <= 1 || vs.height <= 1) return {};
    // Fórmula de SendInput: píxel = valor * (ancho - 1) / 65535 (+ origen del escritorio virtual).
    const double ax = (static_cast<double>(d.x) - vs.x) * 65535.0 / (vs.width - 1);
    const double ay = (static_cast<double>(d.y) - vs.y) * 65535.0 / (vs.height - 1);
    return AbsolutePoint{static_cast<std::int32_t>(std::clamp(std::lround(ax), 0L, 65535L)),
                         static_cast<std::int32_t>(std::clamp(std::lround(ay), 0L, 65535L))};
}

}  // namespace lancast
