// Codificador AMD AMF directo (sin Media Foundation): modo "Ultra Low Latency", VBV de 1 frame,
// intra-refresh opcional y salida por slices. El runtime amfrt64.dll lo instala el driver de AMD.

#include "win.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <thread>

#include "components/VideoEncoderAV1.h"
#include "components/VideoEncoderHEVC.h"
#include "components/VideoEncoderVCE.h"
#include "core/Factory.h"
#include "lancast/core/log.hpp"
#include "lancast/platform/mf_codec.hpp"

namespace lancast::mf {
namespace {

// Nombres de propiedades que cambian según el códec.
struct AmfProps {
    const wchar_t* component;
    const wchar_t* usage;
    amf_int64 usage_ull;
    const wchar_t* frame_size;
    const wchar_t* frame_rate;
    const wchar_t* rate_control;
    amf_int64 rc_cbr;
    amf_int64 rc_vbr;
    const wchar_t* target_bitrate;
    const wchar_t* peak_bitrate;
    const wchar_t* vbv;
    const wchar_t* quality_preset;
    amf_int64 preset_speed;
    amf_int64 preset_balanced;
    const wchar_t* query_timeout;
    const wchar_t* output_mode;  // nullptr si no hay salida por slices
    const wchar_t* output_buffer_type;
    const wchar_t* cap_slice_output;
};

const AmfProps& props_for(Codec codec) {
    static const AmfProps h264{AMFVideoEncoderVCE_AVC,
                               AMF_VIDEO_ENCODER_USAGE,
                               AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY,
                               AMF_VIDEO_ENCODER_FRAMESIZE,
                               AMF_VIDEO_ENCODER_FRAMERATE,
                               AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                               AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR,
                               AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,
                               AMF_VIDEO_ENCODER_TARGET_BITRATE,
                               AMF_VIDEO_ENCODER_PEAK_BITRATE,
                               AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,
                               AMF_VIDEO_ENCODER_QUALITY_PRESET,
                               AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED,
                               AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED,
                               AMF_VIDEO_ENCODER_QUERY_TIMEOUT,
                               AMF_VIDEO_ENCODER_OUTPUT_MODE,
                               AMF_VIDEO_ENCODER_OUTPUT_BUFFER_TYPE,
                               AMF_VIDEO_ENCODER_CAP_SUPPORT_SLICE_OUTPUT};
    static const AmfProps hevc{AMFVideoEncoder_HEVC,
                               AMF_VIDEO_ENCODER_HEVC_USAGE,
                               AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY,
                               AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
                               AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
                               AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                               AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR,
                               AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,
                               AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE,
                               AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE,
                               AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE,
                               AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                               AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED,
                               AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED,
                               AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT,
                               AMF_VIDEO_ENCODER_HEVC_OUTPUT_MODE,
                               AMF_VIDEO_ENCODER_HEVC_OUTPUT_BUFFER_TYPE,
                               AMF_VIDEO_ENCODER_HEVC_CAP_SUPPORT_SLICE_OUTPUT};
    static const AmfProps av1{AMFVideoEncoder_AV1,
                              AMF_VIDEO_ENCODER_AV1_USAGE,
                              AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY,
                              AMF_VIDEO_ENCODER_AV1_FRAMESIZE,
                              AMF_VIDEO_ENCODER_AV1_FRAMERATE,
                              AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
                              AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR,
                              AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR,
                              AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE,
                              AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE,
                              AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE,
                              AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET,
                              AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED,
                              AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED,
                              AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT,
                              nullptr,
                              nullptr,
                              nullptr};
    switch (codec) {
        case Codec::Hevc:
            return hevc;
        case Codec::Av1:
            return av1;
        default:
            return h264;
    }
}

std::string amf_error(const char* what, AMF_RESULT r) { return std::format("AMF: {} (código {})", what, static_cast<int>(r)); }

class AmfEncoder final : public Encoder {
public:
    ~AmfEncoder() override {
        running_ = false;
        if (output_thread_.joinable()) output_thread_.join();
        if (encoder_) {
            encoder_->Terminate();
            encoder_.Release();
        }
        if (context_) {
            context_->Terminate();
            context_.Release();
        }
        if (dll_ != nullptr) FreeLibrary(dll_);
    }

    static platform::Result<std::unique_ptr<AmfEncoder>> create(const gpu::Device& device, const EncoderConfig& cfg) {
        std::unique_ptr<AmfEncoder> e{new AmfEncoder()};
        e->config_ = cfg;
        if (auto r = e->init(device); !r) return std::unexpected(r.error());
        return e;
    }

    void set_output_callback(OutputCallback callback) override {
        std::scoped_lock lock(mutex_);
        callback_ = std::move(callback);
    }

