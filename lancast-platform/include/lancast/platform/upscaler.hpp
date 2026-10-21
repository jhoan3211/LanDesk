#pragma once

// Superresolución AMD (AMF HQ Scaler con VideoSR): agranda el video decodificado reconstruyendo bordes y texto, como
// "RTX Video Super Resolution" de NVIDIA. Idea: el host transmite a menos resolución (codifica y decodifica más rápido)
// y el cliente lo agranda a la de su pantalla. Solo en GPU AMD (amfrt64.dll del driver).

#include <memory>

#include "lancast/core/types.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/error.hpp"

namespace lancast::gpu {

enum class UpscaleAlgorithm { Bilinear, Bicubic, VideoSr10, VideoSr11 };

[[nodiscard]] const char* to_string(UpscaleAlgorithm a) noexcept;

class Upscaler {
public:
    virtual ~Upscaler() = default;
    // Agranda `input` (NV12, del tamaño de entrada) y devuelve la textura de salida (válida hasta la próxima llamada).
    // Vuelve cuando la GPU terminó de escribirla.
    virtual platform::Result<ComPtr<ID3D11Texture2D>> process(ID3D11Texture2D* input) = 0;
    [[nodiscard]] virtual DXGI_FORMAT output_format() const = 0;
};

[[nodiscard]] platform::Result<std::unique_ptr<Upscaler>> create_amd_upscaler(const Device& device, Size input, Size output,
                                                                             UpscaleAlgorithm algorithm);

}  // namespace lancast::gpu
