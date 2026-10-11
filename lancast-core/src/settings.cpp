#include "lancast/core/settings.hpp"

#include <algorithm>

#include "lancast/core/bitrate.hpp"
#include "lancast/core/toml.hpp"

namespace lancast {
namespace {

template <typename E, std::size_t N>
E enum_from(std::string_view text, const std::string_view (&names)[N], E fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (names[i] == text) return static_cast<E>(i);
    }
    return fallback;
}

constexpr std::string_view kCodecNames[] = {"auto", "h264", "hevc", "av1"};
constexpr std::string_view kRateNames[] = {"cbr", "vbr"};
constexpr std::string_view kScalingNames[] = {"fit", "stretch", "native"};
constexpr std::string_view kResolutionNames[] = {"client", "720p", "1080p", "1440p", "2160p", "custom"};

std::uint32_t clamp_u32(std::int64_t v, std::uint32_t lo, std::uint32_t hi) {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(v, lo, hi));
}

void stream_to_table(const StreamSettings& s, toml::Table& t) {
    t.set("monitor", static_cast<std::int64_t>(s.monitor_id));
    t.set("resolution", std::string{kResolutionNames[static_cast<int>(s.resolution)]});
    t.set("custom_width", static_cast<std::int64_t>(s.custom_width));
    t.set("custom_height", static_cast<std::int64_t>(s.custom_height));
    t.set("fps", static_cast<std::int64_t>(s.fps));
    t.set("codec", std::string{kCodecNames[static_cast<int>(s.codec)]});
    t.set("auto_bitrate", s.auto_bitrate);
    t.set("bitrate_mbps", static_cast<std::int64_t>(s.bitrate_mbps));
    t.set("rate_control", std::string{kRateNames[static_cast<int>(s.rate_control)]});
    t.set("chroma_444", s.chroma_444);
    t.set("hdr", s.hdr);
    t.set("scaling", std::string{kScalingNames[static_cast<int>(s.scaling)]});
    t.set("vsync", s.vsync);
    t.set("audio", s.audio);
    t.set("audio_sample_rate", static_cast<std::int64_t>(s.audio_sample_rate));
    t.set("audio_stereo", s.audio_stereo);
    t.set("audio_volume", static_cast<std::int64_t>(s.audio_volume));
    t.set("audio_buffer_ms", static_cast<std::int64_t>(s.audio_buffer_ms));
    t.set("fec_percent", static_cast<std::int64_t>(s.fec_percent));
    t.set("low_latency", s.low_latency);
    t.set("intra_refresh", s.intra_refresh);
    t.set("slices", s.slices);
}

StreamSettings stream_from_table(const toml::Table& t) {
    StreamSettings s;
    s.monitor_id = clamp_u32(t.get_int("monitor").value_or(s.monitor_id), 0, 64);
    s.resolution = enum_from(t.get_string("resolution").value_or(""), kResolutionNames, s.resolution);
    s.custom_width = clamp_u32(t.get_int("custom_width").value_or(s.custom_width), 320, 7680);
    s.custom_height = clamp_u32(t.get_int("custom_height").value_or(s.custom_height), 240, 4320);
    s.fps = clamp_u32(t.get_int("fps").value_or(s.fps), 10, 360);
    s.codec = enum_from(t.get_string("codec").value_or(""), kCodecNames, s.codec);
    s.auto_bitrate = t.get_bool("auto_bitrate").value_or(s.auto_bitrate);
    s.bitrate_mbps = clamp_u32(t.get_int("bitrate_mbps").value_or(s.bitrate_mbps), kMinBitrateMbps, kMaxBitrateMbps);
    s.rate_control = enum_from(t.get_string("rate_control").value_or(""), kRateNames, s.rate_control);
    s.chroma_444 = t.get_bool("chroma_444").value_or(s.chroma_444);
    s.hdr = t.get_bool("hdr").value_or(s.hdr);
    s.scaling = enum_from(t.get_string("scaling").value_or(""), kScalingNames, s.scaling);
    s.vsync = t.get_bool("vsync").value_or(s.vsync);
    s.audio = t.get_bool("audio").value_or(s.audio);
    s.audio_sample_rate = clamp_u32(t.get_int("audio_sample_rate").value_or(0), 0, 192000);
    if (s.audio_sample_rate != 0 && s.audio_sample_rate < 8000) s.audio_sample_rate = 8000;
    s.audio_stereo = t.get_bool("audio_stereo").value_or(s.audio_stereo);
    s.audio_volume = clamp_u32(t.get_int("audio_volume").value_or(s.audio_volume), 0, 100);
    s.audio_buffer_ms = clamp_u32(t.get_int("audio_buffer_ms").value_or(s.audio_buffer_ms), 5, 200);
    s.fec_percent = clamp_u32(t.get_int("fec_percent").value_or(s.fec_percent), 0, 50);
    s.low_latency = t.get_bool("low_latency").value_or(s.low_latency);
    s.intra_refresh = t.get_bool("intra_refresh").value_or(s.intra_refresh);
    s.slices = t.get_bool("slices").value_or(s.slices);
    return s;
}

}  // namespace

