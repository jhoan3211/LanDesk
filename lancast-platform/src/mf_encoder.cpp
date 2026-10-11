#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>

#include <condition_variable>
#include <deque>
#include <format>
#include <functional>
#include <mutex>

#include "lancast/core/log.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "win.hpp"

namespace lancast::mf {

using gpu::ComPtr;

GUID video_subtype(Codec codec) noexcept {
    const auto fourcc = [](char a, char b, char c, char d) {
        return static_cast<DWORD>(static_cast<unsigned char>(a)) | (static_cast<DWORD>(static_cast<unsigned char>(b)) << 8) |
               (static_cast<DWORD>(static_cast<unsigned char>(c)) << 16) |
               (static_cast<DWORD>(static_cast<unsigned char>(d)) << 24);
    };
    DWORD f = 0;
    switch (codec) {
        case Codec::H264:
            f = fourcc('H', '2', '6', '4');
            break;
        case Codec::Hevc:
            f = fourcc('H', 'E', 'V', 'C');
            break;
        case Codec::Av1:
            f = fourcc('A', 'V', '0', '1');
            break;
    }
    return GUID{f, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}

MfScope::MfScope() { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); }
MfScope::~MfScope() { MFShutdown(); }

namespace {

// Recibe los eventos de un MFT asíncrono (BeginGetEvent) y los despacha en el hilo de Media Foundation.
// Así la salida del codificador se recoge en cuanto está lista, en paralelo con la captura del siguiente frame.
class EventPump final : public IMFAsyncCallback {
public:
    using Handler = std::function<void(MediaEventType, HRESULT)>;

    EventPump(IMFMediaEventGenerator* gen, Handler handler) : generator_(gen), handler_(std::move(handler)) {}

    HRESULT start() { return generator_->BeginGetEvent(this, nullptr); }

    // Desconecta el manejador; espera a que termine el que esté en curso.
    void detach() {
        std::scoped_lock lock(handler_mutex_);
        handler_ = nullptr;
        dead_ = true;
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // IMFAsyncCallback
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        ComPtr<IMFMediaEvent> event;
        const HRESULT hr = generator_->EndGetEvent(result, &event);
        std::scoped_lock lock(handler_mutex_);
        if (FAILED(hr) || !event) {
            dead_ = true;
            if (handler_) handler_(MEError, FAILED(hr) ? hr : E_FAIL);
            return S_OK;
        }
        MediaEventType type = 0;
        HRESULT status = S_OK;
        event->GetType(&type);
        event->GetStatus(&status);
        if (handler_) handler_(type, status);
        if (!dead_) generator_->BeginGetEvent(this, nullptr);
        return S_OK;
    }

private:
    ~EventPump() = default;

    LONG refs_ = 1;
    ComPtr<IMFMediaEventGenerator> generator_;
    std::mutex handler_mutex_;
    Handler handler_;
    bool dead_ = false;
};

void set_codec_value(ICodecAPI* api, const GUID& key, ULONG value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    (void)api->SetValue(&key, &v);
}

void set_codec_bool(ICodecAPI* api, const GUID& key, bool value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    (void)api->SetValue(&key, &v);
}

// ¿El bitstream incluye parámetros de secuencia (SPS/VPS)? Si no, hay que anteponerlos a cada IDR.
bool has_parameter_sets(std::span<const std::uint8_t> data, Codec codec) {
    for (std::size_t i = 0; i + 3 < data.size(); ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            const std::uint8_t nal = data[i + 3];
            if (codec == Codec::H264 && (nal & 0x1F) == 7) return true;
            if (codec == Codec::Hevc && ((nal >> 1) & 0x3F) == 32) return true;
            i += 2;
        }
    }
    return codec == Codec::Av1;
}

class MfEncoder final : public Encoder {
public:
    ~MfEncoder() override {
        if (pump_) pump_->detach();
        if (transform_) {
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
        if (activate_) activate_->ShutdownObject();
        if (pump_) pump_->Release();
    }

    static platform::Result<std::unique_ptr<MfEncoder>> create(const gpu::Device& device, const EncoderConfig& cfg) {
        std::unique_ptr<MfEncoder> enc{new MfEncoder()};
        enc->config_ = cfg;
        if (auto r = enc->init(device); !r) return std::unexpected(r.error());
        return enc;
    }

    void set_output_callback(OutputCallback callback) override {
        std::scoped_lock lock(state_mutex_);
        callback_ = std::move(callback);
    }

    platform::Result<void> submit(ID3D11Texture2D* nv12, std::uint64_t timestamp_us, bool force_keyframe) override {
        {
            // Esperar a que el MFT acepte otra entrada (contrapresión natural si la GPU va justa).
            std::unique_lock lock(state_mutex_);
            if (!state_cv_.wait_for(lock, std::chrono::seconds(1), [&] { return need_input_ > 0 || failed_; })) {
                return std::unexpected(platform::Error{"El codificador no responde",
                                                       static_cast<std::int32_t>(MF_E_TRANSFORM_NEED_MORE_INPUT)});
            }
            if (failed_) return platform::detail::hr_error("Error del codificador", error_hr_);
            --need_input_;
        }

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer);
        if (FAILED(hr)) return platform::detail::hr_error("MFCreateDXGISurfaceBuffer", hr);
        ComPtr<IMF2DBuffer> buffer2d;
        DWORD length = 0;
        if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->GetContiguousLength(&length))) {
            buffer->SetCurrentLength(length);
        }
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(static_cast<LONGLONG>(timestamp_us) * 10);
        sample->SetSampleDuration(10'000'000LL / std::max<std::uint32_t>(config_.fps, 1));

