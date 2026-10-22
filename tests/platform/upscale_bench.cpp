// Superresolución AMD (solo con LANCAST_BENCH=1): ¿conviene transmitir a 1080p y agrandar a 1440p en el cliente?
// Mide el tiempo de agrandar cada cuadro (con pausas de 16 ms, como un stream real) y la calidad (PSNR de la luma)
// de una imagen de escritorio con texto: 1440p -> 1080p -> 1440p comparada con la original, contra el escalado
// actual del cliente (procesador de video D3D11) y contra transmitir a 1440p nativo.

#include <gtest/gtest.h>

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "lancast/platform/d3d.hpp"
#include "lancast/platform/upscaler.hpp"
#include "lancast/platform/video_processor.hpp"

#pragma comment(lib, "winmm.lib")

using namespace lancast;

namespace {

bool bench_enabled() { return GetEnvironmentVariableW(L"LANCAST_BENCH", nullptr, 0) > 0; }

// Escritorio sintético: fondo claro, "texto" negro de 1-2 px en renglones, íconos de colores y un degradado.
std::vector<std::uint32_t> desktop_image(Size s) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(s.width) * s.height, 0xFFF3F3F3);
    std::mt19937 rng(11);
    for (std::uint32_t y = 0; y < s.height; ++y) {
        for (std::uint32_t x = 0; x < s.width; ++x) {
            auto& p = px[static_cast<std::size_t>(y) * s.width + x];
            if (y > s.height * 3 / 4) {  // Degradado (foto/video).
                const auto r = static_cast<std::uint32_t>(x * 255 / s.width);
                const auto g = static_cast<std::uint32_t>((y - s.height * 3 / 4) * 255 / (s.height / 4 + 1));
                p = 0xFF000000 | (r << 16) | (g << 8) | 0x80;
            }
        }
    }
    // Renglones de "texto": trazos verticales y horizontales finos (como letras de 9-11 px).
    for (std::uint32_t line = 20; line + 14 < s.height * 3 / 4; line += 18) {
        std::uint32_t x = 30 + rng() % 40;
        while (x + 10 < s.width - 30) {
            const std::uint32_t w = 5 + rng() % 4;
            for (int stroke = 0; stroke < 4; ++stroke) {
                const std::uint32_t sx = x + rng() % w;
                const std::uint32_t sy = line + rng() % 6;
                const bool vertical = rng() & 1;
                const std::uint32_t len = 4 + rng() % 6;
                for (std::uint32_t k = 0; k < len; ++k) {
                    const std::uint32_t xx = vertical ? sx : sx + k;
                    const std::uint32_t yy = vertical ? sy + k : sy;
                    if (xx < s.width && yy < s.height) px[static_cast<std::size_t>(yy) * s.width + xx] = 0xFF101010;
                }
            }
            x += w + 1 + ((rng() % 7 == 0) ? 6 : 0);
        }
    }
    // Íconos.
    for (int i = 0; i < 12; ++i) {
        const std::uint32_t c = 0xFF000000 | (rng() & 0xFFFFFF);
        const std::uint32_t ox = 40 + i * (s.width - 80) / 12;
        const std::uint32_t oy = s.height * 3 / 4 - 70;
        for (std::uint32_t y = 0; y < 48; ++y) {
            for (std::uint32_t x = 0; x < 48; ++x) px[static_cast<std::size_t>(oy + y) * s.width + ox + x] = c;
        }
    }
    return px;
}

gpu::ComPtr<ID3D11Texture2D> upload_bgra(const gpu::Device& dev, Size s, const std::vector<std::uint32_t>& px) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = s.width;
    d.Height = s.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init{px.data(), s.width * 4, 0};
    gpu::ComPtr<ID3D11Texture2D> t;
    dev.device->CreateTexture2D(&d, &init, &t);
    return t;
}

