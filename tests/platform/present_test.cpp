// Presentación del cliente en ventana: el procesador de video debe dibujar el cuadro a cualquier tamaño de ventana
// (antes, con ciertos tamaños la ventana quedaba negra).

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

#include "lancast/core/geometry.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/video_processor.hpp"

using namespace lancast;

namespace {

gpu::ComPtr<ID3D11Texture2D> solid_bgra(const gpu::Device& dev, Size size, std::uint32_t bgra) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(size.width) * size.height, bgra);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = size.width;
    d.Height = size.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const D3D11_SUBRESOURCE_DATA init{px.data(), size.width * 4, 0};
    gpu::ComPtr<ID3D11Texture2D> t;
    dev.device->CreateTexture2D(&d, &init, &t);
    return t;
}

std::uint32_t read_pixel(const gpu::Device& dev, ID3D11Texture2D* tex, UINT x, UINT y) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 1;
    d.Height = 1;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    gpu::ComPtr<ID3D11Texture2D> staging;
    dev.device->CreateTexture2D(&d, nullptr, &staging);
    const D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
    dev.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    dev.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    const std::uint32_t v = *static_cast<const std::uint32_t*>(m.pData);
    dev.context->Unmap(staging.Get(), 0);
    return v;
}

}  // namespace

TEST(Present, VideoFillsWindowAtAnySize) {
    auto dev = gpu::create_device(std::nullopt);
    if (!dev) GTEST_SKIP() << dev.error().message();

    // Cuadro "decodificado": NV12 de 1920x1080 (como el que entrega el decodificador) con un gris claro.
    const Size stream{1920, 1080};
    auto src = solid_bgra(*dev, stream, 0xFFC0C0C0);
    auto nv12 = gpu::create_texture(dev->device.Get(), stream.width, stream.height, DXGI_FORMAT_NV12,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ASSERT_TRUE(nv12.has_value());
    auto enc = gpu::VideoProcessor::create(*dev, stream, stream, gpu::ColorConversion::RgbToYuv709);
    ASSERT_TRUE(enc.has_value());
    ASSERT_TRUE((*enc)->process(src.Get(), 0, nv12->Get(), nullptr, nullptr));

    int failures = 0;
    int tested = 0;
    for (std::uint32_t w = 320; w <= 2560; w += 53) {
        for (std::uint32_t h = 200; h <= 1440; h += 47) {
            ++tested;
            const Size window{w, h};
            auto target = solid_bgra(*dev, window, 0xFF0000FF);  // Rojo: si queda rojo, no se dibujó.
            auto vp = gpu::VideoProcessor::create(*dev, stream, window, gpu::ColorConversion::YuvToRgb709);
            if (!vp) {
                if (++failures <= 10) std::printf("  %ux%u: no se creó el procesador: %s\n", w, h, vp.error().message().c_str());
                continue;
            }
            const Rect view = view_rect(stream, window, ScalingMode::Fit);
            const RECT s{0, 0, static_cast<LONG>(stream.width), static_cast<LONG>(stream.height)};
            const RECT d{view.x, view.y, view.right(), view.bottom()};
            const bool ok = (*vp)->process(nv12->Get(), 0, target.Get(), &s, &d);
            const std::uint32_t center = read_pixel(*dev, target.Get(), w / 2, h / 2);
            const int g = static_cast<int>((center >> 8) & 0xFF);
            if (!ok || g < 150) {
                if (++failures <= 10) {
                    std::printf("  %ux%u: Blt %s, vista %dx%d en (%d,%d), centro 0x%08X\n", w, h, ok ? "ok" : "falló", view.width,
                                view.height, view.x, view.y, center);
                }
            }
        }
    }
    std::printf("  %d tamaños de ventana probados, %d sin video\n", tested, failures);
    EXPECT_EQ(failures, 0);
}

#include "lancast/platform/color.hpp"

// Como el decodificador: textura NV12 con alto alineado (1088), en un array, con el cuadro visible de 1080.
TEST(Present, AllScalingModesAndShaderPath) {
    auto dev = gpu::create_device(std::nullopt);
    if (!dev) GTEST_SKIP() << dev.error().message();
    const Size stream{1920, 1080};
    auto src = solid_bgra(*dev, stream, 0xFFC0C0C0);

    D3D11_TEXTURE2D_DESC ad{};
    ad.Width = 1920;
    ad.Height = 1088;
    ad.MipLevels = 1;
    ad.ArraySize = 4;
    ad.Format = DXGI_FORMAT_NV12;
    ad.SampleDesc.Count = 1;
    ad.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;  // Como el pool del decodificador.
    gpu::ComPtr<ID3D11Texture2D> decoded;
    ASSERT_TRUE(SUCCEEDED(dev->device->CreateTexture2D(&ad, nullptr, &decoded)));
    auto nv12 = gpu::create_texture(dev->device.Get(), 1920, 1088, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    auto enc = gpu::VideoProcessor::create(*dev, stream, {1920, 1088}, gpu::ColorConversion::RgbToYuv709);
    ASSERT_TRUE(enc.has_value());
    ASSERT_TRUE((*enc)->process(src.Get(), 0, nv12->Get(), nullptr, nullptr));
    dev->context->CopySubresourceRegion(decoded.Get(), D3D11CalcSubresource(0, 2, 1), 0, 0, 0, nv12->Get(), 0, nullptr);

    // Para el camino por shader (4:4:4): cuadro empaquetado W×2H.
    gpu::PackConfig pc{stream, DXGI_FORMAT_B8G8R8A8_UNORM, stream, true, false};
    auto packer = gpu::YuvPacker::create(*dev, pc);
    ASSERT_TRUE(packer.has_value());
    auto packed = gpu::create_texture(dev->device.Get(), 1920, 2160, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ASSERT_TRUE((*packer)->process(src.Get(), packed->Get()));
    auto yuv = gpu::YuvRenderer::create(*dev, stream, true, false);
    ASSERT_TRUE(yuv.has_value());
    (*yuv)->convert(packed->Get(), 0);

    int failures = 0;
    int tested = 0;
    for (const ScalingMode mode : {ScalingMode::Fit, ScalingMode::Stretch, ScalingMode::Native}) {
        for (std::uint32_t w = 320; w <= 2560; w += 97) {
            for (std::uint32_t h = 200; h <= 1440; h += 89) {
                const Size window{w, h};
                const Rect view = view_rect(stream, window, mode);
                const RECT s{0, 0, 1920, 1080};
                const RECT d{view.x, view.y, view.right(), view.bottom()};
                // Procesador de video (cuadros 4:2:0).
                {
                    ++tested;
                    auto target = solid_bgra(*dev, window, 0xFF0000FF);
                    auto vp = gpu::VideoProcessor::create(*dev, stream, window, gpu::ColorConversion::YuvToRgb709);
                    const bool ok = vp && (*vp)->process(decoded.Get(), 2, target.Get(), &s, &d);
                    const int g = static_cast<int>((read_pixel(*dev, target.Get(), w / 2, h / 2) >> 8) & 0xFF);
                    if (!ok || g < 150) {
                        if (++failures <= 12) std::printf("  VP %s %ux%u: Blt %s, vista %dx%d en (%d,%d), verde %d\n",
                                                          mode == ScalingMode::Fit ? "ajustar" : mode == ScalingMode::Stretch ? "estirar" : "1:1",
                                                          w, h, ok ? "ok" : "falló", view.width, view.height, view.x, view.y, g);
                    }
                }
                // Shader (4:4:4).
                {
                    ++tested;
                    auto target = solid_bgra(*dev, window, 0xFF0000FF);
                    gpu::ComPtr<ID3D11RenderTargetView> rtv;
                    dev->device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
                    (*yuv)->present(rtv.Get(), window, view, false, 80, 1000);
                    const int g = static_cast<int>((read_pixel(*dev, target.Get(), w / 2, h / 2) >> 8) & 0xFF);
                    if (g < 150 && ++failures <= 12) std::printf("  shader %ux%u: vista %dx%d, verde %d\n", w, h, view.width, view.height, g);
                }
            }
        }
    }
    std::printf("  %d combinaciones probadas, %d sin video\n", tested, failures);
    EXPECT_EQ(failures, 0);
}
