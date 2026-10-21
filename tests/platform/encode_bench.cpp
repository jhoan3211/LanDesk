// Medición de latencia de la tubería del host (solo con LANCAST_BENCH=1): conversión BGRA -> NV12 en la GPU y
// codificación por hardware, por códec y resolución, con contenido en movimiento y al ritmo de 120 fps.

#include <gtest/gtest.h>

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <array>
#include <format>
#include <cstdio>
#include <map>
#include <mutex>
#include <random>
#include <vector>

#include "lancast/core/bitrate.hpp"
#include "lancast/platform/color.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/video_processor.hpp"

using namespace lancast;

namespace {

bool bench_enabled() { return GetEnvironmentVariableW(L"LANCAST_BENCH", nullptr, 0) > 0; }

// Escritorio sintético más grande que el cuadro: se recorre en diagonal (movimiento real para el codificador).
gpu::ComPtr<ID3D11Texture2D> make_desktop(const gpu::Device& dev, Size size) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(size.width) * size.height);
    std::mt19937 rng(7);
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const std::uint32_t r = (x * 255 / size.width) & 0xFF;
            const std::uint32_t g = (y * 255 / size.height) & 0xFF;
            std::uint32_t b = ((x / 64 + y / 64) % 2) ? 200 : 60;
            // "Texto": bloques finos con detalle alto.
            if ((y % 40) < 14 && (x % 300) < 220) b = (rng() & 1) ? 20 : 235;
            px[static_cast<std::size_t>(y) * size.width + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
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

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::ranges::sort(v);
    return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5))];
}

// Tiempo de GPU de la conversión (consultas de marca de tiempo).
double convert_gpu_ms(const gpu::Device& dev, gpu::VideoProcessor& vp, ID3D11Texture2D* src, ID3D11Texture2D* dst, const RECT& rect) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    gpu::ComPtr<ID3D11Query> disjoint;
    gpu::ComPtr<ID3D11Query> t0;
    gpu::ComPtr<ID3D11Query> t1;
    dev.device->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev.device->CreateQuery(&qd, &t0);
    dev.device->CreateQuery(&qd, &t1);
    dev.context->Begin(disjoint.Get());
    dev.context->End(t0.Get());
    vp.process(src, 0, dst, &rect, nullptr);
    dev.context->End(t1.Get());
    dev.context->End(disjoint.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    while (dev.context->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) Sleep(0);
    UINT64 a = 0;
    UINT64 b = 0;
    while (dev.context->GetData(t0.Get(), &a, sizeof(a), 0) == S_FALSE) Sleep(0);
    while (dev.context->GetData(t1.Get(), &b, sizeof(b), 0) == S_FALSE) Sleep(0);
    if (dj.Disjoint || dj.Frequency == 0) return -1;
    return static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency);
}

// Lo mismo con la conversión por shader (YuvPacker, 4:2:0).
template <typename F>
double gpu_ms(const gpu::Device& dev, F&& work) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    gpu::ComPtr<ID3D11Query> disjoint;
    gpu::ComPtr<ID3D11Query> t0;
    gpu::ComPtr<ID3D11Query> t1;
    dev.device->CreateQuery(&qd, &disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev.device->CreateQuery(&qd, &t0);
    dev.device->CreateQuery(&qd, &t1);
    dev.context->Begin(disjoint.Get());
    dev.context->End(t0.Get());
    work();
    dev.context->End(t1.Get());
    dev.context->End(disjoint.Get());
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    while (dev.context->GetData(disjoint.Get(), &dj, sizeof(dj), 0) == S_FALSE) Sleep(0);
    UINT64 a = 0;
    UINT64 b = 0;
    while (dev.context->GetData(t0.Get(), &a, sizeof(a), 0) == S_FALSE) Sleep(0);
    while (dev.context->GetData(t1.Get(), &b, sizeof(b), 0) == S_FALSE) Sleep(0);
    if (dj.Disjoint || dj.Frequency == 0) return -1;
    return static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency);
}

}  // namespace

