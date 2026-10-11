#include <gtest/gtest.h>

#include "lancast/core/types.hpp"

using namespace lancast;

TEST(Types, VendorFromPciId) {
    EXPECT_EQ(vendor_from_pci_id(0x10DE), GpuVendor::Nvidia);
    EXPECT_EQ(vendor_from_pci_id(0x1002), GpuVendor::Amd);
    EXPECT_EQ(vendor_from_pci_id(0x8086), GpuVendor::Intel);
    EXPECT_EQ(vendor_from_pci_id(0x1414), GpuVendor::Microsoft);
    EXPECT_EQ(vendor_from_pci_id(0xABCD), GpuVendor::Unknown);
}

TEST(Types, BackendForVendor) {
    EXPECT_EQ(backend_for_vendor(GpuVendor::Nvidia), EncoderBackend::Nvenc);
    EXPECT_EQ(backend_for_vendor(GpuVendor::Amd), EncoderBackend::Amf);
    EXPECT_EQ(backend_for_vendor(GpuVendor::Intel), EncoderBackend::Qsv);
    EXPECT_FALSE(backend_for_vendor(GpuVendor::Microsoft).has_value());
}

TEST(Types, FfmpegEncoderNames) {
    EXPECT_EQ(ffmpeg_encoder_name(Codec::H264, EncoderBackend::Nvenc), "h264_nvenc");
    EXPECT_EQ(ffmpeg_encoder_name(Codec::Hevc, EncoderBackend::Amf), "hevc_amf");
    EXPECT_EQ(ffmpeg_encoder_name(Codec::Av1, EncoderBackend::Qsv), "av1_qsv");
}

TEST(Types, RectUnionHandlesNegativeCoordinates) {
    const Rect primary{0, 0, 2560, 1440};
    const Rect left{-1920, 200, 1920, 1080};
    const Rect above{0, -2160, 3840, 2160};

    const Rect all = primary.united(left).united(above);
    EXPECT_EQ(all, (Rect{-1920, -2160, 1920 + 3840, 2160 + 1440}));
    EXPECT_TRUE(all.contains(primary));
    EXPECT_TRUE(all.contains(left));
    EXPECT_TRUE(all.contains(above));
    EXPECT_FALSE(primary.contains(left));
}

TEST(Types, RectUnionWithEmpty) {
    const Rect r{-100, -50, 10, 20};
    EXPECT_EQ(Rect{}.united(r), r);
    EXPECT_EQ(r.united(Rect{}), r);
}

TEST(Types, RationalRefreshRate) {
    EXPECT_NEAR((Rational{144000, 1000}).value(), 144.0, 1e-9);
    EXPECT_NEAR((Rational{60000, 1001}).value(), 59.94, 1e-2);
    EXPECT_EQ((Rational{60, 0}).value(), 0.0);
}
