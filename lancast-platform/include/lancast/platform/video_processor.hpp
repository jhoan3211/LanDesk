#pragma once

// Conversión de color y escalado por hardware con el procesador de video de D3D11
// (BGRA -> NV12 BT.709 en el host; NV12 -> BGRA en el cliente). Todo ocurre en la GPU.

#include <map>
#include <memory>

#include "lancast/core/types.hpp"
#include "lancast/platform/d3d.hpp"

namespace lancast::gpu {

enum class ColorConversion { RgbToYuv709, YuvToRgb709 };

class VideoProcessor {
public:
    static platform::Result<std::unique_ptr<VideoProcessor>> create(const Device& device, Size input, Size output,
                                                                    ColorConversion conversion);

    // Convierte `input` (slice `input_slice` si es un array) en `output`.
    // src/dst: rectángulos dentro de la entrada/salida; dst vacío = toda la salida.
    // rotation: 0/90/180/270 aplicada a la entrada. El fondo fuera de dst se pinta de negro.
    bool process(ID3D11Texture2D* input, UINT input_slice, ID3D11Texture2D* output, const RECT* src, const RECT* dst,
                 std::uint32_t rotation = 0);

    [[nodiscard]] Size input_size() const noexcept { return input_; }
    [[nodiscard]] Size output_size() const noexcept { return output_; }

private:
    VideoProcessor() = default;

    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    Size input_;
    Size output_;
    ColorConversion conversion_ = ColorConversion::RgbToYuv709;

    // Caché de vistas por textura (las texturas del pool se reutilizan).
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> input_views_;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorOutputView>> output_views_;
};

}  // namespace lancast::gpu
