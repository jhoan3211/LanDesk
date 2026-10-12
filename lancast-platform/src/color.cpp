#include "lancast/platform/color.hpp"

#include "win.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <string>
#include <vector>

#include "lancast/core/log.hpp"

#pragma comment(lib, "d3dcompiler.lib")

namespace lancast::gpu {
namespace {

// --- HLSL ---------------------------------------------------------------------------------------------------
// Triángulo que cubre todo el viewport (sin búfer de vértices).
constexpr char kCommon[] = R"(
struct VSOut { float4 pos : SV_Position; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
#if HDR
static const float KR = 0.2627, KB = 0.0593;  // BT.2020 (no constante)
static const float3x3 M709to2020 = { 0.6274040, 0.3292820, 0.0433136,
                                     0.0690970, 0.9195400, 0.0113612,
                                     0.0163916, 0.0880132, 0.8955950 };
static const float3x3 M2020to709 = {  1.6604910, -0.5876411, -0.0728499,
                                     -0.1245505,  1.1328999, -0.0083494,
                                     -0.0181508, -0.1005789,  1.1187297 };
static const float PQ_M1 = 0.1593017578125, PQ_M2 = 78.84375;
static const float PQ_C1 = 0.8359375, PQ_C2 = 18.8515625, PQ_C3 = 18.6875;
float3 pq_encode(float3 nits) {  // ST 2084: luz absoluta -> señal
    float3 lm = pow(saturate(nits / 10000.0), PQ_M1);
    return pow((PQ_C1 + PQ_C2 * lm) / (1.0 + PQ_C3 * lm), PQ_M2);
}
float3 pq_decode(float3 v) {
    float3 vp = pow(saturate(v), 1.0 / PQ_M2);
    return 10000.0 * pow(max(vp - PQ_C1, 0) / (PQ_C2 - PQ_C3 * vp), 1.0 / PQ_M1);
}
#else
static const float KR = 0.2126, KB = 0.0722;  // BT.709
#endif
static const float KG = 1.0 - KR - KB;
#if TEN_BIT  // P010: 10 bits en la parte alta de 16
float encY(float y) { return (64.0 + 876.0 * saturate(y)) * 64.0 / 65535.0; }
float encC(float c) { return (512.0 + 896.0 * clamp(c, -0.5, 0.5)) * 64.0 / 65535.0; }
float decY(float v) { return (v * 65535.0 / 64.0 - 64.0) / 876.0; }
float decC(float v) { return (v * 65535.0 / 64.0 - 512.0) / 896.0; }
#else  // Rango limitado de 8 bits (igual que el procesador de video)
float encY(float y) { return (16.0 + 219.0 * saturate(y)) / 255.0; }
float encC(float c) { return (128.0 + 224.0 * clamp(c, -0.5, 0.5)) / 255.0; }
float decY(float v) { return (v * 255.0 - 16.0) / 219.0; }
float decC(float v) { return (v * 255.0 - 128.0) / 224.0; }
#endif
)";

constexpr char kPack[] = R"(
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);
cbuffer C : register(b0) { uint W; uint H; uint2 pad; };

float3 rgb_at(uint x, uint y) {
    float3 c = src.SampleLevel(lin, (float2(x, y) + 0.5) / float2(W, H), 0).rgb;
#if HDR
    c = pq_encode(max(mul(M709to2020, c), 0) * 80.0);  // scRGB (1.0 = 80 nits) -> BT.2020 PQ
#endif
    return saturate(c);
}
float3 yuv(float3 c) {
    float y = KR * c.r + KG * c.g + KB * c.b;
    return float3(y, (c.b - y) / (2.0 * (1.0 - KB)), (c.r - y) / (2.0 * (1.0 - KR)));
}
float ps_y(VSOut i) : SV_Target {
    uint2 p = uint2(i.pos.xy);
#if PACKED
    if (p.y >= H) {  // Vista auxiliar: color de las columnas impares (U a la izquierda, V a la derecha).
        uint y = p.y - H, hw = W / 2;
        if (p.x < hw) return encC(yuv(rgb_at(2 * p.x + 1, y)).y);
        return encC(yuv(rgb_at(2 * (p.x - hw) + 1, y)).z);
    }
#endif
    return encY(yuv(rgb_at(p.x, p.y)).x);
}
float2 ps_uv(VSOut i) : SV_Target {
    uint2 p = uint2(i.pos.xy);
#if PACKED
    // Arriba: muestra par/par (no promedio: así el cliente recupera cada píxel exacto). Abajo: filas impares.
    uint y = p.y < H / 2 ? 2 * p.y : 2 * (p.y - H / 2) + 1;
    float3 v = yuv(rgb_at(2 * p.x, y));
#else
    float3 v = 0.25 * (yuv(rgb_at(2 * p.x, 2 * p.y)) + yuv(rgb_at(2 * p.x + 1, 2 * p.y)) +
                       yuv(rgb_at(2 * p.x, 2 * p.y + 1)) + yuv(rgb_at(2 * p.x + 1, 2 * p.y + 1)));
#endif
    return float2(encC(v.y), encC(v.z));
}
)";

