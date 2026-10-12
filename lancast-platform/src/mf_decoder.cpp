#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>

#include <format>

#include "lancast/core/log.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "win.hpp"

namespace lancast::mf {

GUID video_subtype(Codec codec) noexcept;  // mf_encoder.cpp

namespace {

using gpu::ComPtr;

ComPtr<IMFActivate> find_decoder(Codec codec, std::string* name) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, video_subtype(codec)};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                                 MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                                 &acts, &count);
    ComPtr<IMFActivate> result;
    if (SUCCEEDED(hr) && count > 0) {
        result = acts[0];
        if (name != nullptr) {
            wchar_t* friendly = nullptr;
            UINT32 len = 0;
            if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len))) {
                *name = platform::detail::to_utf8(std::wstring_view{friendly, len});
                CoTaskMemFree(friendly);
            }
        }
    }
    for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
    if (acts) CoTaskMemFree(acts);
    return result;
}

class MfDecoder final : public Decoder {
public:
    ~MfDecoder() override {
        if (transform_) {
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
        if (activate_) activate_->ShutdownObject();
    }

    static platform::Result<std::unique_ptr<MfDecoder>> create(const gpu::Device& device, Codec codec, Size size, bool ten_bit) {
        std::unique_ptr<MfDecoder> d{new MfDecoder()};
        d->codec_ = codec;
        d->size_ = size;
        d->ten_bit_ = ten_bit;
        if (auto r = d->init(device); !r) return std::unexpected(r.error());
        return d;
    }

    platform::Result<std::optional<DecodedFrame>> decode(std::span<const std::uint8_t> data,
                                                         std::uint64_t timestamp_us) override {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(data.size()), &buffer);
        if (FAILED(hr)) return platform::detail::hr_error("MFCreateMemoryBuffer", hr);
        BYTE* dst = nullptr;
        buffer->Lock(&dst, nullptr, nullptr);
        std::memcpy(dst, data.data(), data.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(data.size()));

        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(static_cast<LONGLONG>(timestamp_us) * 10);

        std::optional<DecodedFrame> frame;
        hr = transform_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (auto r = drain(frame); !r) return std::unexpected(r.error());
            hr = transform_->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) return platform::detail::hr_error("Decodificador: ProcessInput", hr);
        if (auto r = drain(frame); !r) return std::unexpected(r.error());
        return frame;
    }

    [[nodiscard]] std::string name() const override { return name_; }

