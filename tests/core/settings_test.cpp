#include <gtest/gtest.h>

#include "lancast/core/geometry.hpp"
#include "lancast/core/settings.hpp"
#include "lancast/core/toml.hpp"

using namespace lancast;

TEST(Toml, ParsesTablesArraysAndValues) {
    const auto doc = toml::Document::parse(R"(
# comentario
name = "LanCast \"pro\""   # comentario final
port = 47984
ratio = 1.5
on = true

[host]
ips = ["192.168.1.2", "10.0.0.5"]

[[client.profiles]]
name = "A"
fps = 60

[[client.profiles]]
name = "B"
fps = 144
)");
    const toml::Table* root = doc.table("");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->get_string("name"), "LanCast \"pro\"");
    EXPECT_EQ(root->get_int("port"), 47984);
    EXPECT_EQ(root->get_double("ratio"), 1.5);
    EXPECT_EQ(root->get_bool("on"), true);
    EXPECT_EQ(doc.table("host")->get_strings("ips"), (std::vector<std::string>{"192.168.1.2", "10.0.0.5"}));
    const auto& profiles = doc.arrays.at("client.profiles");
    ASSERT_EQ(profiles.size(), 2u);
    EXPECT_EQ(profiles[1].get_int("fps"), 144);
}

TEST(Settings, RoundtripThroughToml) {
    AppSettings s;
    s.device_id = "00112233445566778899aabbccddeeff";
    s.device_name = "Mi PC";
    s.host.control_port = 50000;
    s.host.ip_whitelist = {"192.168.40.7"};
    s.host.receive_folder = R"(D:\Recibidos)";
    s.client.recent_addresses = {"192.168.40.7", "192.168.40.5:47984"};
    s.client.stream.fps = 144;
    s.client.stream.codec = CodecChoice::Av1;
    s.client.stream.resolution = ResolutionPreset::Custom;
    s.client.stream.custom_width = 3440;
    s.client.stream.custom_height = 1440;
    s.client.stream.scaling = ScalingMode::Native;
    s.client.profiles = default_profiles();

    const AppSettings out = settings_from_toml(settings_to_toml(s));
    EXPECT_EQ(out.device_id, s.device_id);
    EXPECT_EQ(out.device_name, s.device_name);
    EXPECT_EQ(out.host.control_port, 50000);
    EXPECT_EQ(out.host.ip_whitelist, s.host.ip_whitelist);
    EXPECT_EQ(out.host.receive_folder, s.host.receive_folder);
    EXPECT_EQ(out.client.recent_addresses, s.client.recent_addresses);
    EXPECT_EQ(out.client.stream, s.client.stream);
    ASSERT_EQ(out.client.profiles.size(), 3u);
    EXPECT_EQ(out.client.profiles[0].name, "Trabajo 1440p 4:4:4");
    EXPECT_TRUE(out.client.profiles[0].stream.chroma_444);
    EXPECT_EQ(out.client.profiles[1].stream.fps, 144u);
}

TEST(Settings, ClampsInvalidValues) {
    const AppSettings s = settings_from_toml("[client.stream]\nfps = 99999\nfec_percent = 90\ncodec = \"xyz\"\n");
    EXPECT_EQ(s.client.stream.fps, 360u);
    EXPECT_EQ(s.client.stream.fec_percent, 50u);
    EXPECT_EQ(s.client.stream.codec, CodecChoice::Auto);
}

TEST(Settings, RequestedSizeAndBitrate) {
    StreamSettings s;
    EXPECT_EQ(requested_size(s, {2560, 1440}), (Size{2560, 1440}));
    s.resolution = ResolutionPreset::R1080;
    EXPECT_EQ(requested_size(s, {2560, 1440}), (Size{1920, 1080}));

    s.fps = 144;
    EXPECT_EQ(effective_bitrate_kbps(s, {2560, 1440}, Codec::Hevc), 200000u);
    s.auto_bitrate = false;
    s.bitrate_mbps = 300;
    EXPECT_EQ(effective_bitrate_kbps(s, {2560, 1440}, Codec::Hevc), 300000u);
}