constexpr char kUnpack[] = R"(
Texture2D<float> ty : register(t0);
Texture2D<float2> tuv : register(t1);
SamplerState lin : register(s0);
cbuffer C : register(b0) { uint W; uint H; uint2 pad; };

float3 yuv_at(uint2 p) {
    float y = decY(ty.Load(int3(p, 0)));
    float2 c;
#if PACKED
    if ((p.x & 1) != 0) {
        c = float2(ty.Load(int3(p.x / 2, H + p.y, 0)), ty.Load(int3(W / 2 + p.x / 2, H + p.y, 0)));
    } else if ((p.y & 1) != 0) {
        c = tuv.Load(int3(p.x / 2, H / 2 + p.y / 2, 0));
    } else {
        c = tuv.Load(int3(p.x / 2, p.y / 2, 0));
    }
#else
    // 4:2:0: color interpolado (el plano de color cubre la misma área a la mitad de resolución).
    c = tuv.SampleLevel(lin, (float2(p) + 0.5) / float2(W, H), 0);
#endif
    return float3(y, decC(c.x), decC(c.y));
}
float4 ps_unpack(VSOut i) : SV_Target {
    float3 v = yuv_at(uint2(i.pos.xy));
    float r = v.x + 2.0 * (1.0 - KR) * v.z;
    float b = v.x + 2.0 * (1.0 - KB) * v.y;
    float3 c = float3(r, (v.x - KR * r - KB * b) / KG, b);
#if HDR
    return float4(mul(M2020to709, pq_decode(c)) / 80.0, 1);  // scRGB lineal
#else
    return float4(saturate(c), 1);
#endif
}
)";

constexpr char kPresent[] = R"(
Texture2D<float4> img : register(t0);
SamplerState lin : register(s0);
cbuffer C : register(b0) { float4 rect; float sdr_white; float peak; uint mode; float pad; };

float3 linear_to_srgb(float3 x) {
    return x <= 0.0031308 ? 12.92 * x : 1.055 * pow(abs(x), 1.0 / 2.4) - 0.055;
}
float4 ps_present(VSOut i) : SV_Target {
    float4 c = img.SampleLevel(lin, (i.pos.xy - rect.xy) / rect.zw, 0);
    if (mode == 1) {  // HDR -> pantalla SDR: Reinhard extendido (blanco SDR = 1, pico del contenido = blanco total)
        float3 x = max(c.rgb, 0) * 80.0 / sdr_white;
        float w = max(peak / sdr_white, 1.0);
        x = x * (1.0 + x / (w * w)) / (1.0 + x);
        c.rgb = linear_to_srgb(saturate(x));
    }
    return float4(c.rgb, 1);
}
)";

constexpr char kUi[] = R"(
Texture2D<float4> ui : register(t0);
SamplerState pt : register(s0);
cbuffer C : register(b0) { float2 size; float scale; float pad; };
float3 srgb_to_linear(float3 x) { return x <= 0.04045 ? x / 12.92 : pow((abs(x) + 0.055) / 1.055, 2.4); }
float4 ps_ui(VSOut i) : SV_Target {
    float4 c = ui.SampleLevel(pt, i.pos.xy / size, 0);  // Premultiplicado (ImGui sobre fondo transparente)
    if (c.a <= 0.0) return float4(0, 0, 0, 0);
    return float4(srgb_to_linear(saturate(c.rgb / c.a)) * c.a * scale, c.a);
}
)";

