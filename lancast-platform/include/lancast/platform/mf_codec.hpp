#pragma once

// Codificación y decodificación por hardware con Media Foundation.
// - Codificador: el MFT de hardware del driver (NVENC / AMF / QuickSync) con entrada de textura D3D11 NV12.
// - Decodificador: el MFT de decodificación de Windows con DXVA (D3D11), salida en textura NV12 de la GPU.
// Así LanCast es un único .exe sin DLL de terceros.

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lancast/core/settings.hpp"
#include "lancast/core/types.hpp"
#include "lancast/platform/d3d.hpp"

namespace lancast::mf {

// Inicializa Media Foundation mientras viva el objeto.
class MfScope {
public:
    MfScope();
    ~MfScope();
    MfScope(const MfScope&) = delete;
    MfScope& operator=(const MfScope&) = delete;
};

struct EncoderConfig {
    Codec codec = Codec::H264;
    Size size;
    std::uint32_t fps = 60;
    std::uint32_t bitrate_kbps = 20000;
    RateControl rate_control = RateControl::Cbr;
    bool low_latency = true;    // Preset más rápido y VBV de 1 frame.
    bool direct = true;         // Usar NVENC/AMF directamente si la GPU lo permite (si no, Media Foundation).
    bool intra_refresh = false; // Recuperación por intra-refresh continuo en vez de keyframes completos.
    bool slices = false;        // Entregar el frame por slices a medida que se codifican (envío sub-frame).
    bool ten_bit = false;       // HDR: entrada P010 BT.2020 PQ, HEVC Main10 / AV1 10 bits (solo AMF y NVENC directos).
};

struct EncodedPacket {
    std::vector<std::uint8_t> data;
    bool keyframe = false;
    std::uint64_t timestamp_us = 0;
    // Modo slices: un frame llega en varios paquetes; el último lleva last_slice = true.
    std::uint8_t slice_index = 0;
    bool last_slice = true;
};

class Encoder {
public:
    using OutputCallback = std::function<void(EncodedPacket&&)>;

    virtual ~Encoder() = default;
    // Modo en paralelo: cada frame codificado se entrega a este callback (desde un hilo de Media Foundation)
    // en cuanto está listo, mientras el llamador ya envía el siguiente frame con submit().
    virtual void set_output_callback(OutputCallback callback) = 0;
    // Entrega un frame y vuelve en cuanto el codificador lo acepta. `nv12` debe seguir válido hasta que
    // el codificador lo consuma (usar un pool de varias texturas).
    virtual platform::Result<void> submit(ID3D11Texture2D* nv12, std::uint64_t timestamp_us, bool force_keyframe) = 0;
    // `nv12` debe seguir válido hasta la siguiente llamada (usar un pool de texturas).
    virtual platform::Result<std::optional<EncodedPacket>> encode(ID3D11Texture2D* nv12, std::uint64_t timestamp_us,
                                                                  bool force_keyframe) = 0;
    virtual bool set_bitrate(std::uint32_t kbps) = 0;
    // ¿El codificador mantiene intra-refresh continuo? (el cliente puede seguir decodificando tras una pérdida)
    [[nodiscard]] virtual bool intra_refresh_active() const { return false; }
    // Ante una pérdida: ola de intra-refresh en lugar de keyframe (NVENC). AMF usa intra-refresh continuo.
    virtual void request_intra_refresh() {}
    // ¿Entrega el frame por slices (EncodedPacket::last_slice)?
    [[nodiscard]] virtual bool slices_active() const { return false; }
    // "Media Foundation", "AMF directo", "NVENC directo".
    [[nodiscard]] virtual std::string backend() const = 0;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual const EncoderConfig& config() const = 0;
};

[[nodiscard]] platform::Result<std::unique_ptr<Encoder>> create_encoder(const gpu::Device& device,
                                                                        const EncoderConfig& config);

// Códecs que este equipo puede codificar por hardware en la GPU dada.
[[nodiscard]] std::vector<Codec> available_encoders(std::uint64_t adapter_luid);

struct DecodedFrame {
    gpu::ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    Size size;  // Tamaño visible (sin el relleno de alineación del decodificador).
    gpu::ComPtr<IUnknown> keep_alive;  // Mantiene la superficie fuera del pool hasta presentarla.
};

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual platform::Result<std::optional<DecodedFrame>> decode(std::span<const std::uint8_t> data,
                                                                 std::uint64_t timestamp_us) = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

// ten_bit: el stream es de 10 bits (HDR): salida P010 en vez de NV12.
[[nodiscard]] platform::Result<std::unique_ptr<Decoder>> create_decoder(const gpu::Device& device, Codec codec, Size size,
                                                                        bool ten_bit = false);

// Códecs que este equipo puede decodificar (MFT de Windows con aceleración por hardware).
[[nodiscard]] std::vector<Codec> available_decoders();

}  // namespace lancast::mf