    platform::Result<void> submit(ID3D11Texture2D* nv12, std::uint64_t timestamp_us, bool force_keyframe) override {
        amf::AMFSurfacePtr surface;
        AMF_RESULT r = context_->CreateSurfaceFromDX11Native(nv12, &surface, nullptr);
        if (r != AMF_OK) return std::unexpected(platform::Error{amf_error("CreateSurfaceFromDX11Native", r), 0});
        surface->SetPts(static_cast<amf_pts>(timestamp_us) * 10);
        if (force_keyframe) mark_keyframe(surface);

        // Si la cola del codificador está llena, esperar a que el hilo de salida la vacíe.
        const std::uint64_t deadline = GetTickCount64() + 1000;
        while ((r = encoder_->SubmitInput(surface)) == AMF_INPUT_FULL) {
            if (GetTickCount64() > deadline) return std::unexpected(platform::Error{"AMF: el codificador no acepta frames", 0});
            std::unique_lock lock(mutex_);
            drained_cv_.wait_for(lock, std::chrono::milliseconds(2));
        }
        if (r != AMF_OK) return std::unexpected(platform::Error{amf_error("SubmitInput", r), 0});
        return {};
    }

    platform::Result<std::optional<EncodedPacket>> encode(ID3D11Texture2D* nv12, std::uint64_t timestamp_us,
                                                          bool force_keyframe) override {
        if (auto r = submit(nv12, timestamp_us, force_keyframe); !r) return std::unexpected(r.error());
        std::unique_lock lock(mutex_);
        output_cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return !output_.empty(); });
        if (output_.empty()) return std::optional<EncodedPacket>{};
        EncodedPacket p = std::move(output_.front());
        output_.pop_front();
        return std::optional{std::move(p)};
    }

    bool set_bitrate(std::uint32_t kbps) override {
        const AmfProps& p = props_for(config_.codec);
        config_.bitrate_kbps = kbps;
        const amf_int64 bps = static_cast<amf_int64>(kbps) * 1000;
        encoder_->SetProperty(p.target_bitrate, bps);
        encoder_->SetProperty(p.peak_bitrate, config_.rate_control == RateControl::Cbr ? bps : bps * 3 / 2);
        encoder_->SetProperty(p.vbv, vbv_bits(bps));
        return true;
    }

    [[nodiscard]] std::string name() const override { return name_; }
    [[nodiscard]] std::string backend() const override { return "AMF directo"; }
    [[nodiscard]] const EncoderConfig& config() const override { return config_; }
    [[nodiscard]] bool intra_refresh_active() const override { return intra_refresh_; }
    [[nodiscard]] bool slices_active() const override { return slices_; }

private:
    AmfEncoder() = default;

    [[nodiscard]] amf_int64 vbv_bits(amf_int64 bps) const {
        return bps / std::max<std::uint32_t>(config_.fps, 1) * (config_.low_latency ? 1 : 2);
    }