struct Defines {
    bool hdr = false;
    bool ten_bit = false;
    bool packed = false;
};

platform::Result<ComPtr<ID3DBlob>> compile(const std::string& source, const char* entry, const char* target, Defines d) {
    const D3D_SHADER_MACRO macros[] = {{"HDR", d.hdr ? "1" : "0"},
                                       {"TEN_BIT", d.ten_bit ? "1" : "0"},
                                       {"PACKED", d.packed ? "1" : "0"},
                                       {nullptr, nullptr}};
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "lancast", macros, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::string msg = "Shader ";
        msg += entry;
        if (errors) msg += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        log::error("{}", msg);
        return platform::detail::hr_error(msg, hr);
    }
    return code;
}

platform::Result<ComPtr<ID3D11VertexShader>> make_vs(ID3D11Device* dev, Defines d) {
    auto code = compile(kCommon, "vs_main", "vs_5_0", d);
    if (!code) return std::unexpected(code.error());
    ComPtr<ID3D11VertexShader> vs;
    const HRESULT hr = dev->CreateVertexShader((*code)->GetBufferPointer(), (*code)->GetBufferSize(), nullptr, &vs);
    if (FAILED(hr)) return platform::detail::hr_error("CreateVertexShader", hr);
    return vs;
}

platform::Result<ComPtr<ID3D11PixelShader>> make_ps(ID3D11Device* dev, const char* body, const char* entry, Defines d) {
    auto code = compile(std::string(kCommon) + body, entry, "ps_5_0", d);
    if (!code) return std::unexpected(code.error());
    ComPtr<ID3D11PixelShader> ps;
    const HRESULT hr = dev->CreatePixelShader((*code)->GetBufferPointer(), (*code)->GetBufferSize(), nullptr, &ps);
    if (FAILED(hr)) return platform::detail::hr_error("CreatePixelShader", hr);
    return ps;
}

platform::Result<ComPtr<ID3D11Buffer>> make_constants(ID3D11Device* dev, UINT bytes) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = (bytes + 15) & ~15u;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> b;
    const HRESULT hr = dev->CreateBuffer(&d, nullptr, &b);
    if (FAILED(hr)) return platform::detail::hr_error("CreateBuffer", hr);
    return b;
}

platform::Result<ComPtr<ID3D11SamplerState>> make_sampler(ID3D11Device* dev, bool linear) {
    D3D11_SAMPLER_DESC d{};
    d.Filter = linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> s;
    const HRESULT hr = dev->CreateSamplerState(&d, &s);
    if (FAILED(hr)) return platform::detail::hr_error("CreateSamplerState", hr);
    return s;
}

// Vista de un plano de una textura NV12/P010: R8/R16 = luma, R8G8/R16G16 = color.
platform::Result<ComPtr<ID3D11ShaderResourceView>> plane_srv(ID3D11Device* dev, ID3D11Texture2D* tex, DXGI_FORMAT format) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> v;
    const HRESULT hr = dev->CreateShaderResourceView(tex, &d, &v);
    if (FAILED(hr)) return platform::detail::hr_error("Vista de plano (SRV)", hr);
    return v;
}

platform::Result<ComPtr<ID3D11RenderTargetView>> plane_rtv(ID3D11Device* dev, ID3D11Texture2D* tex, DXGI_FORMAT format) {
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> v;
    const HRESULT hr = dev->CreateRenderTargetView(tex, &d, &v);
    if (FAILED(hr)) return platform::detail::hr_error("Vista de plano (RTV)", hr);
    return v;
}

void draw_fullscreen(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv,
                     const D3D11_VIEWPORT& vp) {
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->Draw(3, 0);
}

void unbind(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

D3D11_VIEWPORT viewport(float x, float y, float w, float h) { return D3D11_VIEWPORT{x, y, w, h, 0.0f, 1.0f}; }

struct alignas(16) SizeConstants {
    std::uint32_t w;
    std::uint32_t h;
    std::uint32_t pad[2];
};

}  // namespace

