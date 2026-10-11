#include <gtest/gtest.h>

#include "lancast/core/bitrate.hpp"

using lancast::auto_bitrate_mbps;
using lancast::Codec;

TEST(AutoBitrate, MatchesReferenceTableForHevcAndAv1) {
    for (const Codec codec : {Codec::Hevc, Codec::Av1}) {
        EXPECT_EQ(auto_bitrate_mbps(1920, 1080, 60, codec), 50u);
        EXPECT_EQ(auto_bitrate_mbps(2560, 1440, 60, codec), 85u);
        EXPECT_EQ(auto_bitrate_mbps(2560, 1440, 144, codec), 200u);
        EXPECT_EQ(auto_bitrate_mbps(3840, 2160, 60, codec), 150u);
        EXPECT_EQ(auto_bitrate_mbps(3840, 2160, 144, codec), 350u);
    }
}

TEST(AutoBitrate, H264IsFortyPercentHigher) {
    EXPECT_EQ(auto_bitrate_mbps(1920, 1080, 60, Codec::H264), 70u);
    EXPECT_EQ(auto_bitrate_mbps(2560, 1440, 144, Codec::H264), 280u);
    EXPECT_EQ(auto_bitrate_mbps(3840, 2160, 144, Codec::H264), 490u);
}

TEST(AutoBitrate, ScalesLinearlyWithFps) {
    EXPECT_EQ(auto_bitrate_mbps(1920, 1080, 120, Codec::Hevc), 100u);
    EXPECT_EQ(auto_bitrate_mbps(1920, 1080, 30, Codec::Hevc), 25u);
    // Entre los puntos 60 y 144 fps de 1440p: 85 + 60 × (200 − 85) / 84 ≈ 167.
    EXPECT_EQ(auto_bitrate_mbps(2560, 1440, 120, Codec::Hevc), 167u);
}

TEST(AutoBitrate, IsMonotonicInFpsAndResolution) {
    std::uint32_t previous = 0;
    for (double fps = 30; fps <= 240; fps += 1) {
        const auto value = auto_bitrate_mbps(2560, 1440, fps, Codec::Hevc);
        EXPECT_GE(value, previous) << "fps=" << fps;
        previous = value;
    }
    const auto r720 = auto_bitrate_mbps(1280, 720, 60, Codec::Hevc);
    const auto r1080 = auto_bitrate_mbps(1920, 1080, 60, Codec::Hevc);
    const auto r1600 = auto_bitrate_mbps(2560, 1600, 60, Codec::Hevc);
    const auto r1440 = auto_bitrate_mbps(2560, 1440, 60, Codec::Hevc);
    const auto r4k = auto_bitrate_mbps(3840, 2160, 60, Codec::Hevc);
    EXPECT_LT(r720, r1080);
    EXPECT_LT(r1080, r1440);
    EXPECT_LT(r1440, r1600);
    EXPECT_LT(r1600, r4k);
}

TEST(AutoBitrate, ClampsToAllowedRange) {
    EXPECT_EQ(auto_bitrate_mbps(640, 360, 30, Codec::Hevc), lancast::kMinBitrateMbps);
    EXPECT_EQ(auto_bitrate_mbps(7680, 4320, 240, Codec::H264), lancast::kMaxBitrateMbps);
    EXPECT_EQ(auto_bitrate_mbps(0, 1080, 60, Codec::Hevc), lancast::kMinBitrateMbps);
    EXPECT_EQ(auto_bitrate_mbps(1920, 1080, 0, Codec::Hevc), lancast::kMinBitrateMbps);
}