TEST(Bench, EncodeLatency) {
    if (!bench_enabled()) GTEST_SKIP() << "Define LANCAST_BENCH=1 para medir";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    mf::MfScope mf;
    timeBeginPeriod(1);
    auto dev = gpu::create_device(std::nullopt);
    ASSERT_TRUE(dev.has_value());
    const Size desktop_size{2880, 1620};
    auto desktop = make_desktop(*dev, desktop_size);
    const auto codecs = mf::available_encoders(dev->luid);

    std::printf("\n  %-6s %-10s %-7s %-9s | conversión GPU | captura->bitstream (mediana / p95) | bytes/cuadro\n", "códec", "tamaño",
                "slices", "backend");
    for (const Size size : {Size{1920, 1080}, Size{2560, 1440}}) {
        auto vp = gpu::VideoProcessor::create(*dev, desktop_size, size, gpu::ColorConversion::RgbToYuv709);
        ASSERT_TRUE(vp.has_value());
        std::array<gpu::ComPtr<ID3D11Texture2D>, 5> pool;
        for (auto& t : pool) {
            t = *gpu::create_texture(dev->device.Get(), size.width, size.height, DXGI_FORMAT_NV12,
                                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        }
        std::vector<double> conv;
        for (int i = 0; i < 40; ++i) {
            const RECT r{i * 4, i * 2, static_cast<LONG>(i * 4 + 2560), static_cast<LONG>(i * 2 + 1440)};
            const double ms = convert_gpu_ms(*dev, **vp, desktop.Get(), pool[0].Get(), r);
            if (ms >= 0) conv.push_back(ms);
        }
        const double conv_ms = percentile(conv, 0.5);
        // Conversión por shader desde una captura del mismo tamaño de escritorio (con escalado si hace falta).
        {
            const Size cap{2560, 1440};
            auto capture = make_desktop(*dev, cap);
            auto vp_same = gpu::VideoProcessor::create(*dev, cap, size, gpu::ColorConversion::RgbToYuv709);
            gpu::PackConfig pc{cap, DXGI_FORMAT_B8G8R8A8_UNORM, size, false, false};
            auto packer = gpu::YuvPacker::create(*dev, pc);
            std::vector<double> a_vp;
            std::vector<double> a_sh;
            for (int i = 0; i < 40; ++i) {
                a_vp.push_back(gpu_ms(*dev, [&] { (*vp_same)->process(capture.Get(), 0, pool[1].Get(), nullptr, nullptr); }));
                a_sh.push_back(gpu_ms(*dev, [&] { (*packer)->process(capture.Get(), pool[2].Get()); }));
            }
            std::printf("  Conversión de una captura 2560x1440 a %ux%u: procesador de video %.2f ms · shader %.2f ms\n", size.width,
                        size.height, percentile(a_vp, 0.5), percentile(a_sh, 0.5));
        }

        for (const Codec codec : codecs) {
            for (const bool slices : {false, true}) {
                wchar_t fps_env[16]{};
                const std::uint32_t fps = GetEnvironmentVariableW(L"LANCAST_BENCH_FPS", fps_env, 16) > 0
                                              ? static_cast<std::uint32_t>(_wtoi(fps_env)) : 120;
                mf::EncoderConfig ec{codec, size, fps, auto_bitrate_mbps(size.width, size.height, fps, codec) * 1000, RateControl::Cbr,
                                     true};
                ec.slices = slices;
                auto enc = mf::create_encoder(*dev, ec);
                if (!enc) {
                    std::printf("  %-6s %ux%u: %s\n", std::string{to_string(codec)}.c_str(), size.width, size.height,
                                enc.error().message().c_str());
                    continue;
                }
                std::mutex m;
                std::map<std::uint64_t, std::uint64_t> sent;
                std::vector<double> latency;
                std::uint64_t bytes = 0;
                int frames = 0;
                (*enc)->set_output_callback([&](mf::EncodedPacket&& p) {
                    const std::uint64_t now = gpu::now_us();
                    std::scoped_lock lock(m);
                    bytes += p.data.size();
                    if (!p.last_slice) return;
                    ++frames;
                    if (const auto it = sent.find(p.timestamp_us); it != sent.end() && p.timestamp_us > 30) {
                        latency.push_back(static_cast<double>(now - it->second) / 1000.0);
                    }
                });
                const std::uint64_t interval = 1'000'000 / fps;
                std::uint64_t next = gpu::now_us();
                for (int i = 0; i < 240; ++i) {
                    while (gpu::now_us() < next) Sleep(0);
                    next += interval;
                    const int k = i % 60;
                    const RECT r{k * 5, k * 3, static_cast<LONG>(k * 5 + 2560), static_cast<LONG>(k * 3 + 1440)};
                    const std::uint64_t t0 = gpu::now_us();
                    const std::uint64_t ts = static_cast<std::uint64_t>(i + 1) * interval;
                    {
                        std::scoped_lock lock(m);
                        sent[ts] = t0;
                    }
                    ID3D11Texture2D* target = pool[static_cast<std::size_t>(i) % pool.size()].Get();
                    (*vp)->process(desktop.Get(), 0, target, &r, nullptr);
                    auto ok = (*enc)->submit(target, ts, i == 0);
                    ASSERT_TRUE(ok.has_value()) << ok.error().message();
                }
                Sleep(200);
                std::scoped_lock lock(m);
                std::printf("  %-6s %-10s %-7s %-9s | %6.2f ms      | %6.2f / %6.2f ms                  | %6.0f KB\n",
                            std::string{to_string(codec)}.c_str(), std::format("{}x{}", size.width, size.height).c_str(),
                            slices ? "sí" : "no", (*enc)->backend() == "AMF directo" ? "AMF" : (*enc)->backend().c_str(), conv_ms,
                            percentile(latency, 0.5), percentile(latency, 0.95),
                            frames > 0 ? static_cast<double>(bytes) / frames / 1024.0 : 0.0);
            }
        }
    }
    timeEndPeriod(1);
}

// Desglose de la latencia del host por etapa (LANCAST_BENCH=1): conversión de color en la GPU y codificación, para
// 4:2:0 y 4:4:4 empaquetado (W×2H) a 1080p y 1440p, desde una captura de 2560x1440 con contenido en movimiento.
TEST(Bench, HostLatencyBreakdown) {
    if (!bench_enabled()) GTEST_SKIP() << "Define LANCAST_BENCH=1 para medir";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    mf::MfScope mf;
    timeBeginPeriod(1);
    auto dev = gpu::create_device(std::nullopt);
    ASSERT_TRUE(dev.has_value());
    const Size cap{2560, 1440};
    // Dos "capturas" distintas alternadas: el codificador siempre ve cambios.
    std::array<gpu::ComPtr<ID3D11Texture2D>, 2> captures{make_desktop(*dev, cap), make_desktop(*dev, Size{cap.width, cap.height})};
    {
        // La segunda, con el contenido corrido (otra semilla de "texto").
        D3D11_BOX box{0, 0, 0, cap.width - 64, cap.height, 1};
        auto big = make_desktop(*dev, Size{cap.width + 64, cap.height});
        dev->context->CopySubresourceRegion(captures[1].Get(), 0, 0, 0, 0, big.Get(), 0, &box);
    }
    std::printf("\n  %-14s %-11s | conversión GPU | codificación (mediana / p95) | KB/cuadro\n", "modo", "codificado");
    struct Mode {
        const char* name;
        Size out;
        bool c444;
        std::uint32_t mbps;
    };
    for (const Mode m : {Mode{"1080p 4:2:0", {1920, 1080}, false, 50}, Mode{"1440p 4:2:0", {2560, 1440}, false, 127},
                         Mode{"1080p 4:4:4", {1920, 1080}, true, 100}, Mode{"1440p 4:4:4", {2560, 1440}, true, 137}}) {
        auto packer = gpu::YuvPacker::create(*dev, gpu::PackConfig{cap, DXGI_FORMAT_B8G8R8A8_UNORM, m.out, m.c444, false});
        ASSERT_TRUE(packer.has_value());
        const Size coded = (*packer)->coded_size();
        std::array<gpu::ComPtr<ID3D11Texture2D>, 5> pool;
        for (auto& t : pool) {
            t = *gpu::create_texture(dev->device.Get(), coded.width, coded.height, DXGI_FORMAT_NV12,
                                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        }
        std::vector<double> conv;
        for (int i = 0; i < 40; ++i) {
            const double ms = gpu_ms(*dev, [&] { (*packer)->process(captures[static_cast<std::size_t>(i) % 2].Get(), pool[0].Get()); });
            if (ms >= 0) conv.push_back(ms);
        }
        mf::EncoderConfig ec{Codec::Hevc, coded, 60, m.mbps * 1000, RateControl::Cbr, true};
        auto enc = mf::create_encoder(*dev, ec);
        ASSERT_TRUE(enc.has_value()) << enc.error().message();
        std::mutex mu;
        std::map<std::uint64_t, std::uint64_t> sent;
        std::vector<double> latency;
        std::uint64_t bytes = 0;
        int frames = 0;
        (*enc)->set_output_callback([&](mf::EncodedPacket&& p) {
            const std::uint64_t now = gpu::now_us();
            std::scoped_lock lock(mu);
            bytes += p.data.size();
            ++frames;
            if (const auto it = sent.find(p.timestamp_us); it != sent.end() && p.timestamp_us > 30) {
                latency.push_back(static_cast<double>(now - it->second) / 1000.0);
            }
        });
        const std::uint64_t interval = 1'000'000 / 60;
        std::uint64_t next = gpu::now_us();
        for (int i = 0; i < 180; ++i) {
            while (gpu::now_us() < next) Sleep(0);
            next += interval;
            ID3D11Texture2D* target = pool[static_cast<std::size_t>(i) % pool.size()].Get();
            (*packer)->process(captures[static_cast<std::size_t>(i) % 2].Get(), target);
            const std::uint64_t ts = static_cast<std::uint64_t>(i + 1) * interval;
            {
                std::scoped_lock lock(mu);
                sent[ts] = gpu::now_us();
            }
            ASSERT_TRUE((*enc)->submit(target, ts, i == 0).has_value());
        }
        Sleep(200);
        std::scoped_lock lock(mu);
        std::printf("  %-14s %-11s | %6.2f ms      | %6.2f / %6.2f ms             | %6.0f\n", m.name,
                    std::format("{}x{}", coded.width, coded.height).c_str(), percentile(conv, 0.5), percentile(latency, 0.5),
                    percentile(latency, 0.95), frames > 0 ? static_cast<double>(bytes) / frames / 1024.0 : 0.0);
    }
    timeEndPeriod(1);
}