// ------------------------------------------------------------------------------------------------ YuvPacker

platform::Result<std::unique_ptr<YuvPacker>> YuvPacker::create(const Device& device, const PackConfig& config) {
    if (config.output.width % 2 != 0 || config.output.height % 2 != 0) {
        return std::unexpected(platform::Error{"El tamaño del stream debe ser par", 0});
    }
    std::unique_ptr<YuvPacker> p{new YuvPacker()};
    p->device_ = device;
    p->config_ = config;
    const Defines d{config.hdr, config.hdr, config.chroma_444};
    ID3D11Device* dev = device.device.Get();
    auto vs = make_vs(dev, d);
    if (!vs) return std::unexpected(vs.error());
    auto psy = make_ps(dev, kPack, "ps_y", d);
    if (!psy) return std::unexpected(psy.error());
    auto psuv = make_ps(dev, kPack, "ps_uv", d);
    if (!psuv) return std::unexpected(psuv.error());
    auto cb = make_constants(dev, sizeof(SizeConstants));
    if (!cb) return std::unexpected(cb.error());
    auto smp = make_sampler(dev, true);
    if (!smp) return std::unexpected(smp.error());
    p->vs_ = *vs;
    p->ps_y_ = *psy;
    p->ps_uv_ = *psuv;
    p->constants_ = *cb;
    p->sampler_ = *smp;
    const SizeConstants c{config.output.width, config.output.height, {}};
    device.context->UpdateSubresource(p->constants_.Get(), 0, nullptr, &c, 0, 0);
    log::info("Conversión por shader: {}x{} -> {}x{} {}{}", config.input.width, config.input.height, config.output.width,
              config.output.height, config.hdr ? "P010 BT.2020 PQ" : "NV12 BT.709", config.chroma_444 ? ", 4:4:4 empaquetado" : "");
    return p;
}

bool YuvPacker::process(ID3D11Texture2D* input, ID3D11Texture2D* output) {
    ID3D11Device* dev = device_.device.Get();
    ID3D11DeviceContext* ctx = device_.context.Get();
    auto& srv = inputs_[input];
    if (!srv) {
        if (FAILED(dev->CreateShaderResourceView(input, nullptr, &srv))) {
            inputs_.erase(input);
            return false;
        }
    }
    auto& rtvs = outputs_[output];
    if (!rtvs.first) {
        auto y = plane_rtv(dev, output, config_.hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM);
        auto uv = plane_rtv(dev, output, config_.hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM);
        if (!y || !uv) {
            outputs_.erase(output);
            return false;
        }
        rtvs = {*y, *uv};
    }
    const Size coded = coded_size();
    ID3D11ShaderResourceView* views[1] = {srv.Get()};
    ID3D11SamplerState* samplers[1] = {sampler_.Get()};
    ID3D11Buffer* cbs[1] = {constants_.Get()};
    ctx->PSSetShaderResources(0, 1, views);
    ctx->PSSetSamplers(0, 1, samplers);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    draw_fullscreen(ctx, vs_.Get(), ps_y_.Get(), rtvs.first.Get(),
                    viewport(0, 0, static_cast<float>(coded.width), static_cast<float>(coded.height)));
    draw_fullscreen(ctx, vs_.Get(), ps_uv_.Get(), rtvs.second.Get(),
                    viewport(0, 0, static_cast<float>(coded.width / 2), static_cast<float>(coded.height / 2)));
    unbind(ctx);
    return true;
}

// ------------------------------------------------------------------------------------------------ YuvRenderer

