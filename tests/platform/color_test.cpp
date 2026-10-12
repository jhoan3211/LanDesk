// 4:4:4 empaquetado y HDR: empaquetar en el host y reconstruir en el cliente debe devolver la imagen original
// (con y sin pasar por el códec). Un patrón de color que cambia en cada píxel es el peor caso para 4:2:0.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <cstdio>
#include <vector>


#include "lancast/platform/color.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/mf_codec.hpp"

using namespace lancast;

namespace {

constexpr std::uint32_t kW = 256;
constexpr std::uint32_t kH = 128;

// Color por paridad de columna y fila: rojo, verde, azul, amarillo (más un degradé suave).
std::array<std::uint8_t, 4> pattern(std::uint32_t x, std::uint32_t y) {
    const std::uint8_t g = static_cast<std::uint8_t>(40 + (x + y) % 120);
    switch ((x & 1) | ((y & 1) << 1)) {
        case 0: return {g, g, 230, 255};      // BGRA: rojo
        case 1: return {g, 220, g, 255};      // verde
        case 2: return {230, g, g, 255};      // azul
        default: return {g, 220, 230, 255};   // amarillo
    }
}

gpu::ComPtr<ID3D11Texture2D> make_source(const gpu::Device& dev) {
    std::vector<std::uint8_t> px(kW * kH * 4);
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            const auto c = pattern(x, y);
            std::copy(c.begin(), c.end(), px.begin() + (y * kW + x) * 4);
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = kW;
    d.Height = kH;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init{px.data(), kW * 4, 0};
    gpu::ComPtr<ID3D11Texture2D> t;
    EXPECT_TRUE(SUCCEEDED(dev.device->CreateTexture2D(&d, &init, &t)));
    return t;
}

// Lee la imagen reconstruida (RGBA8) y devuelve el error medio y máximo por canal contra el patrón.
std::pair<double, int> compare_rgba8(const gpu::Device& dev, ID3D11Texture2D* image) {
    D3D11_TEXTURE2D_DESC d{};
    image->GetDesc(&d);
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    gpu::ComPtr<ID3D11Texture2D> staging;
    EXPECT_TRUE(SUCCEEDED(dev.device->CreateTexture2D(&d, nullptr, &staging)));
    dev.context->CopyResource(staging.Get(), image);
    D3D11_MAPPED_SUBRESOURCE m{};
    EXPECT_TRUE(SUCCEEDED(dev.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)));
    double sum = 0;
    int worst = 0;
    for (std::uint32_t y = 0; y < kH; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(m.pData) + y * m.RowPitch;
        for (std::uint32_t x = 0; x < kW; ++x) {
            const auto want = pattern(x, y);  // BGRA
            const std::uint8_t* got = row + x * 4;  // RGBA
            const int e[3] = {std::abs(got[0] - want[2]), std::abs(got[1] - want[1]), std::abs(got[2] - want[0])};
            for (const int v : e) {
                sum += v;
                worst = std::max(worst, v);
            }
        }
    }
    dev.context->Unmap(staging.Get(), 0);
    return {sum / (kW * kH * 3.0), worst};
}

struct Roundtrip {
    double mean = 0;
    int worst = 0;
};

// Empaqueta, (opcionalmente) codifica y decodifica, y reconstruye.
Roundtrip roundtrip(const gpu::Device& dev, bool chroma_444, std::optional<Codec> codec) {
    auto src = make_source(dev);
    gpu::PackConfig pc;
    pc.input = {kW, kH};
    pc.output = {kW, kH};
    pc.chroma_444 = chroma_444;
    auto packer = gpu::YuvPacker::create(dev, pc);
    EXPECT_TRUE(packer.has_value()) << (packer ? "" : packer.error().message());
    if (!packer) return {};
    const Size coded = (*packer)->coded_size();
    auto packed = gpu::create_texture(dev.device.Get(), coded.width, coded.height, DXGI_FORMAT_NV12,
                                      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    EXPECT_TRUE(packed.has_value());
    EXPECT_TRUE((*packer)->process(src.Get(), packed->Get()));

    auto renderer = gpu::YuvRenderer::create(dev, {kW, kH}, chroma_444, false);
    EXPECT_TRUE(renderer.has_value()) << (renderer ? "" : renderer.error().message());
    if (!renderer) return {};

    if (!codec) {
        (*renderer)->convert(packed->Get(), 0);
    } else {
        mf::EncoderConfig ec{*codec, coded, 30, 80000, RateControl::Cbr, false};
        auto enc = mf::create_encoder(dev, ec);
        EXPECT_TRUE(enc.has_value()) << (enc ? "" : enc.error().message());
        if (!enc) return {};
        auto dec = mf::create_decoder(dev, *codec, coded);
        EXPECT_TRUE(dec.has_value()) << (dec ? "" : dec.error().message());
        if (!dec) return {};
        std::optional<mf::DecodedFrame> frame;
        for (int i = 0; i < 6 && !frame; ++i) {  // Varios cuadros: el decodificador puede retener el primero.
            auto pkt = (*enc)->encode(packed->Get(), 1000 + i * 33333, i == 0);
            if (!pkt || !pkt->has_value()) continue;
            auto out = (*dec)->decode((*pkt)->data, (*pkt)->timestamp_us);
            if (out && out->has_value()) frame = std::move(**out);
        }
        EXPECT_TRUE(frame.has_value()) << "El decodificador no entregó ningún cuadro";
        if (!frame) return {};
        (*renderer)->convert(frame->texture.Get(), frame->subresource);
    }
    const auto [mean, worst] = compare_rgba8(dev, (*renderer)->image());
    return {mean, worst};
}

class ColorPipeline : public ::testing::Test {
protected:
    static void SetUpTestSuite() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    void SetUp() override {
        mf_ = std::make_unique<mf::MfScope>();
        auto d = gpu::create_device(std::nullopt);
        if (!d) GTEST_SKIP() << "Sin GPU: " << d.error().message();
        dev_ = std::move(*d);
    }
    std::unique_ptr<mf::MfScope> mf_;
    gpu::Device dev_;
};

}  // namespace

