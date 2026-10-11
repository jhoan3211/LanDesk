// Pruebas de integración contra el hardware real. Se ejecutan en cualquier PC con Windows 10/11 y pantalla.

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

#include "lancast/platform/platform.hpp"

using namespace lancast;

class Enumeration : public ::testing::Test {
protected:
    static void SetUpTestSuite() { platform::enable_per_monitor_dpi_awareness(); }
};

TEST_F(Enumeration, FindsAtLeastOneHardwareGpu) {
    const auto gpus = platform::enumerate_gpus();
    ASSERT_TRUE(gpus.has_value()) << gpus.error().message();
    ASSERT_FALSE(gpus->empty());
    EXPECT_TRUE(std::ranges::any_of(*gpus, [](const GpuInfo& g) { return !g.is_software; }));

    std::set<std::uint64_t> luids;
    for (std::uint32_t i = 0; i < gpus->size(); ++i) {
        EXPECT_EQ((*gpus)[i].index, i);
        EXPECT_FALSE((*gpus)[i].name.empty());
        luids.insert((*gpus)[i].luid);
    }
    EXPECT_EQ(luids.size(), gpus->size()) << "Los LUID deben ser únicos";
}

TEST_F(Enumeration, MonitorsAreConsistent) {
    const auto gpus = platform::enumerate_gpus();
    ASSERT_TRUE(gpus.has_value());
    const auto monitors = platform::enumerate_monitors(*gpus);
    ASSERT_TRUE(monitors.has_value()) << monitors.error().message();
    ASSERT_FALSE(monitors->empty());

    // Exactamente un monitor principal y está en el origen del escritorio virtual.
    const auto primaries = std::ranges::count_if(*monitors, [](const MonitorInfo& m) { return m.primary; });
    EXPECT_EQ(primaries, 1);
    const auto primary = std::ranges::find_if(*monitors, [](const MonitorInfo& m) { return m.primary; });
    ASSERT_NE(primary, monitors->end());
    EXPECT_EQ(primary->desktop.x, 0);
    EXPECT_EQ(primary->desktop.y, 0);

    Rect virtual_desktop;
    for (std::uint32_t i = 0; i < monitors->size(); ++i) {
        const MonitorInfo& m = (*monitors)[i];
        EXPECT_EQ(m.id, i + 1) << "Numeración 1..N";
        EXPECT_GT(m.desktop.width, 0);
        EXPECT_GT(m.desktop.height, 0);
        EXPECT_GT(m.native_width, 0u);
        EXPECT_GT(m.native_height, 0u);
        EXPECT_GE(m.dpi, 96u);
        EXPECT_GT(m.refresh_rate.value(), 1.0);
        EXPECT_FALSE(m.gdi_name.empty());
        if (i > 0) EXPECT_LE((*monitors)[i - 1].desktop.x, m.desktop.x) << "Orden de izquierda a derecha";
        virtual_desktop = virtual_desktop.united(m.desktop);
    }
    for (const auto& m : *monitors) EXPECT_TRUE(virtual_desktop.contains(m.desktop));
}

TEST_F(Enumeration, EncodersMatchTheirGpuVendor) {
    const auto gpus = platform::enumerate_gpus();
    ASSERT_TRUE(gpus.has_value());
    const auto encoders = platform::enumerate_encoders(*gpus);
    ASSERT_TRUE(encoders.has_value()) << encoders.error().message();
    for (const auto& e : *encoders) {
        ASSERT_LT(e.gpu_index, gpus->size());
        EXPECT_EQ(backend_for_vendor((*gpus)[e.gpu_index].vendor), e.backend);
        EXPECT_EQ(e.ffmpeg_name, ffmpeg_encoder_name(e.codec, e.backend));
    }
}

TEST_F(Enumeration, DecodersReportValidResolutions) {
    const auto gpus = platform::enumerate_gpus();
    ASSERT_TRUE(gpus.has_value());
    const auto probe = platform::enumerate_decoders(*gpus);
    ASSERT_TRUE(probe.has_value()) << probe.error().message();
    for (const auto& d : probe->decoders) {
        EXPECT_GE(d.max_width, 1920u);
        EXPECT_GE(d.max_height, 1080u);
        EXPECT_FALSE((*gpus)[d.gpu_index].is_software);
    }
}

TEST_F(Enumeration, FullReportHasNoErrors) {
    const SystemReport report = platform::collect_system_report();
    EXPECT_FALSE(report.gpus.empty());
    EXPECT_FALSE(report.monitors.empty());
    EXPECT_GT(report.virtual_desktop.width, 0);
    for (const auto& w : report.warnings) {
        EXPECT_EQ(w.find("0x"), std::string::npos) << "Error de API inesperado: " << w;
    }
}
