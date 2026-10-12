#pragma once

// Conversión de color con shaders propios (para lo que el procesador de video de D3D11 no hace):
//
// - 4:4:4 real con cualquier códec y decodificador 4:2:0: el host empaca el color completo en un cuadro de doble
//   alto (W×2H). Arriba va un 4:2:0 normal (con la muestra de color par/par); abajo, en el plano de luma, el color
//   de las columnas impares, y en el plano de color, el de las filas impares. El cliente lo vuelve a armar: cada
//   píxel recupera su color exacto. (Mismo principio que AVC444 de Escritorio remoto.)
// - HDR de 10 bits: el host convierte la captura FP16 (scRGB) a BT.2020 + PQ en P010; el cliente lo devuelve a
//   scRGB para una pantalla HDR o lo adapta (tone mapping) para una SDR.

#include <map>
#include <memory>
#include <utility>

#include "lancast/core/types.hpp"
#include "lancast/platform/d3d.hpp"

struct HMONITOR__;

namespace lancast::gpu {

// Tamaño codificado: el doble de alto con 4:4:4 empaquetado.
[[nodiscard]] constexpr Size coded_size(Size logical, bool chroma_444) noexcept {
    return chroma_444 ? Size{logical.width, logical.height * 2} : logical;
}

struct PackConfig {
    Size input;                                     // Textura capturada.
    DXGI_FORMAT input_format = DXGI_FORMAT_B8G8R8A8_UNORM;  // o R16G16B16A16_FLOAT (scRGB, HDR)
    Size output;                                    // Tamaño lógico del stream (W×H, pares).
    bool chroma_444 = false;
    bool hdr = false;                               // BT.2020 PQ 10 bits (P010); si no, BT.709 8 bits (NV12).
};

// Host: captura -> NV12/P010 (con escalado bilineal y empaquetado 4:4:4 opcional).
class YuvPacker {
public:
    static platform::Result<std::unique_ptr<YuvPacker>> create(const Device& device, const PackConfig& config);

    [[nodiscard]] Size coded_size() const noexcept { return gpu::coded_size(config_.output, config_.chroma_444); }
    [[nodiscard]] DXGI_FORMAT format() const noexcept { return config_.hdr ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12; }
    // `output`: textura de coded_size() y format() con BIND_RENDER_TARGET.
    bool process(ID3D11Texture2D* input, ID3D11Texture2D* output);

private:
    YuvPacker() = default;
    Device device_;
    PackConfig config_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_y_;
    ComPtr<ID3D11PixelShader> ps_uv_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> inputs_;
    std::map<ID3D11Texture2D*, std::pair<ComPtr<ID3D11RenderTargetView>, ComPtr<ID3D11RenderTargetView>>> outputs_;
};

// Cliente: NV12/P010 decodificado (4:2:0 o 4:4:4 empaquetado) -> imagen RGB del tamaño lógico -> back buffer.
class YuvRenderer {
public:
    static platform::Result<std::unique_ptr<YuvRenderer>> create(const Device& device, Size logical, bool chroma_444,
                                                                 bool hdr);

    // Paso 1: reconstruye el color. `decoded` es la superficie del decodificador (puede ser un array).
    bool convert(ID3D11Texture2D* decoded, UINT subresource);
    // Imagen reconstruida: RGBA8 con gamma (SDR) o RGBA16F scRGB lineal (HDR; 1.0 = 80 nits).
    [[nodiscard]] ID3D11Texture2D* image() const noexcept { return image_.Get(); }

    // Paso 2: dibuja la imagen escalada dentro de `view` (lo de afuera queda negro).
    // target_hdr: back buffer scRGB (FP16); si no, SDR 8 bits (con tone mapping si el stream es HDR).
    bool present(ID3D11RenderTargetView* target, Size target_size, const Rect& view, bool target_hdr,
                 float sdr_white_nits, float peak_nits);

    [[nodiscard]] bool hdr() const noexcept { return hdr_; }

private:
    YuvRenderer() = default;
    Device device_;
    Size logical_;
    bool chroma_444_ = false;
    bool hdr_ = false;
    ComPtr<ID3D11Texture2D> copy_;  // Copia del cuadro decodificado con vistas de shader.
    ComPtr<ID3D11ShaderResourceView> copy_y_;
    ComPtr<ID3D11ShaderResourceView> copy_uv_;
    ComPtr<ID3D11Texture2D> image_;
    ComPtr<ID3D11RenderTargetView> image_rtv_;
    ComPtr<ID3D11ShaderResourceView> image_srv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_unpack_;
    ComPtr<ID3D11PixelShader> ps_present_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
};

// Salida HDR: dibuja la interfaz (BGRA8 premultiplicado, con gamma) sobre un back buffer scRGB al brillo de
// "contenido SDR" que el usuario eligió en Windows.
class UiCompositor {
public:
    static platform::Result<std::unique_ptr<UiCompositor>> create(const Device& device);
    bool draw(ID3D11ShaderResourceView* ui, ID3D11RenderTargetView* target, Size size, float sdr_white_nits);

private:
    UiCompositor() = default;
    Device device_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_;
};

struct DisplayColor {
    bool hdr = false;               // La pantalla está en modo HDR (BT.2020 PQ).
    float sdr_white_nits = 80.0f;   // Brillo del contenido SDR elegido en Windows.
    float peak_nits = 1000.0f;
};
// Estado HDR del monitor (para decidir la swapchain del cliente).
[[nodiscard]] DisplayColor display_color(HMONITOR__* monitor);

}  // namespace lancast::gpu
