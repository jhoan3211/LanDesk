#include "renderer.hpp"

#include "lancast/core/log.hpp"

namespace lancast::app {

void VideoRenderer::reset() {
    last_.reset();
    decoder_.reset();
    vp_.reset();
    epoch_ = 0xFFFF;
}

bool VideoRenderer::decode(const client::Frame& frame, const proto::StreamStarted& stream, double& decode_ms) {
    const Size size{stream.width, stream.height};
    if (!decoder_ || epoch_ != stream.epoch || codec_ != stream.codec || size_ != size) {
        // Nueva configuración: solo se puede empezar por un keyframe.
        if (!frame.frame.info.keyframe) {
            needs_keyframe_ = true;
            return false;
        }
        last_.reset();
        decoder_.reset();
        auto dec = mf::create_decoder(device_, stream.codec, size);
        if (!dec) {
            error_ = dec.error().message();
            log::error("Decodificador: {}", error_);
            return false;
        }
        decoder_ = std::move(*dec);
        epoch_ = stream.epoch;
        codec_ = stream.codec;
        size_ = size;
        error_.clear();
    }

    const std::uint64_t t0 = gpu::now_us();
    auto out = decoder_->decode(frame.frame.data, frame.frame.info.capture_time_us);
    decode_ms = (gpu::now_us() - t0) / 1000.0;
    if (!out) {
        error_ = out.error().message();
        log::warn("Error al decodificar: {}", error_);
        decoder_.reset();
        needs_keyframe_ = true;
        return false;
    }
    if (!out->has_value()) {
        // El decodificador se tragó varios cuadros seguidos (p. ej. referencia perdida): pedir un keyframe.
        if (++inputs_without_output_ >= 8) {
            needs_keyframe_ = true;
            inputs_without_output_ = 0;
        }
        return false;
    }
    inputs_without_output_ = 0;
    last_ = std::move(**out);
    return true;
}

bool VideoRenderer::draw(ID3D11Texture2D* backbuffer, Size backbuffer_size, const Rect& view) {
    if (!last_) return false;
    if (!vp_ || vp_->input_size() != size_ || vp_out_ != backbuffer_size) {
        auto vp = gpu::VideoProcessor::create(device_, size_, backbuffer_size, gpu::ColorConversion::YuvToRgb709);
        if (!vp) {
            error_ = vp.error().message();
            return false;
        }
        vp_ = std::move(*vp);
        vp_out_ = backbuffer_size;
    }
    const RECT src{0, 0, static_cast<LONG>(last_->size.width), static_cast<LONG>(last_->size.height)};
    const RECT dst{view.x, view.y, view.right(), view.bottom()};
    const bool ok = vp_->process(last_->texture.Get(), last_->subresource, backbuffer, &src, &dst);
    if (!ok && !draw_failed_logged_) {
        log::error("No se pudo dibujar el video: {}x{} -> back buffer {}x{}, destino {}x{} en ({},{})", size_.width,
                   size_.height, backbuffer_size.width, backbuffer_size.height, view.width, view.height, view.x, view.y);
        draw_failed_logged_ = true;
    }
    if (ok) draw_failed_logged_ = false;
    return ok;
}

}  // namespace lancast::app
