#pragma once

// Configuración de LanCast (se guarda en %APPDATA%\LanCast\config.toml) y perfiles de stream.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lancast/core/types.hpp"

namespace lancast {

inline constexpr std::uint16_t kDefaultControlPort = 47984;  // TCP: control. +1 = archivos.
inline constexpr std::uint16_t kDefaultMediaPort = 47998;    // UDP: video y audio.
inline constexpr std::uint16_t kDiscoveryPort = 47999;       // UDP broadcast: descubrimiento.
inline constexpr std::uint16_t kDefaultWebPort = 47990;      // HTTPS: cliente web para el celular.

enum class CodecChoice : std::uint8_t { Auto, H264, Hevc, Av1 };
enum class RateControl : std::uint8_t { Cbr, Vbr };
enum class ScalingMode : std::uint8_t { Fit, Stretch, Native };
enum class ResolutionPreset : std::uint8_t { ClientNative, R720, R1080, R1440, R2160, Custom };

struct StreamSettings {
    std::uint32_t monitor_id = 1;  // 0 = todos los monitores (escritorio virtual completo).
    ResolutionPreset resolution = ResolutionPreset::ClientNative;
    std::uint32_t custom_width = 1920;
    std::uint32_t custom_height = 1080;
    std::uint32_t fps = 60;
    bool fps_match_display = true;  // FPS = frecuencia del monitor donde está la ventana del cliente.
    CodecChoice codec = CodecChoice::Auto;
    bool auto_bitrate = true;
    std::uint32_t bitrate_mbps = 50;
    RateControl rate_control = RateControl::Cbr;
    bool chroma_444 = false;
    bool hdr = false;
    ScalingMode scaling = ScalingMode::Fit;
    bool vsync = false;
    bool audio = true;
    std::uint32_t audio_sample_rate = 0;  // 0 = la del sistema del host (normalmente 48000).
    bool audio_stereo = true;
    std::uint32_t audio_volume = 100;     // 0..100, se aplica en el cliente.
    std::uint32_t audio_buffer_ms = 20;   // Colchón de reproducción del cliente (menos = menor latencia).
    std::uint32_t fec_percent = 15;
    bool low_latency = true;  // Codificador en preset rápido y búfer de 1 frame (menos calidad por bit).
    // Recuperar pérdidas con intra-refresh en vez de keyframes. Apagado por defecto: medido con los
    // decodificadores de Windows, un keyframe recupera antes (descartan cuadros durante la ola de refresco).
    bool intra_refresh = false;
    bool slices = true;         // Envío por slices: la red empieza antes de que termine de codificarse el cuadro.
    bool adaptive = true;       // El host baja resolución/FPS solo si su codificador no da abasto.

    friend bool operator==(const StreamSettings&, const StreamSettings&) = default;
};

struct Profile {
    std::string name;
    StreamSettings stream;
};

struct HostSettings {
    bool enabled = true;
    std::uint16_t control_port = kDefaultControlPort;
    std::uint16_t media_port = kDefaultMediaPort;
    std::uint16_t web_port = kDefaultWebPort;
    bool web_enabled = false;  // Cliente web para celulares: pendiente.
    bool allow_input = true;
    bool allow_clipboard = true;
    bool allow_files = true;
    bool allow_audio = true;
    bool private_networks_only = true;
    std::vector<std::string> ip_whitelist;  // Vacía = cualquier IP privada.
    std::string receive_folder;             // Vacío = Descargas\LanCast.
    bool overwrite_existing = false;
};

struct ClientSettings {
    std::vector<std::string> recent_addresses;
    std::string receive_folder;
    StreamSettings stream;
    std::string active_profile;
    std::vector<Profile> profiles;
    bool view_only = false;
    bool clipboard_sync = true;
    bool relative_mouse = false;
    bool show_stats = false;
    std::uint32_t simulate_loss_percent = 0;  // Solo para pruebas.
};

// 2 (1.4): resolución y FPS "Igual a esta pantalla" por defecto.
inline constexpr std::uint32_t kDefaultsVersion = 2;

struct AppSettings {
    std::string device_id;    // 16 bytes aleatorios en hex; identifica este equipo al emparejar.
    std::string device_name;  // Nombre visible (por defecto, el nombre del equipo).
    bool start_with_windows = false;
    bool minimize_to_tray = true;
    bool check_updates = true;  // Buscar versiones nuevas en GitHub al iniciar.
    // Versión de los valores por defecto aplicados (migraciones únicas al actualizar LanCast).
    std::uint32_t defaults_version = kDefaultsVersion;
    HostSettings host;
    ClientSettings client;
};

[[nodiscard]] std::string settings_to_toml(const AppSettings& settings);
[[nodiscard]] AppSettings settings_from_toml(std::string_view text);

// Perfiles predefinidos ("Trabajo 1440p 4:4:4", "Juego 144 Hz", "Celular").
[[nodiscard]] std::vector<Profile> default_profiles();

[[nodiscard]] std::string_view to_string(CodecChoice c) noexcept;
[[nodiscard]] std::string_view to_string(RateControl r) noexcept;
[[nodiscard]] std::string_view to_string(ScalingMode s) noexcept;
[[nodiscard]] std::string_view to_string(ResolutionPreset r) noexcept;

// Resolución máxima pedida al host según el preset (la nativa del cliente se pasa como parámetro).
[[nodiscard]] Size requested_size(const StreamSettings& s, Size client_native) noexcept;

// Bitrate efectivo (automático o manual) en kbps.
[[nodiscard]] std::uint32_t effective_bitrate_kbps(const StreamSettings& s, Size stream_size, Codec codec) noexcept;

}  // namespace lancast