platform::Result<std::unique_ptr<YuvRenderer>> YuvRenderer::create(const Device& device, Size logical, bool chroma_444,
                                                                   bool hdr) {
    std::unique_ptr<YuvRenderer> r{new YuvRenderer()};
    r->device_ = device;
    r->logical_ = logical;
    r->chroma_444_ = chroma_444;
    r->hdr_ = hdr;
    ID3D11Device* dev = device.device.Get();
    const Defines d{hdr, hdr, chroma_444};
    const Size coded = gpu::coded_size(logical, chroma_444);

    auto copy = create_texture(dev, coded.width, coded.height, hdr ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12, D3D11_BIND_SHADER_RESOURCE);
    if (!copy) return std::unexpected(copy.error());
    r->copy_ = *copy;
    auto y = plane_srv(dev, r->copy_.Get(), hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM);
    auto uv = plane_srv(dev, r->copy_.Get(), hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM);
    if (!y) return std::unexpected(y.error());
    if (!uv) return std::unexpected(uv.error());
    r->copy_y_ = *y;
    r->copy_uv_ = *uv;

    auto image = create_texture(dev, logical.width, logical.height,
                                hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!image) return std::unexpected(image.error());
    r->image_ = *image;
    if (FAILED(dev->CreateRenderTargetView(r->image_.Get(), nullptr, &r->image_rtv_)) ||
        FAILED(dev->CreateShaderResourceView(r->image_.Get(), nullptr, &r->image_srv_))) {
        return std::unexpected(platform::Error{"Vistas de la imagen reconstruida", 0});
    }

    auto vs = make_vs(dev, d);
    if (!vs) return std::unexpected(vs.error());
    auto unpack = make_ps(dev, kUnpack, "ps_unpack", d);
    if (!unpack) return std::unexpected(unpack.error());
    auto present = make_ps(dev, kPresent, "ps_present", d);
    if (!present) return std::unexpected(present.error());
    auto cb = make_constants(dev, 32);
    if (!cb) return std::unexpected(cb.error());
    auto smp = make_sampler(dev, true);
    if (!smp) return std::unexpected(smp.error());
    r->vs_ = *vs;
    r->ps_unpack_ = *unpack;
    r->ps_present_ = *present;
    r->constants_ = *cb;
    r->sampler_ = *smp;
    return r;
}

bool YuvRenderer::convert(ID3D11Texture2D* decoded, UINT subresource) {
    ID3D11DeviceContext* ctx = device_.context.Get();
    const Size coded = gpu::coded_size(logical_, chroma_444_);
    // El decodificador entrega una superficie de su pool (a veces sin vistas de shader): se copia a la nuestra.
    const D3D11_BOX box{0, 0, 0, coded.width, coded.height, 1};
    ctx->CopySubresourceRegion(copy_.Get(), 0, 0, 0, 0, decoded, subresource, &box);

    struct alignas(16) {
        std::uint32_t w;
        std::uint32_t h;
        std::uint32_t pad[6];  // Mismo búfer que present() (32 bytes).
    } c{logical_.width, logical_.height, {}};
    ctx->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);
    ID3D11ShaderResourceView* views[2] = {copy_y_.Get(), copy_uv_.Get()};
    ID3D11SamplerState* samplers[1] = {sampler_.Get()};
    ID3D11Buffer* cbs[1] = {constants_.Get()};
    ctx->PSSetShaderResources(0, 2, views);
    ctx->PSSetSamplers(0, 1, samplers);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    draw_fullscreen(ctx, vs_.Get(), ps_unpack_.Get(), image_rtv_.Get(),
                    viewport(0, 0, static_cast<float>(logical_.width), static_cast<float>(logical_.height)));
    unbind(ctx);
    return true;
}

bool YuvRenderer::present(ID3D11RenderTargetView* target, Size target_size, const Rect& view, bool target_hdr,
                          float sdr_white_nits, float peak_nits) {
    ID3D11DeviceContext* ctx = device_.context.Get();
    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(target, black);
    if (view.width <= 0 || view.height <= 0) return true;

    struct alignas(16) {
        float rect[4];
        float sdr_white;
        float peak;
        std::uint32_t mode;
        float pad;
    } c{{static_cast<float>(view.x), static_cast<float>(view.y), static_cast<float>(view.width),
         static_cast<float>(view.height)},
        std::max(sdr_white_nits, 80.0f),
        std::max(peak_nits, 400.0f),
        hdr_ && !target_hdr ? 1u : 0u,
        0};
    ctx->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);
    ID3D11ShaderResourceView* views[1] = {image_srv_.Get()};
    ID3D11SamplerState* samplers[1] = {sampler_.Get()};
    ID3D11Buffer* cbs[1] = {constants_.Get()};
    ctx->PSSetShaderResources(0, 1, views);
    ctx->PSSetSamplers(0, 1, samplers);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    // Recorte al back buffer (el viewport puede salirse si la vista es más grande, p. ej. escala 1:1).
    const float x0 = std::max(0.0f, static_cast<float>(view.x));
    const float y0 = std::max(0.0f, static_cast<float>(view.y));
    const float x1 = std::min(static_cast<float>(target_size.width), static_cast<float>(view.right()));
    const float y1 = std::min(static_cast<float>(target_size.height), static_cast<float>(view.bottom()));
    if (x1 > x0 && y1 > y0) draw_fullscreen(ctx, vs_.Get(), ps_present_.Get(), target, viewport(x0, y0, x1 - x0, y1 - y0));
    unbind(ctx);
    return true;
}