TEST_F(ColorPipeline, Packed444ReconstructsEveryPixel) {
    const Roundtrip r444 = roundtrip(dev_, true, std::nullopt);
    const Roundtrip r420 = roundtrip(dev_, false, std::nullopt);
    std::printf("  Sin códec: 4:4:4 error medio %.2f (máx %d) · 4:2:0 error medio %.2f (máx %d)\n", r444.mean, r444.worst,
                r420.mean, r420.worst);
    EXPECT_LE(r444.worst, 3) << "4:4:4 debe devolver cada píxel (solo redondeo de 8 bits)";
    EXPECT_GT(r420.mean, 20.0) << "El patrón debería ser imposible para 4:2:0";
}

TEST_F(ColorPipeline, Packed444SurvivesTheCodec) {
    for (const Codec c : {Codec::H264, Codec::Hevc}) {
        const auto enc = mf::available_encoders(dev_.luid);
        const auto dec = mf::available_decoders();
        if (std::ranges::find(enc, c) == enc.end() || std::ranges::find(dec, c) == dec.end()) continue;
        const Roundtrip r444 = roundtrip(dev_, true, c);
        const Roundtrip r420 = roundtrip(dev_, false, c);
        std::printf("  %s: 4:4:4 error medio %.2f · 4:2:0 error medio %.2f\n", std::string{to_string(c)}.c_str(), r444.mean,
                    r420.mean);
        EXPECT_LT(r444.mean, 8.0);
        EXPECT_LT(r444.mean * 2.5, r420.mean) << "4:4:4 debe ser claramente más fiel que 4:2:0";
    }
}

namespace {

// Patrón HDR en scRGB lineal (1.0 = 80 nits): grises y colores de 10 a 1000 nits.
std::array<float, 3> hdr_pattern(std::uint32_t x, std::uint32_t y) {
    const float nits = 10.0f + 990.0f * static_cast<float>(x) / (kW - 1);
    const float v = nits / 80.0f;
    switch ((y / 16) % 4) {
        case 0: return {v, v, v};
        case 1: return {v, v * 0.4f, v * 0.2f};
        case 2: return {v * 0.3f, v, v * 0.5f};
        default: return {v * 0.4f, v * 0.5f, v};
    }
}

std::uint16_t to_half(float f) {
    std::uint32_t b = 0;
    std::memcpy(&b, &f, 4);
    const std::uint32_t sign = (b >> 16) & 0x8000;
    const int exp = static_cast<int>((b >> 23) & 0xFF) - 127 + 15;
    const std::uint32_t mant = b & 0x7FFFFF;
    if (exp <= 0) return static_cast<std::uint16_t>(sign);
    if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7C00);
    return static_cast<std::uint16_t>(sign | (exp << 10) | (mant >> 13));
}

