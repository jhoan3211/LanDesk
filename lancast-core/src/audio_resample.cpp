#include "lancast/core/audio_resample.hpp"

#include <algorithm>
#include <cmath>

namespace lancast::audio {

void Resampler::configure(std::uint32_t in_rate, std::uint32_t out_rate, std::uint8_t out_channels) {
    if (in_rate == in_rate_ && out_rate == out_rate_ && out_channels == out_channels_) return;
    in_rate_ = std::max<std::uint32_t>(in_rate, 1);
    out_rate_ = std::max<std::uint32_t>(out_rate, 1);
    out_channels_ = out_channels == 1 ? 1 : 2;
    pos_ = 0.0;
    has_prev_ = false;
}

void Resampler::process(std::span<const std::int16_t> in, std::vector<std::int16_t>& out) {
    out.clear();
    const std::size_t frames = in.size() / 2;
    if (frames == 0) return;

    auto emit = [&](double l, double r) {
        if (out_channels_ == 1) {
            out.push_back(static_cast<std::int16_t>(std::clamp(std::lround((l + r) * 0.5), -32768L, 32767L)));
        } else {
            out.push_back(static_cast<std::int16_t>(std::clamp(std::lround(l), -32768L, 32767L)));
            out.push_back(static_cast<std::int16_t>(std::clamp(std::lround(r), -32768L, 32767L)));
        }
    };

    if (in_rate_ == out_rate_) {
        out.reserve(frames * out_channels_);
        for (std::size_t f = 0; f < frames; ++f) emit(in[2 * f], in[2 * f + 1]);
        return;
    }

    // Frame "virtual" -1 = último del bloque anterior, para que la interpolación sea continua.
    auto sample = [&](std::ptrdiff_t f, int ch) -> double {
        if (f < 0) return has_prev_ ? prev_[ch] : in[static_cast<std::size_t>(ch)];
        return in[static_cast<std::size_t>(f) * 2 + static_cast<std::size_t>(ch)];
    };
    const double step = static_cast<double>(in_rate_) / out_rate_;
    out.reserve(static_cast<std::size_t>(frames / step + 2) * out_channels_);
    double pos = pos_;
    while (pos < static_cast<double>(frames) - 1.0 + 1e-9) {
        const auto i = static_cast<std::ptrdiff_t>(std::floor(pos));
        const double t = pos - static_cast<double>(i);
        emit(sample(i, 0) * (1 - t) + sample(i + 1, 0) * t, sample(i, 1) * (1 - t) + sample(i + 1, 1) * t);
        pos += step;
    }
    pos_ = pos - static_cast<double>(frames);  // Relativa al siguiente bloque (puede ser -1..0).
    prev_[0] = in[(frames - 1) * 2];
    prev_[1] = in[(frames - 1) * 2 + 1];
    has_prev_ = true;
}

}  // namespace lancast::audio