std::vector<float> luma_of(const gpu::Device& dev, ID3D11Texture2D* bgra, Size s) {
    D3D11_TEXTURE2D_DESC d{};
    bgra->GetDesc(&d);
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    gpu::ComPtr<ID3D11Texture2D> staging;
    dev.device->CreateTexture2D(&d, nullptr, &staging);
    dev.context->CopyResource(staging.Get(), bgra);
    D3D11_MAPPED_SUBRESOURCE m{};
    std::vector<float> y(static_cast<std::size_t>(s.width) * s.height);
    if (FAILED(dev.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return y;
    for (std::uint32_t r = 0; r < s.height; ++r) {
        const auto* row = reinterpret_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(r) * m.RowPitch;
        for (std::uint32_t c = 0; c < s.width; ++c) {
            const std::uint8_t* p = row + c * 4;
            y[static_cast<std::size_t>(r) * s.width + c] = 0.0722f * p[0] + 0.7152f * p[1] + 0.2126f * p[2];
        }
    }
    dev.context->Unmap(staging.Get(), 0);
    return y;
}

double psnr(const std::vector<float>& a, const std::vector<float>& b) {
    double mse = 0;
    for (std::size_t i = 0; i < a.size(); ++i) mse += (a[i] - b[i]) * (a[i] - b[i]);
    mse /= static_cast<double>(a.size());
    return mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99;
}

void wait_gpu(const gpu::Device& dev) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    gpu::ComPtr<ID3D11Query> q;
    dev.device->CreateQuery(&qd, &q);
    dev.context->End(q.Get());
    dev.context->Flush();
    BOOL done = FALSE;
    while (dev.context->GetData(q.Get(), &done, sizeof(done), 0) != S_OK || !done) YieldProcessor();
}

double median(std::vector<double> v) {
    std::ranges::sort(v);
    return v.empty() ? 0 : v[v.size() / 2];
}
double p95(std::vector<double> v) {
    std::ranges::sort(v);
    return v.empty() ? 0 : v[std::min(v.size() - 1, v.size() * 95 / 100)];
}

}  // namespace