std::string_view to_string(CodecChoice c) noexcept {
    switch (c) {
        case CodecChoice::Auto:
            return "Automático";
        case CodecChoice::H264:
            return "H.264";
        case CodecChoice::Hevc:
            return "HEVC";
        case CodecChoice::Av1:
            return "AV1";
    }
    return "?";
}

std::string_view to_string(RateControl r) noexcept { return r == RateControl::Cbr ? "CBR" : "VBR"; }

std::string_view to_string(ScalingMode s) noexcept {
    switch (s) {
        case ScalingMode::Fit:
            return "Ajustar";
        case ScalingMode::Stretch:
            return "Estirar";
        case ScalingMode::Native:
            return "1:1";
    }
    return "?";
}

std::string_view to_string(ResolutionPreset r) noexcept {
    switch (r) {
        case ResolutionPreset::ClientNative:
            return "Nativa del cliente";
        case ResolutionPreset::R720:
            return "720p";
        case ResolutionPreset::R1080:
            return "1080p";
        case ResolutionPreset::R1440:
            return "1440p";
        case ResolutionPreset::R2160:
            return "4K (2160p)";
        case ResolutionPreset::Custom:
            return "Personalizada";
    }
    return "?";
}

std::string settings_to_toml(const AppSettings& s) {
    toml::Document doc;
    toml::Table& root = doc.table_mut("");
    root.set("device_id", s.device_id);
    root.set("device_name", s.device_name);
    root.set("start_with_windows", s.start_with_windows);
    root.set("minimize_to_tray", s.minimize_to_tray);
    root.set("check_updates", s.check_updates);

    toml::Table& h = doc.table_mut("host");
    h.set("enabled", s.host.enabled);
    h.set("control_port", static_cast<std::int64_t>(s.host.control_port));
    h.set("media_port", static_cast<std::int64_t>(s.host.media_port));
    h.set("web_port", static_cast<std::int64_t>(s.host.web_port));
    h.set("web_enabled", s.host.web_enabled);
    h.set("allow_input", s.host.allow_input);
    h.set("allow_clipboard", s.host.allow_clipboard);
    h.set("allow_files", s.host.allow_files);
    h.set("allow_audio", s.host.allow_audio);
    h.set("private_networks_only", s.host.private_networks_only);
    h.set("ip_whitelist", s.host.ip_whitelist);
    h.set("receive_folder", s.host.receive_folder);
    h.set("overwrite_existing", s.host.overwrite_existing);

    toml::Table& c = doc.table_mut("client");
    c.set("recent_addresses", s.client.recent_addresses);
    c.set("receive_folder", s.client.receive_folder);
    c.set("active_profile", s.client.active_profile);
    c.set("view_only", s.client.view_only);
    c.set("clipboard_sync", s.client.clipboard_sync);
    c.set("relative_mouse", s.client.relative_mouse);
    c.set("show_stats", s.client.show_stats);
    c.set("simulate_loss_percent", static_cast<std::int64_t>(s.client.simulate_loss_percent));
    stream_to_table(s.client.stream, doc.table_mut("client.stream"));

    auto& profiles = doc.arrays["client.profiles"];
    for (const Profile& p : s.client.profiles) {
        toml::Table t;
        t.set("name", p.name);
        stream_to_table(p.stream, t);
        profiles.push_back(std::move(t));
    }
    return doc.serialize();
}

