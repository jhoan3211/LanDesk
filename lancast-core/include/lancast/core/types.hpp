#pragma once

// Tipos compartidos entre host, cliente y (más adelante) el handshake del protocolo.
// Este módulo no depende de Windows para poder reutilizarse en pruebas y en otras plataformas.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lancast {

enum class Codec : std::uint8_t { H264, Hevc, Av1 };

enum class GpuVendor : std::uint8_t { Nvidia, Amd, Intel, Microsoft, Qualcomm, Unknown };

// Backends de codificación por hardware que usaremos a través de FFmpeg.
enum class EncoderBackend : std::uint8_t { Nvenc, Amf, Qsv };

inline constexpr Codec kAllCodecs[] = {Codec::H264, Codec::Hevc, Codec::Av1};

[[nodiscard]] GpuVendor vendor_from_pci_id(std::uint32_t pci_vendor_id) noexcept;
[[nodiscard]] std::optional<EncoderBackend> backend_for_vendor(GpuVendor vendor) noexcept;

[[nodiscard]] std::string_view to_string(Codec codec) noexcept;
[[nodiscard]] std::string_view to_string(GpuVendor vendor) noexcept;
[[nodiscard]] std::string_view to_string(EncoderBackend backend) noexcept;

// Nombre del codificador en FFmpeg, p. ej. "hevc_nvenc", "av1_amf", "h264_qsv".
[[nodiscard]] std::string ffmpeg_encoder_name(Codec codec, EncoderBackend backend);

// Rectángulo en coordenadas del escritorio virtual (píxeles físicos). x/y pueden ser negativos.
struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] std::int32_t right() const noexcept { return x + width; }
    [[nodiscard]] std::int32_t bottom() const noexcept { return y + height; }
    [[nodiscard]] bool contains(const Rect& other) const noexcept;
    [[nodiscard]] Rect united(const Rect& other) const noexcept;
    friend bool operator==(const Rect&, const Rect&) = default;
};

struct Rational {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;

    [[nodiscard]] double value() const noexcept {
        return denominator == 0 ? 0.0 : static_cast<double>(numerator) / denominator;
    }
};

struct GpuInfo {
    std::uint32_t index = 0;  // Orden por preferencia de rendimiento (0 = la más potente).
    std::string name;
    GpuVendor vendor = GpuVendor::Unknown;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint64_t luid = 0;  // LUID del adaptador empaquetado (HighPart << 32 | LowPart).
    std::uint64_t dedicated_video_memory = 0;
    std::uint64_t shared_system_memory = 0;
    bool is_software = false;
};

struct MonitorInfo {
    std::uint32_t id = 0;  // 1..N, de izquierda a derecha (coincide con Ctrl+Alt+Shift+N).
    std::string gdi_name;  // "\\.\DISPLAY1"
    std::string friendly_name;
    std::optional<std::uint32_t> gpu_index;
    Rect desktop;  // Modo actual, en coordenadas del escritorio virtual.
    std::uint32_t native_width = 0;
    std::uint32_t native_height = 0;
    Rational refresh_rate;
    std::uint32_t dpi = 96;
    std::uint32_t scale_percent = 100;
    std::uint32_t rotation_degrees = 0;
    std::uint32_t bits_per_color = 8;
    bool primary = false;
    bool hdr_supported = false;
    bool hdr_enabled = false;
};

struct EncoderInfo {
    Codec codec = Codec::H264;
    EncoderBackend backend = EncoderBackend::Nvenc;
    std::string ffmpeg_name;
    std::uint32_t gpu_index = 0;
    std::string mft_name;      // Nombre del MFT de hardware que expone el driver.
    std::string runtime_dll;   // DLL de runtime que necesita FFmpeg (nvEncodeAPI64.dll, amfrt64.dll...).
    bool runtime_present = false;
};

struct DecoderInfo {
    Codec codec = Codec::H264;
    std::string profile;  // "Main", "Main10", "Profile0"...
    std::uint32_t gpu_index = 0;
    bool ten_bit = false;
    bool chroma_444 = false;
    std::uint32_t max_width = 0;   // Máxima resolución probada con éxito.
    std::uint32_t max_height = 0;
};

struct UnknownDecoderProfile {
    std::uint32_t gpu_index = 0;
    std::string guid;
};

struct SystemReport {
    std::vector<GpuInfo> gpus;
    std::vector<MonitorInfo> monitors;
    Rect virtual_desktop;
    std::vector<EncoderInfo> encoders;
    std::vector<DecoderInfo> decoders;
    std::vector<UnknownDecoderProfile> other_decoder_profiles;
    std::vector<std::string> warnings;
};

}  // namespace lancast