    void mark_keyframe(amf::AMFSurfacePtr& s) const {
        switch (config_.codec) {
            case Codec::H264:
                s->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
                s->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
                s->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
                break;
            case Codec::Hevc:
                s->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                               static_cast<amf_int64>(AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR));
                s->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
                break;
            case Codec::Av1:
                s->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY));
                s->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, true);
                break;
        }
    }

    [[nodiscard]] bool is_keyframe(amf::AMFData* data) const {
        amf_int64 type = -1;
        switch (config_.codec) {
            case Codec::H264:
                data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
                return type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR || type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I;
            case Codec::Hevc:
                data->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &type);
                return type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR || type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_I;
            case Codec::Av1:
                data->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &type);
                return type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY;
        }
        return false;
    }

    platform::Result<void> init(const gpu::Device& device) {
        dll_ = LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dll_ == nullptr) return std::unexpected(platform::Error{"amfrt64.dll no está instalado (driver de AMD)", 0});
        auto init_fn = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll_, AMF_INIT_FUNCTION_NAME));
        if (init_fn == nullptr) return std::unexpected(platform::Error{"amfrt64.dll no exporta AMFInit", 0});
        AMF_RESULT r = init_fn(AMF_FULL_VERSION, &factory_);
        if (r != AMF_OK) return std::unexpected(platform::Error{amf_error("AMFInit (¿driver antiguo?)", r), 0});
        if ((r = factory_->CreateContext(&context_)) != AMF_OK) return std::unexpected(platform::Error{amf_error("CreateContext", r), 0});
        if ((r = context_->InitDX11(device.device.Get())) != AMF_OK) return std::unexpected(platform::Error{amf_error("InitDX11", r), 0});

        const AmfProps& p = props_for(config_.codec);
        if (config_.ten_bit && config_.codec == Codec::H264) {
            return std::unexpected(platform::Error{"AMF: H.264 no admite 10 bits (HDR)", 0});
        }
        if ((r = factory_->CreateComponent(context_, p.component, &encoder_)) != AMF_OK) {
            return std::unexpected(platform::Error{amf_error(std::format("{} no soportado por esta GPU", to_string(config_.codec)).c_str(), r), 0});
        }

        // El "usage" configura todo el conjunto de parámetros: va primero.
        encoder_->SetProperty(p.usage, p.usage_ull);
        encoder_->SetProperty(p.frame_size, AMFConstructSize(static_cast<amf_int32>(config_.size.width),
                                                             static_cast<amf_int32>(config_.size.height)));
        encoder_->SetProperty(p.frame_rate, AMFConstructRate(config_.fps, 1));
        encoder_->SetProperty(p.rate_control, config_.rate_control == RateControl::Cbr ? p.rc_cbr : p.rc_vbr);
        encoder_->SetProperty(p.quality_preset, config_.low_latency ? p.preset_speed : p.preset_balanced);
        set_bitrate(config_.bitrate_kbps);
        encoder_->SetProperty(p.query_timeout, static_cast<amf_int64>(20));  // QueryOutput bloquea hasta 20 ms.

        switch (config_.codec) {
            case Codec::H264:
                encoder_->SetProperty(AMF_VIDEO_ENCODER_PROFILE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_PROFILE_HIGH));
                encoder_->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0));
                encoder_->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(config_.fps) * 3600);
                encoder_->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
                encoder_->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
                encoder_->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE,
                                      static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
                encoder_->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_FULL_RANGE_COLOR, false);
                if (config_.intra_refresh) {
                    // Refresca todo el cuadro en ~0,5 s: tras una pérdida la imagen se repara sola, sin keyframe.
                    const amf_int64 mbs = ((config_.size.width + 15) / 16) * ((config_.size.height + 15) / 16);
                    const amf_int64 frames = std::max<amf_int64>(config_.fps / 2, 8);
                    intra_refresh_ = encoder_->SetProperty(AMF_VIDEO_ENCODER_INTRA_REFRESH_NUM_MBS_PER_SLOT,
                                                           std::max<amf_int64>(mbs / frames, 1)) == AMF_OK;
                }
                break;
            case Codec::Hevc:
                encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE,
                                      static_cast<amf_int64>(config_.ten_bit ? AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10
                                                                             : AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN));
                encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, static_cast<amf_int64>(config_.fps) * 3600);
                encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_NUM_GOPS_PER_IDR, static_cast<amf_int64>(1));
                encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
                encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, false);
                if (config_.ten_bit) {
                    // HDR10: la entrada ya viene en BT.2020 PQ (shader del host); se declara igual a la salida.
                    if (encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH,
                                              static_cast<amf_int64>(AMF_COLOR_BIT_DEPTH_10)) != AMF_OK) {
                        return std::unexpected(platform::Error{"AMF: esta GPU no codifica HEVC de 10 bits", 0});
                    }
                    for (const wchar_t* prop : {AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PROFILE, AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE}) {
                        encoder_->SetProperty(prop, static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020));
                    }
                    for (const wchar_t* prop : {AMF_VIDEO_ENCODER_HEVC_INPUT_TRANSFER_CHARACTERISTIC,
                                                AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC}) {
                        encoder_->SetProperty(prop, static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084));
                    }
                    for (const wchar_t* prop : {AMF_VIDEO_ENCODER_HEVC_INPUT_COLOR_PRIMARIES, AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES}) {
                        encoder_->SetProperty(prop, static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT2020));
                    }
                } else {
                    encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE,
                                          static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
                }
                if (config_.intra_refresh) {
                    const amf_int64 ctbs = ((config_.size.width + 63) / 64) * ((config_.size.height + 63) / 64);
                    const amf_int64 frames = std::max<amf_int64>(config_.fps / 2, 8);
                    intra_refresh_ = encoder_->SetProperty(AMF_VIDEO_ENCODER_HEVC_INTRA_REFRESH_NUM_CTBS_PER_SLOT,
                                                           std::max<amf_int64>(ctbs / frames, 1)) == AMF_OK;
                }
                break;
            case Codec::Av1:
                encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, static_cast<amf_int64>(0));  // Solo el primero.
                encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE,
                                      static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_LOWEST_LATENCY));
                encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_SCREEN_CONTENT_TOOLS, true);
                if (config_.ten_bit) {
                    if (encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH,
                                              static_cast<amf_int64>(AMF_COLOR_BIT_DEPTH_10)) != AMF_OK) {
                        return std::unexpected(platform::Error{"AMF: esta GPU no codifica AV1 de 10 bits", 0});
                    }
                    encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_INPUT_COLOR_PROFILE,
                                          static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020));
                    encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE,
                                          static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020));
                    encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_TRANSFER_CHARACTERISTIC,
                                          static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084));
                    encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PRIMARIES,
                                          static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT2020));
                } else {
                    encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE,
                                          static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
                }
                if (config_.intra_refresh) {
                    intra_refresh_ = encoder_->SetProperty(AMF_VIDEO_ENCODER_AV1_INTRA_REFRESH_MODE,
                                                           static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_INTRA_REFRESH_MODE__CONTINUOUS)) == AMF_OK;
                }
                break;
        }

        // Salida por slices (envío sub-frame) si la GPU lo soporta.
        if (config_.slices && p.output_mode != nullptr) {
            amf::AMFCapsPtr caps;
            bool supported = false;
            if (encoder_->GetCaps(&caps) == AMF_OK && caps) caps->GetProperty(p.cap_slice_output, &supported);
            if (supported) {
                const amf_int64 slices = config_.size.height >= 1080 ? 4 : 2;
                encoder_->SetProperty(config_.codec == Codec::Hevc ? AMF_VIDEO_ENCODER_HEVC_SLICES_PER_FRAME
                                                                   : AMF_VIDEO_ENCODER_SLICES_PER_FRAME,
                                      slices);
                slices_ = encoder_->SetProperty(p.output_mode, static_cast<amf_int64>(1)) == AMF_OK;  // OUTPUT_MODE_SLICE
            }
        }

        if ((r = encoder_->Init(config_.ten_bit ? amf::AMF_SURFACE_P010 : amf::AMF_SURFACE_NV12,
                                static_cast<amf_int32>(config_.size.width),
                                static_cast<amf_int32>(config_.size.height))) != AMF_OK) {
            return std::unexpected(platform::Error{
                amf_error(std::format("Init {}x{}", config_.size.width, config_.size.height).c_str(), r), 0});
        }
        name_ = std::format("AMD AMF {}{}", to_string(config_.codec), config_.ten_bit ? " 10 bits" : "");
        running_ = true;
        output_thread_ = std::thread([this] { output_loop(); });
        log::info("Codificador: {} directo ({}x{} @ {} fps, {} kbps, {}{}{})", name_, config_.size.width, config_.size.height,
                  config_.fps, config_.bitrate_kbps, to_string(config_.rate_control), intra_refresh_ ? ", intra-refresh" : "",
                  slices_ ? ", slices" : "");
        return {};
    }

    void output_loop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const AmfProps& p = props_for(config_.codec);
        std::uint8_t slice_index = 0;
        while (running_) {
            amf::AMFDataPtr data;
            const AMF_RESULT r = encoder_->QueryOutput(&data);  // Bloquea hasta QUERY_TIMEOUT.
            if (r == AMF_EOF) break;
            if (r != AMF_OK || !data) continue;
            drained_cv_.notify_all();

            amf::AMFBufferPtr buffer(data);
            if (!buffer) continue;
            EncodedPacket packet;
            const auto* bytes = static_cast<const std::uint8_t*>(buffer->GetNative());
            packet.data.assign(bytes, bytes + buffer->GetSize());
            packet.timestamp_us = static_cast<std::uint64_t>(data->GetPts() / 10);
            packet.keyframe = is_keyframe(data);
            if (slices_ && p.output_buffer_type != nullptr) {
                amf_int64 type = 0;
                data->GetProperty(p.output_buffer_type, &type);
                packet.slice_index = slice_index;
                packet.last_slice = type != 1;  // 1 = SLICE (intermedio); 0 = FRAME, 2 = SLICE_LAST
                slice_index = packet.last_slice ? 0 : static_cast<std::uint8_t>(slice_index + 1);
            }

            OutputCallback cb;
            {
                std::scoped_lock lock(mutex_);
                cb = callback_;
                if (!cb) {
                    output_.push_back(std::move(packet));
                    output_cv_.notify_all();
                    continue;
                }
            }
            cb(std::move(packet));
        }
    }

    EncoderConfig config_;
    std::string name_ = "AMD AMF";
    HMODULE dll_ = nullptr;
    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContextPtr context_;
    amf::AMFComponentPtr encoder_;
    bool intra_refresh_ = false;
    bool slices_ = false;

    std::atomic<bool> running_{false};
    std::thread output_thread_;
    std::mutex mutex_;
    std::condition_variable output_cv_;
    std::condition_variable drained_cv_;
    OutputCallback callback_;
    std::deque<EncodedPacket> output_;
};

}  // namespace

platform::Result<std::unique_ptr<Encoder>> create_amf_encoder(const gpu::Device& device, const EncoderConfig& config) {
    auto e = AmfEncoder::create(device, config);
    if (!e) return std::unexpected(e.error());
    return std::unique_ptr<Encoder>{std::move(*e)};
}

}  // namespace lancast::mf