// ------------------------------------------------------------------------------------------------ UiCompositor

platform::Result<std::unique_ptr<UiCompositor>> UiCompositor::create(const Device& device) {
    std::unique_ptr<UiCompositor> u{new UiCompositor()};
    u->device_ = device;
    ID3D11Device* dev = device.device.Get();
    auto vs = make_vs(dev, {});
    if (!vs) return std::unexpected(vs.error());
    auto ps = make_ps(dev, kUi, "ps_ui", {});
    if (!ps) return std::unexpected(ps.error());
    auto cb = make_constants(dev, 16);
    if (!cb) return std::unexpected(cb.error());
    auto smp = make_sampler(dev, false);
    if (!smp) return std::unexpected(smp.error());
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;  // Premultiplicado.
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bd, &u->blend_))) return std::unexpected(platform::Error{"CreateBlendState", 0});
    u->vs_ = *vs;
    u->ps_ = *ps;
    u->constants_ = *cb;
    u->sampler_ = *smp;
    return u;
}

bool UiCompositor::draw(ID3D11ShaderResourceView* ui, ID3D11RenderTargetView* target, Size size, float sdr_white_nits) {
    ID3D11DeviceContext* ctx = device_.context.Get();
    struct alignas(16) {
        float size[2];
        float scale;
        float pad;
    } c{{static_cast<float>(size.width), static_cast<float>(size.height)}, std::max(sdr_white_nits, 80.0f) / 80.0f, 0};
    ctx->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);
    ID3D11SamplerState* samplers[1] = {sampler_.Get()};
    ID3D11Buffer* cbs[1] = {constants_.Get()};
    ctx->PSSetShaderResources(0, 1, &ui);
    ctx->PSSetSamplers(0, 1, samplers);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
    draw_fullscreen(ctx, vs_.Get(), ps_.Get(), target,
                    viewport(0, 0, static_cast<float>(size.width), static_cast<float>(size.height)));
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    unbind(ctx);
    return true;
}

// ------------------------------------------------------------------------------------------------ pantalla

DisplayColor display_color(HMONITOR__* monitor) {
    DisplayColor out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a, adapter.Reset()) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o, output.Reset()) {
            ComPtr<IDXGIOutput6> o6;
            DXGI_OUTPUT_DESC1 d{};
            if (FAILED(output.As(&o6)) || FAILED(o6->GetDesc1(&d)) || d.Monitor != monitor) continue;
            out.hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            if (d.MaxLuminance > 0) out.peak_nits = d.MaxLuminance;
        }
    }
    if (!out.hdr) return out;

    // Brillo del contenido SDR (Configuración → Pantalla → HDR): DisplayConfig, emparejando por nombre GDI.
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return out;
    UINT32 paths = 0;
    UINT32 modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> path_info(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> mode_info(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path_info.data(), &modes, mode_info.data(), nullptr) != ERROR_SUCCESS) {
        return out;
    }
    for (UINT32 i = 0; i < paths; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path_info[i].sourceInfo.adapterId;
        source.header.id = path_info[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || wcscmp(source.viewGdiDeviceName, mi.szDevice) != 0) {
            continue;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path_info[i].targetInfo.adapterId;
        white.header.id = path_info[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0) {
            out.sdr_white_nits = static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f;
        }
        break;
    }
    return out;
}

}  // namespace lancast::gpu