        std::scoped_lock io(io_mutex_);
        if (force_keyframe && codec_api_) set_codec_value(codec_api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        hr = transform_->ProcessInput(input_id_, sample.Get(), 0);
        if (FAILED(hr)) return platform::detail::hr_error("ProcessInput", hr);
        return {};
    }

    platform::Result<std::optional<EncodedPacket>> encode(ID3D11Texture2D* nv12, std::uint64_t timestamp_us,
                                                          bool force_keyframe) override {
        if (auto r = submit(nv12, timestamp_us, force_keyframe); !r) return std::unexpected(r.error());
        // Modo síncrono (sin callback): esperar el bitstream (de este frame o de uno anterior si el MFT tiene retardo).
        std::unique_lock lock(state_mutex_);
        state_cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return !output_.empty() || failed_; });
        if (failed_) return platform::detail::hr_error("Error del codificador", error_hr_);
        if (output_.empty()) return std::optional<EncodedPacket>{};
        EncodedPacket p = std::move(output_.front());
        output_.pop_front();
        return std::optional{std::move(p)};
    }

    bool set_bitrate(std::uint32_t kbps) override {
        if (!codec_api_) return false;
        config_.bitrate_kbps = kbps;
        set_codec_value(codec_api_.Get(), CODECAPI_AVEncCommonMeanBitRate, kbps * 1000);
        return true;
    }

    [[nodiscard]] std::string name() const override { return name_; }
    [[nodiscard]] const EncoderConfig& config() const override { return config_; }