AppSettings settings_from_toml(std::string_view text) {
    const toml::Document doc = toml::Document::parse(text);
    AppSettings s;
    if (const toml::Table* root = doc.table("")) {
        s.device_id = root->get_string("device_id").value_or("");
        s.device_name = root->get_string("device_name").value_or("");
        s.start_with_windows = root->get_bool("start_with_windows").value_or(s.start_with_windows);
        s.minimize_to_tray = root->get_bool("minimize_to_tray").value_or(s.minimize_to_tray);
        s.check_updates = root->get_bool("check_updates").value_or(s.check_updates);
    }
    if (const toml::Table* h = doc.table("host")) {
        s.host.enabled = h->get_bool("enabled").value_or(s.host.enabled);
        s.host.control_port = static_cast<std::uint16_t>(clamp_u32(h->get_int("control_port").value_or(s.host.control_port), 1024, 65534));
        s.host.media_port = static_cast<std::uint16_t>(clamp_u32(h->get_int("media_port").value_or(s.host.media_port), 1024, 65535));
        s.host.web_port = static_cast<std::uint16_t>(clamp_u32(h->get_int("web_port").value_or(s.host.web_port), 1024, 65535));
        s.host.web_enabled = h->get_bool("web_enabled").value_or(s.host.web_enabled);
        s.host.allow_input = h->get_bool("allow_input").value_or(s.host.allow_input);
        s.host.allow_clipboard = h->get_bool("allow_clipboard").value_or(s.host.allow_clipboard);
        s.host.allow_files = h->get_bool("allow_files").value_or(s.host.allow_files);
        s.host.allow_audio = h->get_bool("allow_audio").value_or(s.host.allow_audio);
        s.host.private_networks_only = h->get_bool("private_networks_only").value_or(s.host.private_networks_only);
        s.host.ip_whitelist = h->get_strings("ip_whitelist");
        s.host.receive_folder = h->get_string("receive_folder").value_or("");
        s.host.overwrite_existing = h->get_bool("overwrite_existing").value_or(s.host.overwrite_existing);
    }
    if (const toml::Table* c = doc.table("client")) {
        s.client.recent_addresses = c->get_strings("recent_addresses");
        s.client.receive_folder = c->get_string("receive_folder").value_or("");
        s.client.active_profile = c->get_string("active_profile").value_or("");
        s.client.view_only = c->get_bool("view_only").value_or(s.client.view_only);
        s.client.clipboard_sync = c->get_bool("clipboard_sync").value_or(s.client.clipboard_sync);
        s.client.relative_mouse = c->get_bool("relative_mouse").value_or(s.client.relative_mouse);
        s.client.show_stats = c->get_bool("show_stats").value_or(s.client.show_stats);
        s.client.simulate_loss_percent = clamp_u32(c->get_int("simulate_loss_percent").value_or(0), 0, 50);
    }
    if (const toml::Table* st = doc.table("client.stream")) s.client.stream = stream_from_table(*st);

    if (const auto it = doc.arrays.find("client.profiles"); it != doc.arrays.end()) {
        for (const toml::Table& t : it->second) {
            Profile p;
            p.name = t.get_string("name").value_or("");
            if (p.name.empty()) continue;
            p.stream = stream_from_table(t);
            s.client.profiles.push_back(std::move(p));
        }
    } else {
        s.client.profiles = default_profiles();
    }
    return s;
}

std::vector<Profile> default_profiles() {
    std::vector<Profile> out;

    Profile work{"Trabajo 1440p 4:4:4", {}};
    work.stream.resolution = ResolutionPreset::R1440;
    work.stream.fps = 60;
    work.stream.chroma_444 = true;
    work.stream.codec = CodecChoice::Hevc;
    out.push_back(work);

    Profile game{"Juego 144 Hz", {}};
    game.stream.resolution = ResolutionPreset::ClientNative;
    game.stream.fps = 144;
    game.stream.codec = CodecChoice::Auto;
    game.stream.vsync = false;
    out.push_back(game);

    Profile phone{"Celular", {}};
    phone.stream.resolution = ResolutionPreset::R720;
    phone.stream.fps = 30;
    phone.stream.codec = CodecChoice::H264;
    phone.stream.auto_bitrate = false;
    phone.stream.bitrate_mbps = 8;
    out.push_back(phone);
    return out;
}

Size requested_size(const StreamSettings& s, Size client_native) noexcept {
    switch (s.resolution) {
        case ResolutionPreset::ClientNative:
            return client_native.width > 0 ? client_native : Size{1920, 1080};
        case ResolutionPreset::R720:
            return {1280, 720};
        case ResolutionPreset::R1080:
            return {1920, 1080};
        case ResolutionPreset::R1440:
            return {2560, 1440};
        case ResolutionPreset::R2160:
            return {3840, 2160};
        case ResolutionPreset::Custom:
            return {s.custom_width, s.custom_height};
    }
    return {1920, 1080};
}

std::uint32_t effective_bitrate_kbps(const StreamSettings& s, Size stream_size, Codec codec) noexcept {
    const std::uint32_t mbps =
        s.auto_bitrate ? auto_bitrate_mbps(stream_size.width, stream_size.height, s.fps, codec) : s.bitrate_mbps;
    return mbps * 1000;
}

}  // namespace lancast
