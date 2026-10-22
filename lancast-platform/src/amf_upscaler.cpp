// Superresolución AMD con el componente AMFHQScaler (ver upscaler.hpp).

#include "win.hpp"

#include <format>

#include "components/HQScaler.h"
#include "core/Factory.h"
#include "lancast/platform/upscaler.hpp"

namespace lancast::gpu {

const char* to_string(UpscaleAlgorithm a) noexcept {
    switch (a) {
        case UpscaleAlgorithm::Bilinear: return "Bilineal";
        case UpscaleAlgorithm::Bicubic: return "Bicúbico";
        case UpscaleAlgorithm::VideoSr10: return "AMD VideoSR 1.0";
        case UpscaleAlgorithm::VideoSr11: return "AMD VideoSR 1.1";
    }
    return "?";
}

namespace {

platform::Error amf_error(const char* what, AMF_RESULT r) { return {std::format("AMF {}: código {}", what, static_cast<int>(r)), 0}; }

class AmfUpscaler final : public Upscaler {
public:
    ~AmfUpscaler() override {
        // Todo lo de AMF se suelta antes de descargar su DLL.
        last_ = nullptr;
        if (scaler_) scaler_->Terminate();
        scaler_ = nullptr;
        if (context_) context_->Terminate();
        context_ = nullptr;
        if (dll_ != nullptr) FreeLibrary(dll_);
    }

    platform::Result<void> init(const Device& device, Size in, Size out, UpscaleAlgorithm algorithm) {
        device_ = &device;
        dll_ = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dll_ == nullptr) return std::unexpected(platform::Error{"amfrt64.dll no está instalado (no es una GPU AMD)", 0});
        auto init_fn = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll_, AMF_INIT_FUNCTION_NAME));
        if (init_fn == nullptr) return std::unexpected(platform::Error{"amfrt64.dll no exporta AMFInit", 0});
        amf::AMFFactory* factory = nullptr;
        AMF_RESULT r = init_fn(AMF_FULL_VERSION, &factory);
        if (r != AMF_OK) return std::unexpected(amf_error("AMFInit", r));
        if ((r = factory->CreateContext(&context_)) != AMF_OK) return std::unexpected(amf_error("CreateContext", r));
        if ((r = context_->InitDX11(device.device.Get())) != AMF_OK) return std::unexpected(amf_error("InitDX11", r));
        if ((r = factory->CreateComponent(context_, AMFHQScaler, &scaler_)) != AMF_OK) {
            return std::unexpected(amf_error("HQScaler no disponible en este driver", r));
        }
        amf_int64 alg = AMF_HQ_SCALER_ALGORITHM_BILINEAR;
        switch (algorithm) {
            case UpscaleAlgorithm::Bilinear: alg = AMF_HQ_SCALER_ALGORITHM_BILINEAR; break;
            case UpscaleAlgorithm::Bicubic: alg = AMF_HQ_SCALER_ALGORITHM_BICUBIC; break;
            case UpscaleAlgorithm::VideoSr10: alg = AMF_HQ_SCALER_ALGORITHM_VIDEOSR1_0; break;
            case UpscaleAlgorithm::VideoSr11: alg = AMF_HQ_SCALER_ALGORITHM_VIDEOSR1_1; break;
        }
        scaler_->SetProperty(AMF_HQ_SCALER_ENGINE_TYPE, static_cast<amf_int64>(amf::AMF_MEMORY_DX11));
        scaler_->SetProperty(AMF_HQ_SCALER_ALGORITHM, alg);
        scaler_->SetProperty(AMF_HQ_SCALER_OUTPUT_SIZE, AMFConstructSize(static_cast<amf_int32>(out.width), static_cast<amf_int32>(out.height)));
        scaler_->SetProperty(AMF_HQ_SCALER_KEEP_ASPECT_RATIO, false);
        scaler_->SetProperty(AMF_HQ_SCALER_FROM_SRGB, false);
        if ((r = scaler_->Init(amf::AMF_SURFACE_NV12, static_cast<amf_int32>(in.width), static_cast<amf_int32>(in.height))) != AMF_OK) {
            return std::unexpected(amf_error("HQScaler Init (NV12)", r));
        }
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        device.device->CreateQuery(&qd, &done_);
        return {};
    }

    platform::Result<ComPtr<ID3D11Texture2D>> process(ID3D11Texture2D* input) override {
        amf::AMFSurfacePtr surface;
        AMF_RESULT r = context_->CreateSurfaceFromDX11Native(input, &surface, nullptr);
        if (r != AMF_OK) return std::unexpected(amf_error("CreateSurfaceFromDX11Native", r));
        if ((r = scaler_->SubmitInput(surface)) != AMF_OK) return std::unexpected(amf_error("SubmitInput", r));
        amf::AMFDataPtr data;
        for (int i = 0; i < 200; ++i) {
            r = scaler_->QueryOutput(&data);
            if (r == AMF_OK && data) break;
            if (r != AMF_OK && r != AMF_REPEAT && r != AMF_NEED_MORE_INPUT) return std::unexpected(amf_error("QueryOutput", r));
            Sleep(0);
        }
        if (!data) return std::unexpected(platform::Error{"AMF HQScaler no entregó salida", 0});
        amf::AMFSurfacePtr out(data);
        if (!out || out->GetPlanesCount() == 0) return std::unexpected(platform::Error{"AMF HQScaler: salida sin planos", 0});
        auto* tex = static_cast<ID3D11Texture2D*>(out->GetPlaneAt(0)->GetNative());
        if (tex == nullptr) return std::unexpected(platform::Error{"AMF HQScaler: salida sin textura DX11", 0});
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        format_ = d.Format;
        // Esperar a que la GPU termine (AMF trabaja en el mismo dispositivo).
        device_->context->End(done_.Get());
        device_->context->Flush();
        BOOL finished = FALSE;
        while (device_->context->GetData(done_.Get(), &finished, sizeof(finished), 0) != S_OK || !finished) YieldProcessor();
        last_ = out;  // La textura vive mientras la superficie de AMF.
        return ComPtr<ID3D11Texture2D>(tex);
    }

    [[nodiscard]] DXGI_FORMAT output_format() const override { return format_; }

private:
    const Device* device_ = nullptr;
    HMODULE dll_ = nullptr;
    amf::AMFContextPtr context_;
    amf::AMFComponentPtr scaler_;
    amf::AMFSurfacePtr last_;
    ComPtr<ID3D11Query> done_;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

}  // namespace

platform::Result<std::unique_ptr<Upscaler>> create_amd_upscaler(const Device& device, Size input, Size output,
                                                               UpscaleAlgorithm algorithm) {
    auto u = std::make_unique<AmfUpscaler>();
    if (auto r = u->init(device, input, output, algorithm); !r) return std::unexpected(r.error());
    return u;
}

}  // namespace lancast::gpu