TEST(Bench, AmdSuperResolution) {
    if (!bench_enabled()) GTEST_SKIP() << "LANCAST_BENCH=1";
    timeBeginPeriod(1);
    auto dev = gpu::create_device(std::nullopt);
    ASSERT_TRUE(dev.has_value());
    const Size big{2560, 1440};
    const Size low_size{1920, 1080};

    const auto original_px = desktop_image(big);
    auto original = upload_bgra(*dev, big, original_px);
    std::vector<float> ref(original_px.size());
    for (std::size_t i = 0; i < ref.size(); ++i) {
        const std::uint32_t p = original_px[i];
        ref[i] = 0.0722f * (p & 0xFF) + 0.7152f * ((p >> 8) & 0xFF) + 0.2126f * ((p >> 16) & 0xFF);
    }

    auto nv12 = [&](Size s) {
        return *gpu::create_texture(dev->device.Get(), s.width, s.height, DXGI_FORMAT_NV12,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    };
    auto bgra = [&](Size s) {
        return *gpu::create_texture(dev->device.Get(), s.width, s.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    };
    // Lo que llegaría al cliente: 1080p (o 1440p nativo) en NV12, como sale del decodificador.
    auto low = nv12(low_size);
    auto native = nv12(big);
    (*gpu::VideoProcessor::create(*dev, big, low_size, gpu::ColorConversion::RgbToYuv709))->process(original.Get(), 0, low.Get(), nullptr, nullptr);
    (*gpu::VideoProcessor::create(*dev, big, big, gpu::ColorConversion::RgbToYuv709))->process(original.Get(), 0, native.Get(), nullptr, nullptr);

    auto to_rgb_big = *gpu::VideoProcessor::create(*dev, big, big, gpu::ColorConversion::YuvToRgb709);
    auto out_rgb = bgra(big);
    to_rgb_big->process(native.Get(), 0, out_rgb.Get(), nullptr, nullptr);
    const double psnr_native = psnr(ref, luma_of(*dev, out_rgb.Get(), big));

    // Escalado actual del cliente: el procesador de video agranda al convertir a RGB.
    auto vp_up = *gpu::VideoProcessor::create(*dev, low_size, big, gpu::ColorConversion::YuvToRgb709);
    std::vector<double> vp_ms;
    for (int i = 0; i < 60; ++i) {
        const std::uint64_t t0 = gpu::now_us();
        vp_up->process(low.Get(), 0, out_rgb.Get(), nullptr, nullptr);
        wait_gpu(*dev);
        vp_ms.push_back((gpu::now_us() - t0) / 1000.0);
        Sleep(16);
    }
    const double psnr_vp = psnr(ref, luma_of(*dev, out_rgb.Get(), big));

    std::printf("\n  Agrandar 1920x1080 -> 2560x1440 (cada 16 ms)        | tiempo med / p95 | calidad (PSNR luma)\n");
    std::printf("  1440p nativo (sin agrandar)                         |        -         | %5.2f dB\n", psnr_native);
    std::printf("  Escalado actual del cliente (procesador de video)   | %5.2f / %5.2f ms | %5.2f dB\n", median(vp_ms), p95(vp_ms), psnr_vp);

    for (const auto alg : {gpu::UpscaleAlgorithm::Bicubic, gpu::UpscaleAlgorithm::VideoSr10, gpu::UpscaleAlgorithm::VideoSr11}) {
        auto up = gpu::create_amd_upscaler(*dev, low_size, big, alg);
        if (!up) {
            std::printf("  %-50s | no disponible: %s\n", gpu::to_string(alg), up.error().message().c_str());
            continue;
        }
        std::vector<double> ms;
        gpu::ComPtr<ID3D11Texture2D> result;
        bool failed = false;
        for (int i = 0; i < 60; ++i) {
            const std::uint64_t t0 = gpu::now_us();
            auto r = (*up)->process(low.Get());
            if (!r) {
                std::printf("  %-50s | error: %s\n", gpu::to_string(alg), r.error().message().c_str());
                failed = true;
                break;
            }
            ms.push_back((gpu::now_us() - t0) / 1000.0);
            result = *r;
            Sleep(16);
        }
        if (failed) continue;
        // La salida (NV12 a 1440p) se pasa a RGB sin escalar para compararla.
        double q = 0;
        if ((*up)->output_format() == DXGI_FORMAT_NV12) {
            to_rgb_big->process(result.Get(), 0, out_rgb.Get(), nullptr, nullptr);
            q = psnr(ref, luma_of(*dev, out_rgb.Get(), big));
        } else {
            q = psnr(ref, luma_of(*dev, result.Get(), big));
        }
        std::printf("  %-50s | %5.2f / %5.2f ms | %5.2f dB (salida formato %d)\n", gpu::to_string(alg), median(ms), p95(ms), q,
                    static_cast<int>((*up)->output_format()));
    }
    // 2x exacto (720p -> 1440p): algunos algoritmos solo admiten ciertas escalas.
    const Size s720{1280, 720};
    auto low720 = nv12(s720);
    (*gpu::VideoProcessor::create(*dev, big, s720, gpu::ColorConversion::RgbToYuv709))->process(original.Get(), 0, low720.Get(), nullptr, nullptr);
    auto vp720 = *gpu::VideoProcessor::create(*dev, s720, big, gpu::ColorConversion::YuvToRgb709);
    vp720->process(low720.Get(), 0, out_rgb.Get(), nullptr, nullptr);
    std::printf("  720p -> 1440p, escalado actual del cliente          |        -         | %5.2f dB\n", psnr(ref, luma_of(*dev, out_rgb.Get(), big)));
    for (const auto alg : {gpu::UpscaleAlgorithm::VideoSr10, gpu::UpscaleAlgorithm::VideoSr11}) {
        auto up = gpu::create_amd_upscaler(*dev, s720, big, alg);
        if (!up) continue;
        std::vector<double> ms;
        gpu::ComPtr<ID3D11Texture2D> result;
        for (int i = 0; i < 60; ++i) {
            const std::uint64_t t0 = gpu::now_us();
            auto r = (*up)->process(low720.Get());
            if (!r) break;
            ms.push_back((gpu::now_us() - t0) / 1000.0);
            result = *r;
            Sleep(16);
        }
        if (!result) continue;
        to_rgb_big->process(result.Get(), 0, out_rgb.Get(), nullptr, nullptr);
        std::printf("  720p -> 1440p, %-36s | %5.2f / %5.2f ms | %5.2f dB\n", gpu::to_string(alg), median(ms), p95(ms),
                    psnr(ref, luma_of(*dev, out_rgb.Get(), big)));
    }
    timeEndPeriod(1);
}
