#pragma once

// Mensajes del canal de control (TCP) y del canal de archivos.
//
// Trama: [u32 longitud][contenido]. Durante el handshake el contenido va en claro; después, cifrado con
// AES-256-GCM (nonce = contador por dirección). El contenido empieza siempre con un u8 MsgType.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lancast/core/bytes.hpp"
#include "lancast/core/settings.hpp"
#include "lancast/core/types.hpp"

namespace lancast::proto {

inline constexpr std::uint32_t kMagic = 0x534E434C;  // "LCNS"
inline constexpr std::uint16_t kVersion = 2;
inline constexpr std::size_t kMaxFrame = 32u * 1024 * 1024;

using DeviceId = std::array<std::uint8_t, 16>;
using PublicKey = std::array<std::uint8_t, 64>;  // ECDH P-256: X || Y
using Nonce = std::array<std::uint8_t, 32>;
using Mac = std::array<std::uint8_t, 32>;

enum class MsgType : std::uint8_t {
    // Handshake (en claro)
    Hello = 1,
    HelloAck = 2,
    AuthStart = 3,
    AuthStartAck = 4,
    AuthProof = 5,
    AuthResult = 6,

    // Sesión (cifrado)
    ClientCaps = 10,
    HostInfo = 11,
    StreamConfig = 12,
    StreamStarted = 13,
    StreamError = 14,
    RequestKeyframe = 15,
    Ping = 16,
    Pong = 17,
    MouseAbs = 20,
    MouseRel = 21,
    MouseButton = 22,
    MouseWheel = 23,
    Key = 24,
    ReleaseAll = 25,
    Clipboard = 26,
    CursorShape = 27,
    CursorPos = 28,
    Disconnect = 30,
    LatencyProbe = 31,  // Cliente -> host: mostrar el marcador en blanco/negro (id 0 = terminar).
    LatencyMark = 32,   // Host -> cliente: instante en que el marcador cambió y dónde está.
    StreamStopped = 33, // Host -> cliente: el stream secundario se detuvo (o no pudo iniciarse).
    SecureAttention = 34, // Cliente -> host: Ctrl+Alt+Supr (solo con el host como servicio).
    Text = 35,            // Cliente -> host: texto escrito (UTF-8, TextMessage); teclados de celular.

