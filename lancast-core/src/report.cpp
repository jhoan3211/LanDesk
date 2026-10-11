#include "lancast/core/report.hpp"

#include <format>
#include <iterator>

#include "lancast/core/bitrate.hpp"
#include "lancast/core/json_writer.hpp"

namespace lancast {
namespace {

std::string format_bytes(std::uint64_t bytes) {
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    constexpr double kMiB = 1024.0 * 1024.0;
    const auto b = static_cast<double>(bytes);
    if (b >= kGiB) return std::format("{:.1f} GB", b / kGiB);
    return std::format("{:.0f} MB", b / kMiB);
}

std::string gpu_label(const SystemReport& report, std::uint32_t gpu_index) {
    for (const auto& gpu : report.gpus) {
        if (gpu.index == gpu_index) return std::format("GPU {} ({})", gpu.index, gpu.name);
    }
    return std::format("GPU {}", gpu_index);
}

}  // namespace

std::string format_report_text(const SystemReport& report, bool verbose) {
    std::string out;
    auto line = [&out](std::string_view s) {
        out += s;
        out += '\n';
    };

    line("== GPUs ==");
    if (report.gpus.empty()) line("  (ninguna)");
    for (const auto& gpu : report.gpus) {
        line(std::format("  [{}] {}{}", gpu.index, gpu.name, gpu.is_software ? "  (software)" : ""));
        line(std::format("      Fabricante: {} (VEN_{:04X} DEV_{:04X})  VRAM dedicada: {}  Compartida: {}",
                         to_string(gpu.vendor), gpu.vendor_id, gpu.device_id,
                         format_bytes(gpu.dedicated_video_memory), format_bytes(gpu.shared_system_memory)));
        if (verbose) line(std::format("      LUID: {:016X}", gpu.luid));
    }

    line("");
    line("== Monitores ==");
    if (report.monitors.empty()) line("  (ninguno)");
    for (const auto& m : report.monitors) {
        line(std::format("  [{}] {}{}  {}", m.id, m.friendly_name.empty() ? m.gdi_name : m.friendly_name,
                         m.primary ? "  (principal)" : "", m.gdi_name));
        line(std::format("      Modo actual: {}x{} @ {:.2f} Hz   Nativa: {}x{}", m.desktop.width, m.desktop.height,
                         m.refresh_rate.value(), m.native_width, m.native_height));
        line(std::format("      Posición: ({}, {})   Escala: {}% ({} DPI)   Rotación: {}°", m.desktop.x, m.desktop.y,
                         m.scale_percent, m.dpi, m.rotation_degrees));
        line(std::format("      Color: {} bits   HDR: {}   Conectado a: {}", m.bits_per_color,
                         m.hdr_enabled     ? "activado"
                         : m.hdr_supported ? "soportado (desactivado)"
                                           : "no",
                         m.gpu_index ? gpu_label(report, *m.gpu_index) : std::string{"?"}));
    }
    const Rect& vd = report.virtual_desktop;
    line(std::format("  [Todos] Escritorio virtual: {}x{} en ({}, {})", vd.width, vd.height, vd.x, vd.y));

    line("");
    line("== Codificadores por hardware (host) ==");
    if (report.encoders.empty()) line("  (ninguno detectado)");
    for (const auto& e : report.encoders) {
        line(std::format("  {:<12} {:<6} {:<10} {}  [MFT: {}]  runtime {}: {}", e.ffmpeg_name, to_string(e.codec),
                         to_string(e.backend), gpu_label(report, e.gpu_index), e.mft_name, e.runtime_dll,
                         e.runtime_present ? "OK" : "NO ENCONTRADO"));
    }

    line("");
    line("== Decodificadores D3D11VA (cliente) ==");
    if (report.decoders.empty()) line("  (ninguno detectado)");
    for (const auto& d : report.decoders) {
        line(std::format("  {:<6} {:<14} {}  máx. probado {}x{}", to_string(d.codec), d.profile,
                         gpu_label(report, d.gpu_index), d.max_width, d.max_height));
    }
    if (verbose && !report.other_decoder_profiles.empty()) {
        line("  Otros perfiles DXVA expuestos por el driver:");
        for (const auto& p : report.other_decoder_profiles) {
            line(std::format("    {}  {}", gpu_label(report, p.gpu_index), p.guid));
        }
    }

    line("");
    line("== Bitrate automático de referencia ==");
    for (const auto& [w, h, fps] : {std::tuple{1920u, 1080u, 60.0}, std::tuple{2560u, 1440u, 60.0},
                                    std::tuple{2560u, 1440u, 144.0}, std::tuple{3840u, 2160u, 60.0},
                                    std::tuple{3840u, 2160u, 144.0}}) {
        line(std::format("  {}x{}@{:<4} HEVC/AV1: {:>4} Mbps   H.264: {:>4} Mbps", w, h, fps,
                         auto_bitrate_mbps(w, h, fps, Codec::Hevc), auto_bitrate_mbps(w, h, fps, Codec::H264)));
    }

    if (!report.warnings.empty()) {
        line("");
        line("== Avisos ==");
        for (const auto& w : report.warnings) line("  - " + w);
    }
    return out;
}

std::string format_report_json(const SystemReport& report) {
    JsonWriter j;
    j.begin_object();

    j.key("gpus").begin_array();
    for (const auto& g : report.gpus) {
        j.begin_object()
            .field("index", g.index)
            .field("name", g.name)
            .field("vendor", to_string(g.vendor))
            .field("vendor_id", g.vendor_id)
            .field("device_id", g.device_id)
            .field("luid", std::format("{:016X}", g.luid))
            .field("dedicated_video_memory", g.dedicated_video_memory)
            .field("shared_system_memory", g.shared_system_memory)
            .field("is_software", g.is_software)
            .end_object();
    }
    j.end_array();

    j.key("monitors").begin_array();
    for (const auto& m : report.monitors) {
        j.begin_object()
            .field("id", m.id)
            .field("gdi_name", m.gdi_name)
            .field("friendly_name", m.friendly_name);
        j.key("gpu_index");
        if (m.gpu_index) {
            j.value(*m.gpu_index);
        } else {
            j.null();
        }
        j.key("desktop")
            .begin_object()
            .field("x", m.desktop.x)
            .field("y", m.desktop.y)
            .field("width", m.desktop.width)
            .field("height", m.desktop.height)
            .end_object();
        j.field("native_width", m.native_width)
            .field("native_height", m.native_height)
            .field("refresh_rate_hz", m.refresh_rate.value())
            .field("dpi", m.dpi)
            .field("scale_percent", m.scale_percent)
            .field("rotation_degrees", m.rotation_degrees)
            .field("bits_per_color", m.bits_per_color)
            .field("primary", m.primary)
            .field("hdr_supported", m.hdr_supported)
            .field("hdr_enabled", m.hdr_enabled)
            .end_object();
    }
    j.end_array();

    j.key("virtual_desktop")
        .begin_object()
        .field("x", report.virtual_desktop.x)
        .field("y", report.virtual_desktop.y)
        .field("width", report.virtual_desktop.width)
        .field("height", report.virtual_desktop.height)
        .end_object();

    j.key("encoders").begin_array();
    for (const auto& e : report.encoders) {
        j.begin_object()
            .field("ffmpeg_name", e.ffmpeg_name)
            .field("codec", to_string(e.codec))
            .field("backend", to_string(e.backend))
            .field("gpu_index", e.gpu_index)
            .field("mft_name", e.mft_name)
            .field("runtime_dll", e.runtime_dll)
            .field("runtime_present", e.runtime_present)
            .end_object();
    }
    j.end_array();

    j.key("decoders").begin_array();
    for (const auto& d : report.decoders) {
        j.begin_object()
            .field("codec", to_string(d.codec))
            .field("profile", d.profile)
            .field("gpu_index", d.gpu_index)
            .field("ten_bit", d.ten_bit)
            .field("chroma_444", d.chroma_444)
            .field("max_width", d.max_width)
            .field("max_height", d.max_height)
            .end_object();
    }
    j.end_array();

    j.key("warnings").begin_array();
    for (const auto& w : report.warnings) j.value(w);
    j.end_array();

    j.end_object();
    return j.str() + "\n";
}

}  // namespace lancast