TEST(Geometry, FitStreamSizeKeepsAspectAndNeverUpscales) {
    EXPECT_EQ(fit_stream_size({3840, 2160}, {1920, 1080}), (Size{1920, 1080}));
    EXPECT_EQ(fit_stream_size({2560, 1440}, {3840, 2160}), (Size{2560, 1440}));
    EXPECT_EQ(fit_stream_size({3440, 1440}, {1920, 1080}), (Size{1920, 804}));
    EXPECT_EQ(fit_stream_size({1080, 1920}, {1920, 1080}), (Size{608, 1080}));  // Monitor vertical.
    const Size s = fit_stream_size({1366, 768}, {1280, 720});
    EXPECT_EQ(s.width % 2, 0u);
    EXPECT_EQ(s.height % 2, 0u);
}

TEST(Geometry, ViewRectModes) {
    EXPECT_EQ(view_rect({1920, 1080}, {2560, 1440}, ScalingMode::Fit), (Rect{0, 0, 2560, 1440}));
    EXPECT_EQ(view_rect({1920, 1080}, {1920, 1200}, ScalingMode::Fit), (Rect{0, 60, 1920, 1080}));  // Letterbox.
    EXPECT_EQ(view_rect({3440, 1440}, {2560, 1440}, ScalingMode::Fit), (Rect{0, 184, 2560, 1072}));
    EXPECT_EQ(view_rect({1920, 1080}, {2560, 1440}, ScalingMode::Native), (Rect{320, 180, 1920, 1080}));
    EXPECT_EQ(view_rect({1920, 1080}, {800, 800}, ScalingMode::Stretch), (Rect{0, 0, 800, 800}));
}

TEST(Geometry, WindowToDesktopMappingWithLetterboxAndNegativeCoordinates) {
    // Stream 1920x1080 de un monitor a la izquierda del principal, mostrado con letterbox en 1920x1200.
    const Rect view = view_rect({1920, 1080}, {1920, 1200}, ScalingMode::Fit);
    const Rect source{-1920, 0, 1920, 1080};

    EXPECT_FALSE(window_to_normalized({10, 30}, view).has_value());  // Barra negra superior.

    const auto top_left = window_to_normalized({0, 60}, view);
    ASSERT_TRUE(top_left.has_value());
    const PointF d0 = normalized_to_desktop(*top_left, source);
    EXPECT_FLOAT_EQ(d0.x, -1920.f);
    EXPECT_FLOAT_EQ(d0.y, 0.f);

    const auto bottom_right = window_to_normalized({1920, 1140}, view);
    ASSERT_TRUE(bottom_right.has_value());
    const PointF d1 = normalized_to_desktop(*bottom_right, source);
    EXPECT_FLOAT_EQ(d1.x, -1.f);
    EXPECT_FLOAT_EQ(d1.y, 1079.f);

    // Escritorio virtual: [-1920, 0] .. [2560, 1440].
    const Rect vs{-1920, 0, 1920 + 2560, 1440};
    EXPECT_EQ(desktop_to_absolute({-1920, 0}, vs).x, 0);
    EXPECT_EQ(desktop_to_absolute({2559, 1439}, vs).x, 65535);
    EXPECT_EQ(desktop_to_absolute({2559, 1439}, vs).y, 65535);
    const auto mid = desktop_to_absolute({0, 0}, vs);
    EXPECT_NEAR(mid.x, 1920.0 * 65535 / (vs.width - 1), 1.0);
}

TEST(Geometry, HiDpiScaledWindow) {
    // Ventana del cliente a 125%: las coordenadas del mouse llegan en píxeles físicos, igual que el view rect.
    const Rect view = view_rect({2560, 1440}, {3200, 1800}, ScalingMode::Fit);
    const auto n = window_to_normalized({1600, 900}, view);
    ASSERT_TRUE(n.has_value());
    EXPECT_NEAR(n->x, 0.5f, 1e-3f);
    EXPECT_NEAR(n->y, 0.5f, 1e-3f);
}
