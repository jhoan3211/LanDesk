#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include "lancast/core/audio_resample.hpp"

using lancast::audio::Resampler;

namespace {

std::vector<std::int16_t> sine(std::uint32_t rate, double freq, std::size_t frames, std::size_t offset = 0) {
    std::vector<std::int16_t> out(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<std::int16_t>(10000 * std::sin(2 * std::numbers::pi * freq * static_cast<double>(i + offset) / rate));
        out[2 * i] = v;
        out[2 * i + 1] = v;
    }
    return out;
}

}  // namespace

TEST(Resampler, PassThroughAndMono) {
    Resampler r;
    r.configure(48000, 48000, 2);
    const std::vector<std::int16_t> in{1, 2, 3, 4, -5, -6};
    std::vector<std::int16_t> out;
    r.process(in, out);
    EXPECT_EQ(out, in);

    r.configure(48000, 48000, 1);
    r.process(in, out);
    EXPECT_EQ(out, (std::vector<std::int16_t>{2, 4, -6}));
}

TEST(Resampler, ProducesExpectedSampleCountAcrossBlocks) {
    Resampler r;
    r.configure(48000, 44100, 2);
    std::vector<std::int16_t> out;
    std::size_t produced = 0;
    for (int block = 0; block < 100; ++block) {  // 100 bloques de 10 ms = 1 s
        r.process(sine(48000, 440, 480, static_cast<std::size_t>(block) * 480), out);
        produced += out.size() / 2;
    }
    EXPECT_NEAR(static_cast<double>(produced), 44100.0, 3.0);
}

TEST(Resampler, KeepsSignalShapeWithoutGlitchesAtBlockBorders) {
    Resampler r;
    r.configure(48000, 24000, 2);
    std::vector<std::int16_t> all;
    std::vector<std::int16_t> out;
    for (int block = 0; block < 20; ++block) {
        r.process(sine(48000, 440, 480, static_cast<std::size_t>(block) * 480), out);
        all.insert(all.end(), out.begin(), out.end());
    }
    // Comparar contra el seno ideal a 24 kHz.
    const auto ideal = sine(24000, 440, all.size() / 2);
    double max_err = 0;
    for (std::size_t i = 0; i < all.size(); i += 2) max_err = std::max(max_err, std::abs(static_cast<double>(all[i]) - static_cast<double>(ideal[i])));
    EXPECT_LT(max_err, 120.0);  // < 1.2 % de la amplitud
}
