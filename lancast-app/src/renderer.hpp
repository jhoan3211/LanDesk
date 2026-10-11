#pragma once

// Presentación del video en el cliente: decodificador por hardware (MF/DXVA) -> procesador de video
// (NV12 -> BGRA con escalado) -> back buffer de la swapchain. Todo en la GPU.

#include <memory>
#include <optional>
#include <string>

#include "lancast/client/client_session.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/video_processor.hpp"

namespace lancast::app {

class VideoRenderer {
public:
    explicit VideoRenderer(const gpu::Device& device) : device_(device) {}

    // Decodifica un frame; devuelve true si hay una imagen nueva lista para dibujar.
    bool decode(const client::Frame& frame, const proto::StreamStarted& stream, double& decode_ms);
    // Dibuja la última imagen en el back buffer dentro de `view` (el resto queda negro).
    bool draw(ID3D11Texture2D* backbuffer, Size backbuffer_size, const Rect& view);

    void reset();
    // Suelta las vistas sobre el back buffer (necesario antes de redimensionar la swapchain).
    void release_target() { vp_.reset(); }
    [[nodiscard]] bool has_picture() const noexcept { return last_.has_value(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::string decoder_name() const { return decoder_ ? decoder_->name() : std::string{}; }
    [[nodiscard]] bool needs_keyframe() noexcept {
        const bool v = needs_keyframe_;
        needs_keyframe_ = false;
        return v;
    }

private:
    const gpu::Device& device_;
    std::unique_ptr<mf::Decoder> decoder_;
    std::uint16_t epoch_ = 0xFFFF;
    Codec codec_ = Codec::H264;
    Size size_;
    std::unique_ptr<gpu::VideoProcessor> vp_;
    Size vp_out_;
    std::optional<mf::DecodedFrame> last_;
    std::string error_;
    bool needs_keyframe_ = false;
    bool draw_failed_logged_ = false;
};

}  // namespace lancast::app