private:
    MfEncoder() = default;

    platform::Result<void> init(const gpu::Device& device) {
        // 1) Buscar el MFT de hardware de esta GPU.
        ComPtr<IMFAttributes> enum_attrs;
        MFCreateAttributes(&enum_attrs, 1);
        LUID luid{};
        luid.LowPart = static_cast<DWORD>(device.luid & 0xFFFFFFFFu);
        luid.HighPart = static_cast<LONG>(device.luid >> 32);
        enum_attrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&luid), sizeof(luid));

        MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, video_subtype(config_.codec)};
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                              &out_info, enum_attrs.Get(), &activates, &count);
        if (FAILED(hr) || count == 0) {
            hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                           &out_info, &activates, &count);
        }
        if (FAILED(hr) || count == 0) {
            if (activates) CoTaskMemFree(activates);
            return std::unexpected(platform::Error{
                std::format("No hay codificador {} por hardware en esta GPU", to_string(config_.codec)), 0});
        }
        activate_ = activates[0];
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);

        wchar_t* friendly = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(activate_->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len))) {
            name_ = platform::detail::to_utf8(std::wstring_view{friendly, len});
            CoTaskMemFree(friendly);
        }
        hr = activate_->ActivateObject(IID_PPV_ARGS(&transform_));
        if (FAILED(hr)) return platform::detail::hr_error("Activar el codificador", hr);

        // 2) Desbloquear el modo asíncrono y activar baja latencia.
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(transform_->GetAttributes(&attrs))) {
            UINT32 async = 0;
            attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async);
            async_ = async != 0;
            if (async_) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        if (!async_) return std::unexpected(platform::Error{"El codificador de hardware no es asíncrono", 0});

        // 3) Dispositivo D3D11 compartido (entrada de textura, cero copias).
        UINT token = 0;
        hr = MFCreateDXGIDeviceManager(&token, &device_manager_);
        if (FAILED(hr)) return platform::detail::hr_error("MFCreateDXGIDeviceManager", hr);
        device_manager_->ResetDevice(device.device.Get(), token);
        hr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(device_manager_.Get()));
        if (FAILED(hr)) return platform::detail::hr_error("El codificador no acepta texturas D3D11", hr);

        DWORD in_count = 0;
        DWORD out_count = 0;
        transform_->GetStreamCount(&in_count, &out_count);
        if (FAILED(transform_->GetStreamIDs(1, &input_id_, 1, &output_id_))) {
            input_id_ = 0;
            output_id_ = 0;
        }

        // 4) Parámetros que deben fijarse antes de los tipos.
        if (SUCCEEDED(transform_.As(&codec_api_))) {
            set_codec_value(codec_api_.Get(), CODECAPI_AVEncCommonRateControlMode,
                            config_.rate_control == RateControl::Cbr ? eAVEncCommonRateControlMode_CBR
                                                                     : eAVEncCommonRateControlMode_PeakConstrainedVBR);
            set_codec_value(codec_api_.Get(), CODECAPI_AVEncCommonMeanBitRate, config_.bitrate_kbps * 1000);
            if (config_.rate_control == RateControl::Vbr) {
                set_codec_value(codec_api_.Get(), CODECAPI_AVEncCommonMaxBitRate, config_.bitrate_kbps * 1500);
            }
            set_codec_bool(codec_api_.Get(), CODECAPI_AVLowLatencyMode, true);
            set_codec_value(codec_api_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            // GOP muy largo: los IDR se piden solo cuando hay pérdidas irrecuperables.
            set_codec_value(codec_api_.Get(), CODECAPI_AVEncMPVGOPSize, config_.fps * 30);
            set_codec_value(codec_api_.Get(), CODECAPI_AVEncCommonQualityVsSpeed, config_.low_latency ? 0 : 50);
            // VBV de 1 frame (latencia mínima) o 2: el tamaño de cada frame se mantiene estable y la red no hace cola.
            set_codec_value(codec_api_.Get(), CODECAPI_AVEncCommonBufferSize,
                            config_.bitrate_kbps * 1000 / std::max<std::uint32_t>(config_.fps, 1) * (config_.low_latency ? 1 : 2));
        }

        // 5) Tipo de salida y luego de entrada.
        ComPtr<IMFMediaType> out_type;
        MFCreateMediaType(&out_type);
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE, video_subtype(config_.codec));
        out_type->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate_kbps * 1000);
        MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, config_.size.width, config_.size.height);
        MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, config_.fps, 1);
        MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        out_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        out_type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        out_type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        if (config_.codec == Codec::H264) out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        if (config_.codec == Codec::Hevc) out_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH265VProfile_Main_420_8);
        hr = transform_->SetOutputType(output_id_, out_type.Get(), 0);
        if (FAILED(hr)) {
            return platform::detail::hr_error(
                std::format("El codificador rechazó {}x{} @ {} fps", config_.size.width, config_.size.height, config_.fps), hr);
        }

        ComPtr<IMFMediaType> in_type;
        for (DWORD i = 0; SUCCEEDED(transform_->GetInputAvailableType(input_id_, i, &in_type)); ++i, in_type.Reset()) {
            GUID sub{};
            in_type->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) break;
        }
        if (!in_type) {
            MFCreateMediaType(&in_type);
            in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        }
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, config_.size.width, config_.size.height);
        MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, config_.fps, 1);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        in_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        hr = transform_->SetInputType(input_id_, in_type.Get(), 0);
        if (FAILED(hr)) return platform::detail::hr_error("SetInputType NV12", hr);

        // Cabecera de secuencia (por si el MFT no repite SPS/PPS en cada IDR).
        ComPtr<IMFMediaType> current_out;
        if (SUCCEEDED(transform_->GetOutputCurrentType(output_id_, &current_out))) {
            UINT32 blob_size = 0;
            if (SUCCEEDED(current_out->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob_size)) && blob_size > 0) {
                sequence_header_.resize(blob_size);
                current_out->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequence_header_.data(), blob_size, nullptr);
            }
        }

        MFT_OUTPUT_STREAM_INFO info{};
        transform_->GetOutputStreamInfo(output_id_, &info);
        provides_samples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        output_buffer_size_ = std::max<DWORD>(info.cbSize, config_.size.width * config_.size.height * 3 / 2);

        // 6) Arrancar.
        ComPtr<IMFMediaEventGenerator> gen;
        if (FAILED(transform_.As(&gen))) return std::unexpected(platform::Error{"El MFT no genera eventos", 0});
        pump_ = new EventPump(gen.Get(), [this](MediaEventType type, HRESULT status) { on_event(type, status); });
        pump_->start();
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        log::info("Codificador: {} ({} {}x{} @ {} fps, {} kbps, {})", name_, to_string(config_.codec), config_.size.width,
                  config_.size.height, config_.fps, config_.bitrate_kbps, to_string(config_.rate_control));
        return {};
    }

    // Se ejecuta en un hilo de Media Foundation.
    void on_event(MediaEventType type, HRESULT status) {
        switch (type) {
            case METransformNeedInput: {
                std::scoped_lock lock(state_mutex_);
                ++need_input_;
                break;
            }
            case METransformHaveOutput: {
                std::optional<EncodedPacket> packet;
                platform::Result<void> r;
                {
                    std::scoped_lock io(io_mutex_);
                    r = collect_output(packet);
                }
                OutputCallback cb;
                {
                    std::scoped_lock lock(state_mutex_);
                    if (!r) {
                        failed_ = true;
                        error_hr_ = r.error().hresult;
                    } else if (packet && !callback_) {
                        output_.push_back(std::move(*packet));
                        packet.reset();
                    }
                    cb = callback_;
                }
                if (packet && cb) cb(std::move(*packet));
                break;
            }
            case MEError: {
                std::scoped_lock lock(state_mutex_);
                failed_ = true;
                error_hr_ = status;
                break;
            }
            default:
                break;
        }
        state_cv_.notify_all();
    }

    platform::Result<void> collect_output(std::optional<EncodedPacket>& result) {
        MFT_OUTPUT_DATA_BUFFER out{};
        out.dwStreamID = output_id_;
        ComPtr<IMFSample> own_sample;
        if (!provides_samples_) {
            MFCreateSample(&own_sample);
            ComPtr<IMFMediaBuffer> buf;
            MFCreateMemoryBuffer(output_buffer_size_, &buf);
            own_sample->AddBuffer(buf.Get());
            out.pSample = own_sample.Get();
        }
        DWORD status = 0;
        HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(transform_->GetOutputAvailableType(output_id_, 0, &t))) transform_->SetOutputType(output_id_, t.Get(), 0);
            if (out.pSample && provides_samples_) out.pSample->Release();
            return {};
        }
        if (FAILED(hr)) {
            if (out.pSample && provides_samples_) out.pSample->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return {};
            return platform::detail::hr_error("ProcessOutput", hr);
        }

        ComPtr<IMFSample> sample;
        if (provides_samples_) {
            sample.Attach(out.pSample);
        } else {
            sample = own_sample;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return {};
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &len))) return {};

        EncodedPacket p;
        p.keyframe = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, 0) != 0;
        LONGLONG time = 0;
        if (SUCCEEDED(sample->GetSampleTime(&time))) p.timestamp_us = static_cast<std::uint64_t>(time / 10);
        const std::span<const std::uint8_t> bytes{data, len};
        if (p.keyframe && !sequence_header_.empty() && !has_parameter_sets(bytes, config_.codec)) {
            p.data = sequence_header_;
        }
        p.data.insert(p.data.end(), bytes.begin(), bytes.end());
        buffer->Unlock();
        result = std::move(p);
        return {};
    }

    EncoderConfig config_;
    std::string name_ = "MFT";
    ComPtr<IMFActivate> activate_;
    ComPtr<IMFTransform> transform_;
    ComPtr<ICodecAPI> codec_api_;
    ComPtr<IMFDXGIDeviceManager> device_manager_;
    EventPump* pump_ = nullptr;
    DWORD input_id_ = 0;
    DWORD output_id_ = 0;
    bool async_ = false;
    bool provides_samples_ = true;
    DWORD output_buffer_size_ = 0;
    std::mutex io_mutex_;     // Serializa ProcessInput / ProcessOutput.
    std::mutex state_mutex_;  // need_input_, output_, failed_, callback_.
    std::condition_variable state_cv_;
    int need_input_ = 0;
    bool failed_ = false;
    HRESULT error_hr_ = S_OK;
    OutputCallback callback_;
    std::deque<EncodedPacket> output_;
    std::vector<std::uint8_t> sequence_header_;
};

}  // namespace

platform::Result<std::unique_ptr<Encoder>> create_encoder(const gpu::Device& device, const EncoderConfig& config) {
    auto enc = MfEncoder::create(device, config);
    if (!enc) return std::unexpected(enc.error());
    return std::unique_ptr<Encoder>{std::move(*enc)};
}

std::vector<Codec> available_encoders(std::uint64_t adapter_luid) {
    std::vector<Codec> out;
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    LUID luid{};
    luid.LowPart = static_cast<DWORD>(adapter_luid & 0xFFFFFFFFu);
    luid.HighPart = static_cast<LONG>(adapter_luid >> 32);
    attrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&luid), sizeof(luid));
    for (const Codec c : kAllCodecs) {
        MFT_REGISTER_TYPE_INFO info{MFMediaType_Video, video_subtype(c)};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                               &info, attrs.Get(), &acts, &count))) {
            if (count > 0) out.push_back(c);
            for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
            CoTaskMemFree(acts);
        }
    }
    return out;
}

}  // namespace lancast::mf