private:
    MfDecoder() = default;

    platform::Result<void> init(const gpu::Device& device) {
        activate_ = find_decoder(codec_, &name_);
        if (!activate_) {
            return std::unexpected(platform::Error{
                std::format("Windows no tiene decodificador {} (instala la extensión de video de Microsoft Store)",
                            to_string(codec_)),
                0});
        }
        HRESULT hr = activate_->ActivateObject(IID_PPV_ARGS(&transform_));
        if (FAILED(hr)) return platform::detail::hr_error("Activar el decodificador", hr);

        ComPtr<IMFAttributes> attrs;
        transform_->GetAttributes(&attrs);
        UINT32 d3d11_aware = 0;
        if (attrs) {
            attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11_aware);
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(transform_.As(&api))) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = 1;
            api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            // Mostrar los cuadros aunque falte una referencia (tras una pérdida): con intra-refresh la imagen
            // se repara sola en pocos cuadros; sin esto el decodificador se congela hasta el próximo keyframe.
            VARIANT keep;
            VariantInit(&keep);
            keep.vt = VT_BOOL;
            keep.boolVal = VARIANT_FALSE;
            api->SetValue(&CODECAPI_AVDecVideoDropPicWithMissingRef, &keep);
        }
        if (!d3d11_aware) return std::unexpected(platform::Error{"El decodificador no soporta D3D11", 0});

        UINT token = 0;
        hr = MFCreateDXGIDeviceManager(&token, &device_manager_);
        if (FAILED(hr)) return platform::detail::hr_error("MFCreateDXGIDeviceManager", hr);
        device_manager_->ResetDevice(device.device.Get(), token);
        hr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(device_manager_.Get()));
        if (FAILED(hr)) return platform::detail::hr_error("Decodificador sin aceleración DXVA", hr);

        ComPtr<IMFMediaType> in_type;
        MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, video_subtype(codec_));
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, size_.width, size_.height);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (ten_bit_ && codec_ == Codec::Hevc) in_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_10);
        hr = transform_->SetInputType(0, in_type.Get(), 0);
        if (FAILED(hr)) return platform::detail::hr_error("Decodificador: SetInputType", hr);
        if (auto r = select_output_type(true); !r) return r;

        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        log::info("Decodificador: {} ({} {}x{})", name_, to_string(codec_), size_.width, size_.height);
        return {};
    }

    // initial: antes de ver el primer cuadro algunos decodificadores solo ofrecen NV12 aunque el stream sea de
    // 10 bits; P010 aparece con el cambio de formato (MF_E_TRANSFORM_STREAM_CHANGE) al leer la cabecera.
    platform::Result<void> select_output_type(bool initial = false) {
        if (ten_bit_ && has_output_type(MFVideoFormat_P010)) return set_output_type(MFVideoFormat_P010);
        if (ten_bit_ && !initial) return std::unexpected(platform::Error{"El decodificador no ofrece salida P010 (10 bits)", 0});
        return set_output_type(MFVideoFormat_NV12);
    }

    bool has_output_type(const GUID& wanted) {
        ComPtr<IMFMediaType> type;
        for (DWORD i = 0; SUCCEEDED(transform_->GetOutputAvailableType(0, i, &type)); ++i, type.Reset()) {
            GUID sub{};
            type->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == wanted) return true;
        }
        return false;
    }

    platform::Result<void> set_output_type(const GUID& wanted) {
        ComPtr<IMFMediaType> type;
        for (DWORD i = 0; SUCCEEDED(transform_->GetOutputAvailableType(0, i, &type)); ++i, type.Reset()) {
            GUID sub{};
            type->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == wanted) {
                const HRESULT hr = transform_->SetOutputType(0, type.Get(), 0);
                if (FAILED(hr)) return platform::detail::hr_error("Decodificador: SetOutputType", hr);
                UINT32 w = 0;
                UINT32 h = 0;
                MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
                MFVideoArea area{};
                if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area),
                                            nullptr))) {
                    w = static_cast<UINT32>(area.Area.cx);
                    h = static_cast<UINT32>(area.Area.cy);
                }
                if (w > 0 && h > 0) visible_ = Size{std::min(w, size_.width), std::min(h, size_.height)};
                return {};
            }
        }
        return std::unexpected(platform::Error{wanted == MFVideoFormat_P010 ? "El decodificador no ofrece salida P010 (10 bits)"
                                                                            : "El decodificador no ofrece salida NV12",
                                               0});
    }

    platform::Result<void> drain(std::optional<DecodedFrame>& frame) {
        for (int guard = 0; guard < 8; ++guard) {
            MFT_OUTPUT_DATA_BUFFER out{};
            DWORD status = 0;
            const HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return {};
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (out.pSample) out.pSample->Release();
                if (auto r = select_output_type(); !r) return r;
                continue;
            }
            if (FAILED(hr)) {
                if (out.pSample) out.pSample->Release();
                return platform::detail::hr_error("Decodificador: ProcessOutput", hr);
            }
            ComPtr<IMFSample> sample;
            sample.Attach(out.pSample);
            if (!sample) continue;

            ComPtr<IMFMediaBuffer> buffer;
            ComPtr<IMFDXGIBuffer> dxgi;
            if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgi))) continue;
            DecodedFrame f;
            if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&f.texture)))) continue;
            dxgi->GetSubresourceIndex(&f.subresource);
            f.size = visible_.width > 0 ? visible_ : size_;
            f.keep_alive = sample;
            frame = std::move(f);  // Si hubiera varios, se queda el más reciente (menor latencia).
        }
        return {};
    }

    Codec codec_ = Codec::H264;
    Size size_;
    Size visible_;
    bool ten_bit_ = false;
    std::string name_ = "MFT";
    ComPtr<IMFActivate> activate_;
    ComPtr<IMFTransform> transform_;
    ComPtr<IMFDXGIDeviceManager> device_manager_;
};

}  // namespace

platform::Result<std::unique_ptr<Decoder>> create_decoder(const gpu::Device& device, Codec codec, Size size, bool ten_bit) {
    auto d = MfDecoder::create(device, codec, size, ten_bit);
    if (!d) return std::unexpected(d.error());
    return std::unique_ptr<Decoder>{std::move(*d)};
}

std::vector<Codec> available_decoders() {
    std::vector<Codec> out;
    for (const Codec c : kAllCodecs) {
        if (find_decoder(c, nullptr)) out.push_back(c);
    }
    return out;
}

}  // namespace lancast::mf