    // Canal de archivos
    FileHello = 40,
    FileOffer = 41,
    FileAccept = 42,
    FileReject = 43,
    FileChunk = 44,
    FileDone = 45,
    FileResult = 46,
    FileCancel = 47,
};

struct Hello {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    DeviceId device_id{};
    std::string device_name;
    PublicKey public_key{};
    Nonce nonce{};
    void write(ByteWriter& w) const;
    static std::optional<Hello> read(ByteReader& r);
};

struct HelloAck {
    std::uint16_t version = kVersion;
    DeviceId device_id{};
    std::string device_name;
    PublicKey public_key{};
    Nonce nonce{};
    bool has_pairing = false;  // El host conoce a este cliente.
    bool accepted = true;      // false = ocupado / IP no permitida (ver message).
    std::string message;
    void write(ByteWriter& w) const;
    static std::optional<HelloAck> read(ByteReader& r);
};

struct AuthStart {
    bool use_pairing = false;  // false = emparejamiento nuevo con PIN.
    void write(ByteWriter& w) const;
    static std::optional<AuthStart> read(ByteReader& r);
};

struct AuthStartAck {
    bool pin_required = false;
    void write(ByteWriter& w) const;
    static std::optional<AuthStartAck> read(ByteReader& r);
};

struct AuthProof {
    Mac mac{};
    void write(ByteWriter& w) const;
    static std::optional<AuthProof> read(ByteReader& r);
};

struct AuthResult {
    bool ok = false;
    std::string message;
    Mac mac{};
    std::array<std::uint8_t, 16> session_id{};
    void write(ByteWriter& w) const;
    static std::optional<AuthResult> read(ByteReader& r);
};

struct ClientCaps {
    std::string client_name;
    std::vector<Codec> decoders;
    std::uint32_t display_width = 0;
    std::uint32_t display_height = 0;
    std::uint32_t display_refresh = 60;
    bool supports_slices = false;  // Opcional (1.3+): sabe reensamblar frames enviados por slices.
    bool supports_444 = false;     // Opcional (1.4+): reconstruye 4:4:4 empaquetado (cuadro W×2H).
    bool supports_hdr = false;     // Opcional (1.4+): decodifica 10 bits BT.2020 PQ.
    void write(ByteWriter& w) const;
    static std::optional<ClientCaps> read(ByteReader& r);
};

struct RemoteMonitor {
    std::uint32_t id = 0;
    std::string name;
    Rect rect;
    std::uint32_t refresh_hz = 60;
    bool primary = false;
    bool hdr = false;
};

struct HostInfo {
    std::string host_name;
    std::vector<RemoteMonitor> monitors;
    std::vector<Codec> encoders;
    bool audio_available = false;
    bool input_allowed = true;
    bool clipboard_allowed = true;
    bool files_allowed = true;
    std::uint16_t media_port = 0;
    std::uint16_t file_port = 0;
    bool secure_desktop = false;  // Opcional (1.3+): host como servicio (bloqueo, UAC, Ctrl+Alt+Supr).
    void write(ByteWriter& w) const;
    static std::optional<HostInfo> read(ByteReader& r);
};

struct StreamConfig {
    std::uint32_t monitor_id = 1;
    std::uint32_t max_width = 1920;
    std::uint32_t max_height = 1080;
    std::uint32_t fps = 60;
    CodecChoice codec = CodecChoice::Auto;
    bool auto_bitrate = true;
    std::uint32_t bitrate_kbps = 50000;
    RateControl rate_control = RateControl::Cbr;
    bool chroma_444 = false;
    bool hdr = false;
    bool audio = true;
    std::uint8_t fec_percent = 15;
    std::uint32_t audio_rate = 0;     // 0 = la del sistema del host.
    std::uint8_t audio_channels = 2;  // 1 = mono, 2 = estéreo.
    bool low_latency = true;          // Opcional (1.2+): preset rápido del codificador.
    bool intra_refresh = false;       // Opcional (1.3+): recuperar pérdidas con intra-refresh.
    bool slices = true;               // Opcional (1.3+): envío por slices (sub-frame).
    std::uint8_t stream_index = 0;    // Opcional (1.3+): 0 = principal, 1 = segunda ventana.
    bool enabled = true;              // Opcional (1.3+): false = detener ese stream.
    bool adaptive = true;             // Opcional (1.4+): bajar resolución/FPS si el codificador no da abasto.
    void write(ByteWriter& w) const;
    static std::optional<StreamConfig> read(ByteReader& r);
};

struct StreamStarted {
    std::uint16_t epoch = 0;
    Codec codec = Codec::H264;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fps = 0;
    std::uint32_t bitrate_kbps = 0;
    std::uint32_t monitor_id = 0;
    Rect source;
    std::uint32_t audio_rate = 0;  // 0 = sin audio
    std::uint8_t audio_channels = 0;
    std::string notes;  // Avisos (p. ej. "4:4:4 no soportado, usando 4:2:0").
    bool intra_refresh = false;   // Opcional (1.3+): el cliente puede seguir decodificando tras una pérdida.
    bool slices = false;          // Opcional (1.3+): los frames llegan por slices.
    std::string encoder_backend;  // Opcional (1.3+): "AMF directo", "NVENC directo", "Media Foundation".
    std::uint8_t stream_index = 0;  // Opcional (1.3+).
    bool chroma_444 = false;  // Opcional (1.4+): 4:4:4 empaquetado; el cuadro codificado mide width × 2·height.
    bool hdr = false;         // Opcional (1.4+): 10 bits BT.2020 PQ (HDR10).
    void write(ByteWriter& w) const;
    static std::optional<StreamStarted> read(ByteReader& r);
};

// Motivo de RequestKeyframe (cuerpo opcional; sin cuerpo = NeedKeyframe).
enum class RecoveryReason : std::uint8_t {
    NeedKeyframe = 0,  // El decodificador necesita empezar de cero (nuevo stream, error).
    FrameLoss = 1,     // Se perdió un frame: vale una ola de intra-refresh si el stream la usa.
};

struct TextMessage {
    std::string text;
    void write(ByteWriter& w) const { w.str(text); }
    static std::optional<TextMessage> read(ByteReader& r) {
        TextMessage m{r.str()};
        return r.ok() ? std::optional{std::move(m)} : std::nullopt;
    }
};

struct PingPong {
    std::uint64_t client_time_us = 0;
    std::uint64_t host_time_us = 0;
    void write(ByteWriter& w) const { w.u64(client_time_us).u64(host_time_us); }
    static std::optional<PingPong> read(ByteReader& r) {
        PingPong p{r.u64(), r.u64()};
        return r.ok() ? std::optional{p} : std::nullopt;
    }
};

struct MouseAbs {
    float x = 0;  // Normalizado [0,1] en el stream.
    float y = 0;
    std::uint8_t stream_index = 0;  // Opcional (1.3+): en qué stream están las coordenadas.
    void write(ByteWriter& w) const { w.f32(x).f32(y).u8(stream_index); }
    static std::optional<MouseAbs> read(ByteReader& r) {
        MouseAbs m{r.f32(), r.f32()};
        if (r.ok() && r.remaining() >= 1) m.stream_index = r.u8();
        return r.ok() ? std::optional{m} : std::nullopt;
    }
};

struct MouseRel {
    std::int32_t dx = 0;
    std::int32_t dy = 0;
    void write(ByteWriter& w) const { w.i32(dx).i32(dy); }
    static std::optional<MouseRel> read(ByteReader& r) {
        MouseRel m{r.i32(), r.i32()};
        return r.ok() ? std::optional{m} : std::nullopt;
    }
};

enum class MouseButtonId : std::uint8_t { Left = 0, Right = 1, Middle = 2, X1 = 3, X2 = 4 };

struct MouseButton {
    MouseButtonId button = MouseButtonId::Left;
    bool down = false;
    void write(ByteWriter& w) const { w.u8(static_cast<std::uint8_t>(button)).boolean(down); }
    static std::optional<MouseButton> read(ByteReader& r) {
        MouseButton m{static_cast<MouseButtonId>(r.u8()), r.boolean()};
        return r.ok() && static_cast<int>(m.button) <= 4 ? std::optional{m} : std::nullopt;
    }
};

struct MouseWheel {
    std::int32_t delta = 0;  // Múltiplos de 120 (WHEEL_DELTA), admite valores finos.
    bool horizontal = false;
    void write(ByteWriter& w) const { w.i32(delta).boolean(horizontal); }
    static std::optional<MouseWheel> read(ByteReader& r) {
        MouseWheel m{r.i32(), r.boolean()};
        return r.ok() ? std::optional{m} : std::nullopt;
    }
};

struct Key {
    std::uint16_t scancode = 0;
    bool extended = false;
    bool down = false;
    void write(ByteWriter& w) const { w.u16(scancode).boolean(extended).boolean(down); }
    static std::optional<Key> read(ByteReader& r) {
        Key k{r.u16(), r.boolean(), r.boolean()};
        return r.ok() ? std::optional{k} : std::nullopt;
    }
};

enum class CursorType : std::uint8_t { Monochrome = 1, Color = 2, MaskedColor = 4 };

struct CursorShape {
    CursorType type = CursorType::Color;
    std::uint32_t width = 0;
    std::uint32_t height = 0;  // Para monocromo: alto total (AND + XOR).
    std::uint32_t pitch = 0;
    std::int32_t hot_x = 0;
    std::int32_t hot_y = 0;
    std::vector<std::uint8_t> data;
    void write(ByteWriter& w) const;
    static std::optional<CursorShape> read(ByteReader& r);
};

struct CursorPos {
    bool visible = false;
    float x = 0;  // Normalizado en el stream.
    float y = 0;
    std::uint8_t stream_index = 0;  // Opcional (1.3+).
    void write(ByteWriter& w) const { w.boolean(visible).f32(x).f32(y).u8(stream_index); }
    static std::optional<CursorPos> read(ByteReader& r) {
        CursorPos c{r.boolean(), r.f32(), r.f32()};
        if (r.ok() && r.remaining() >= 1) c.stream_index = r.u8();
        return r.ok() ? std::optional{c} : std::nullopt;
    }
};

struct StreamStoppedMsg {
    std::uint8_t stream_index = 1;
    std::string reason;
    void write(ByteWriter& w) const { w.u8(stream_index).str(reason); }
    static std::optional<StreamStoppedMsg> read(ByteReader& r) {
        StreamStoppedMsg m;
        m.stream_index = r.u8();
        m.reason = r.str(4096);
        return r.ok() ? std::optional{std::move(m)} : std::nullopt;
    }
};

// --- Medición de latencia real (captura -> pantalla del cliente) ---

struct LatencyProbe {
    std::uint32_t id = 0;  // 0 = fin de la medición (el host quita el marcador).
    bool white = false;
    void write(ByteWriter& w) const { w.u32(id).boolean(white); }
    static std::optional<LatencyProbe> read(ByteReader& r) {
        LatencyProbe p{r.u32(), r.boolean()};
        return r.ok() ? std::optional{p} : std::nullopt;
    }
};

struct LatencyMark {
    std::uint32_t id = 0;
    std::uint64_t host_time_us = 0;  // Cuándo quedó el cambio en la pantalla del host (reloj del host).
    float x = 0;                     // Centro del marcador, normalizado en el stream.
    float y = 0;
    bool white = false;
    void write(ByteWriter& w) const { w.u32(id).u64(host_time_us).f32(x).f32(y).boolean(white); }
    static std::optional<LatencyMark> read(ByteReader& r) {
        LatencyMark m;
        m.id = r.u32();
        m.host_time_us = r.u64();
        m.x = r.f32();
        m.y = r.f32();
        m.white = r.boolean();
        return r.ok() ? std::optional{m} : std::nullopt;
    }
};

// --- Archivos ---

struct FileHello {
    std::array<std::uint8_t, 16> session_id{};
    void write(ByteWriter& w) const { w.raw(session_id); }
    static std::optional<FileHello> read(ByteReader& r) {
        FileHello h{r.fixed<16>()};
        return r.ok() ? std::optional{h} : std::nullopt;
    }
};

struct FileOffer {
    std::uint32_t transfer_id = 0;
    std::string relative_path;  // Con '/' como separador.
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    void write(ByteWriter& w) const { w.u32(transfer_id).str(relative_path).u64(size).i64(mtime); }
    static std::optional<FileOffer> read(ByteReader& r) {
        FileOffer o;
        o.transfer_id = r.u32();
        o.relative_path = r.str(4096);
        o.size = r.u64();
        o.mtime = r.i64();
        return r.ok() ? std::optional{std::move(o)} : std::nullopt;
    }
};

struct FileAccept {
    std::uint32_t transfer_id = 0;
    std::uint64_t offset = 0;  // > 0 = reanudar.
    void write(ByteWriter& w) const { w.u32(transfer_id).u64(offset); }
    static std::optional<FileAccept> read(ByteReader& r) {
        FileAccept a{r.u32(), r.u64()};
        return r.ok() ? std::optional{a} : std::nullopt;
    }
};

struct FileStatus {
    std::uint32_t transfer_id = 0;
    bool ok = false;
    std::string message;
    void write(ByteWriter& w) const { w.u32(transfer_id).boolean(ok).str(message); }
    static std::optional<FileStatus> read(ByteReader& r) {
        FileStatus s;
        s.transfer_id = r.u32();
        s.ok = r.boolean();
        s.message = r.str();
        return r.ok() ? std::optional{std::move(s)} : std::nullopt;
    }
};

struct FileChunkHeader {
    std::uint32_t transfer_id = 0;
    std::uint64_t offset = 0;
    // Seguido de los datos (resto del mensaje).
};

struct FileDone {
    std::uint32_t transfer_id = 0;
    std::array<std::uint8_t, 32> sha256{};
    void write(ByteWriter& w) const { w.u32(transfer_id).raw(sha256); }
    static std::optional<FileDone> read(ByteReader& r) {
        FileDone d{r.u32(), r.fixed<32>()};
        return r.ok() ? std::optional{d} : std::nullopt;
    }
};

// Serializa tipo + cuerpo.
template <typename T>
[[nodiscard]] std::vector<std::uint8_t> encode(MsgType type, const T& body) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(type));
    body.write(w);
    return w.take();
}
[[nodiscard]] inline std::vector<std::uint8_t> encode(MsgType type) { return {static_cast<std::uint8_t>(type)}; }

// --- Descubrimiento (UDP broadcast, en claro) ---
inline constexpr std::uint32_t kDiscoveryMagic = 0x4353434C;  // "LCSC"

struct DiscoveryReply {
    std::string host_name;
    DeviceId device_id{};
    std::uint16_t control_port = 0;
    std::uint16_t web_port = 0;
    bool busy = false;
    void write(ByteWriter& w) const;
    static std::optional<DiscoveryReply> read(ByteReader& r);
};

}  // namespace lancast::proto
