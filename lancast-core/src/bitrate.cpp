#include "lancast/core/bitrate.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace lancast {
namespace {

// Recta bitrate(fps) para una resolución de referencia, definida por dos puntos de la tabla.
struct Anchor {
    double pixels;
    double fps_a;
    double mbps_a;
    double fps_b;
    double mbps_b;

    [[nodiscard]] double at(double fps) const noexcept {
        return mbps_a + (fps - fps_a) * (mbps_b - mbps_a) / (fps_b - fps_a);
    }
};

// 1080p solo tiene un punto (60 fps → 50 Mbps): proporcional a los FPS (pasa por el origen).
constexpr std::array<Anchor, 3> kAnchors{{
    {1920.0 * 1080.0, 0.0, 0.0, 60.0, 50.0},
    {2560.0 * 1440.0, 60.0, 85.0, 144.0, 200.0},
    {3840.0 * 2160.0, 60.0, 150.0, 144.0, 350.0},
}};

constexpr double kH264Factor = 1.4;

double reference_mbps(double pixels, double fps) noexcept {
    const Anchor& first = kAnchors.front();
    const Anchor& last = kAnchors.back();
    if (pixels <= first.pixels) return first.at(fps) * pixels / first.pixels;
    if (pixels >= last.pixels) return last.at(fps) * pixels / last.pixels;

    for (std::size_t i = 0; i + 1 < kAnchors.size(); ++i) {
        const Anchor& lo = kAnchors[i];
        const Anchor& hi = kAnchors[i + 1];
        if (pixels <= hi.pixels) {
            const double t = (pixels - lo.pixels) / (hi.pixels - lo.pixels);
            return lo.at(fps) + t * (hi.at(fps) - lo.at(fps));
        }
    }
    return last.at(fps);
}

}  // namespace

std::uint32_t auto_bitrate_mbps(std::uint32_t width, std::uint32_t height, double fps, Codec codec) noexcept {
    if (width == 0 || height == 0 || !(fps > 0.0)) return kMinBitrateMbps;

    const double pixels = static_cast<double>(width) * static_cast<double>(height);
    double mbps = reference_mbps(pixels, fps);
    if (codec == Codec::H264) mbps *= kH264Factor;

    const double clamped =
        std::clamp(std::round(mbps), static_cast<double>(kMinBitrateMbps), static_cast<double>(kMaxBitrateMbps));
    return static_cast<std::uint32_t>(clamped);
}

}  // namespace lancast