float from_half(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1F;
    const std::uint32_t mant = h & 0x3FF;
    std::uint32_t b = 0;
    if (exp == 0) {
        b = sign;
    } else {
        b = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f = 0;
    std::memcpy(&f, &b, 4);
    return f;
}

// Error relativo medio de la luz (nits) reconstruida.
double hdr_roundtrip(const gpu::Device& dev, bool chroma_444, std::optional<Codec> codec) {
    std::vector<std::uint16_t> px(kW * kH * 4);
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            const auto c = hdr_pattern(x, y);
            for (int k = 0; k < 3; ++k) px[(y * kW + x) * 4 + k] = to_half(c[k]);
            px[(y * kW + x) * 4 + 3] = to_half(1.0f);
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = kW;
    d.Height = kH;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init{px.data(), kW * 8, 0};
    gpu::ComPtr<ID3D11Texture2D> src;
    if (FAILED(dev.device->CreateTexture2D(&d, &init, &src))) return 1e9;

    gpu::PackConfig pc;
    pc.input = {kW, kH};
    pc.input_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pc.output = {kW, kH};
    pc.chroma_444 = chroma_444;
    pc.hdr = true;
    auto packer = gpu::YuvPacker::create(dev, pc);
    EXPECT_TRUE(packer.has_value()) << (packer ? "" : packer.error().message());
    if (!packer) return 1e9;
    const Size coded = (*packer)->coded_size();
    auto packed = gpu::create_texture(dev.device.Get(), coded.width, coded.height, DXGI_FORMAT_P010,
                                      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    EXPECT_TRUE(packed.has_value()) << (packed ? "" : packed.error().message());
    if (!packed) return 1e9;
    (*packer)->process(src.Get(), packed->Get());

    auto renderer = gpu::YuvRenderer::create(dev, {kW, kH}, chroma_444, true);
    EXPECT_TRUE(renderer.has_value()) << (renderer ? "" : renderer.error().message());
    if (!renderer) return 1e9;
    if (!codec) {
        (*renderer)->convert(packed->Get(), 0);
    } else {
        mf::EncoderConfig ec{*codec, coded, 30, 80000, RateControl::Cbr, false};
        ec.ten_bit = true;
        auto enc = mf::create_encoder(dev, ec);
        if (!enc) {
            std::printf("  (sin codificador HDR de 10 bits: %s)\n", enc.error().message().c_str());
            return -1;
        }
        auto dec = mf::create_decoder(dev, *codec, coded, true);
        EXPECT_TRUE(dec.has_value()) << (dec ? "" : dec.error().message());
        if (!dec) return 1e9;
        std::optional<mf::DecodedFrame> frame;
        for (int i = 0; i < 6 && !frame; ++i) {
            auto pkt = (*enc)->encode(packed->Get(), 1000 + i * 33333, i == 0);
            if (!pkt || !pkt->has_value()) continue;
            auto out = (*dec)->decode((*pkt)->data, (*pkt)->timestamp_us);
            if (out && out->has_value()) frame = std::move(**out);
        }
        EXPECT_TRUE(frame.has_value());
        if (!frame) return 1e9;
        (*renderer)->convert(frame->texture.Get(), frame->subresource);
    }

    D3D11_TEXTURE2D_DESC sd{};
    (*renderer)->image()->GetDesc(&sd);
    sd.BindFlags = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    gpu::ComPtr<ID3D11Texture2D> staging;
    dev.device->CreateTexture2D(&sd, nullptr, &staging);
    dev.context->CopyResource(staging.Get(), (*renderer)->image());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(dev.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return 1e9;
    double sum = 0;
    for (std::uint32_t y = 0; y < kH; ++y) {
        const auto* row = reinterpret_cast<const std::uint16_t*>(static_cast<const std::uint8_t*>(m.pData) + y * m.RowPitch);
        for (std::uint32_t x = 0; x < kW; ++x) {
            const auto want = hdr_pattern(x, y);
            const double lum_want = 0.2126 * want[0] + 0.7152 * want[1] + 0.0722 * want[2];
            const double lum_got = 0.2126 * from_half(row[x * 4]) + 0.7152 * from_half(row[x * 4 + 1]) +
                                   0.0722 * from_half(row[x * 4 + 2]);
            sum += std::abs(lum_got - lum_want) / lum_want;
        }
    }
    dev.context->Unmap(staging.Get(), 0);
    return sum / (kW * kH);
}

}  // namespace

TEST_F(ColorPipeline, HdrPqRoundtripKeepsLuminance) {
    const double plain = hdr_roundtrip(dev_, false, std::nullopt);
    const double packed = hdr_roundtrip(dev_, true, std::nullopt);
    std::printf("  HDR sin códec: error de luz %.2f %% (4:2:0) · %.2f %% (4:4:4)\n", plain * 100, packed * 100);
    EXPECT_LT(plain, 0.02);
    EXPECT_LT(packed, 0.01);
    const double hevc = hdr_roundtrip(dev_, false, Codec::Hevc);
    if (hevc >= 0) {
        std::printf("  HDR con HEVC Main10: error de luz %.2f %%\n", hevc * 100);
        EXPECT_LT(hevc, 0.05);
    }
}
