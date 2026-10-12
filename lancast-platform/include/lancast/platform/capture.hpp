#pragma once

// Captura de pantalla con DXGI Desktop Duplication. El frame queda en una textura BGRA de la GPU
// (cero copias a CPU). Para "Todos los monitores" compone cada salida en una textura del tamaño del
// escritorio virtual. El cursor no se incluye en la imagen: su forma y posición se entregan aparte.

#include <memory>
#include <optional>
#include <vector>

#include "lancast/core/protocol.hpp"
#include "lancast/platform/d3d.hpp"

namespace lancast::capture {

enum class Status { NewFrame, Timeout, Recreated, Error };

struct PointerState {
    bool visible = false;
    std::int32_t x = 0;  // Escritorio virtual.
    std::int32_t y = 0;
};

class DesktopCapture {
public:
    // monitor_rects: rectángulos (escritorio virtual) de los monitores a capturar; source: unión de ellos.
    // hdr: captura en FP16 scRGB (monitores en modo HDR); si no, BGRA de 8 bits.
    static platform::Result<std::unique_ptr<DesktopCapture>> create(const gpu::Device& device,
                                                                     const std::vector<Rect>& monitor_rects,
                                                                     const Rect& source, bool hdr = false);
    ~DesktopCapture();

    Status acquire(std::uint32_t timeout_ms);

    [[nodiscard]] ID3D11Texture2D* texture() const noexcept { return composite_.Get(); }
    [[nodiscard]] const Rect& source() const noexcept { return source_; }
    // Rotación (grados) del monitor capturado; solo se aplica en captura de un único monitor.
    [[nodiscard]] std::uint32_t rotation() const noexcept { return rotation_; }
    // Tamaño de la textura capturada (sin rotar).
    [[nodiscard]] Size texture_size() const noexcept { return texture_size_; }
    [[nodiscard]] DXGI_FORMAT format() const noexcept {
        return hdr_ ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    // Instante (µs, reloj QPC) en que Windows presentó la última imagen capturada.
    [[nodiscard]] std::uint64_t last_present_us() const noexcept { return last_present_us_; }

    [[nodiscard]] const PointerState& pointer() const noexcept { return pointer_; }
    // Devuelve la nueva forma del cursor si cambió desde la última llamada.
    std::optional<proto::CursorShape> take_pointer_shape();

private:
    struct Output;
    DesktopCapture() = default;
    platform::Result<void> init_outputs();
    Status acquire_output(Output& out, std::uint32_t timeout_ms, bool& updated);

    gpu::Device device_;
    std::vector<Rect> monitor_rects_;
    Rect source_;
    std::vector<std::unique_ptr<Output>> outputs_;
    gpu::ComPtr<ID3D11Texture2D> composite_;
    Size texture_size_;
    std::uint32_t rotation_ = 0;
    bool hdr_ = false;
    PointerState pointer_;
    std::optional<proto::CursorShape> pending_shape_;
    std::uint64_t last_recreate_us_ = 0;
    std::uint64_t last_present_us_ = 0;
};

}  // namespace lancast::capture
