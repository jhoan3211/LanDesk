// Codificador NVIDIA NVENC directo (sin Media Foundation): preset P1 + "ultra low latency", modo asíncrono
// con eventos (varios frames en vuelo), intra-refresh bajo demanda y varios slices por frame.
// nvEncodeAPI64.dll lo instala el driver de NVIDIA (se necesita un driver compatible con la API 13.1).

#include "win.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <thread>

#include "lancast/core/log.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "nvEncodeAPI.h"

namespace lancast::mf {
namespace {

using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using MaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);

constexpr std::size_t kSlots = 6;  // Frames que pueden estar codificándose a la vez.

struct Slot {
    NV_ENC_OUTPUT_PTR bitstream = nullptr;
    HANDLE event = nullptr;
    NV_ENC_INPUT_PTR mapped = nullptr;
    bool busy = false;
};

class NvencEncoder final : public Encoder {
public:
    ~NvencEncoder() override {
        running_ = false;
        cv_.notify_all();
        if (output_thread_.joinable()) output_thread_.join();
        if (encoder_ != nullptr) {
            // Vaciar la cola del codificador antes de liberar.
            NV_ENC_PIC_PARAMS eos{};
            eos.version = NV_ENC_PIC_PARAMS_VER;
            eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
            api_.nvEncEncodePicture(encoder_, &eos);
            for (Slot& s : slots_) {
                if (s.mapped != nullptr) api_.nvEncUnmapInputResource(encoder_, s.mapped);
                if (s.bitstream != nullptr) api_.nvEncDestroyBitstreamBuffer(encoder_, s.bitstream);
                if (s.event != nullptr) {
                    NV_ENC_EVENT_PARAMS ev{};
                    ev.version = NV_ENC_EVENT_PARAMS_VER;
                    ev.completionEvent = s.event;
                    api_.nvEncUnregisterAsyncEvent(encoder_, &ev);
                    CloseHandle(s.event);
                }
            }
            for (auto& [tex, reg] : registered_) api_.nvEncUnregisterResource(encoder_, reg);
            api_.nvEncDestroyEncoder(encoder_);
        }
        if (dll_ != nullptr) FreeLibrary(dll_);
    }

    static platform::Result<std::unique_ptr<NvencEncoder>> create(const gpu::Device& device, const EncoderConfig& cfg) {
        std::unique_ptr<NvencEncoder> e{new NvencEncoder()};
        e->config_ = cfg;
        if (auto r = e->init(device); !r) return std::unexpected(r.error());
        return e;
    }

    void set_output_callback(OutputCallback callback) override {
        std::scoped_lock lock(mutex_);
        callback_ = std::move(callback);
    }

    platform::Result<void> submit(ID3D11Texture2D* nv12, std::uint64_t timestamp_us, bool force_keyframe) override {
        // Esperar un hueco libre (contrapresión si la GPU va justa).
        std::size_t index = kSlots;
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::seconds(1), [&] {
                    for (std::size_t i = 0; i < kSlots; ++i) {
                        if (!slots_[(next_slot_ + i) % kSlots].busy) return true;
                    }
                    return false;
                })) {
                return std::unexpected(platform::Error{"NVENC: el codificador no responde", 0});
            }
            for (std::size_t i = 0; i < kSlots; ++i) {
                const std::size_t s = (next_slot_ + i) % kSlots;
                if (!slots_[s].busy) {
                    index = s;
                    break;
                }
            }
            slots_[index].busy = true;
            next_slot_ = (index + 1) % kSlots;
        }
        Slot& slot = slots_[index];

        // Registrar la textura la primera vez que se ve (el pool de texturas es fijo).
        NV_ENC_REGISTERED_PTR reg = nullptr;
        if (const auto it = registered_.find(nv12); it != registered_.end()) {
            reg = it->second;
        } else {
            NV_ENC_REGISTER_RESOURCE r{};
            r.version = NV_ENC_REGISTER_RESOURCE_VER;
            r.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
            r.width = config_.size.width;
            r.height = config_.size.height;
            r.resourceToRegister = nv12;
            r.bufferFormat = config_.ten_bit ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
            r.bufferUsage = NV_ENC_INPUT_IMAGE;
            if (const NVENCSTATUS st = api_.nvEncRegisterResource(encoder_, &r); st != NV_ENC_SUCCESS) {
                release(index);
                return std::unexpected(platform::Error{std::format("NVENC: nvEncRegisterResource ({})", static_cast<int>(st)), 0});
            }
            reg = r.registeredResource;
            registered_[nv12] = reg;
        }

        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = reg;
        if (const NVENCSTATUS st = api_.nvEncMapInputResource(encoder_, &map); st != NV_ENC_SUCCESS) {
            release(index);
            return std::unexpected(platform::Error{std::format("NVENC: nvEncMapInputResource ({})", static_cast<int>(st)), 0});
        }
        slot.mapped = map.mappedResource;

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = config_.size.width;
        pic.inputHeight = config_.size.height;
        pic.inputPitch = config_.size.width;
        pic.inputBuffer = map.mappedResource;
        pic.bufferFmt = map.mappedBufferFmt;
        pic.outputBitstream = slot.bitstream;
        pic.completionEvent = slot.event;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = timestamp_us;
        if (force_keyframe) {
            pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        } else if (refresh_requested_.exchange(false) && intra_refresh_) {
            // Ola de intra-refresh: repara la imagen en pocos frames sin el pico de un keyframe.
            const std::uint32_t frames = refresh_frames();
            switch (config_.codec) {
                case Codec::H264:
                    pic.codecPicParams.h264PicParams.forceIntraRefreshWithFrameCnt = frames;
                    break;
                case Codec::Hevc:
                    pic.codecPicParams.hevcPicParams.forceIntraRefreshWithFrameCnt = frames;
                    break;
                case Codec::Av1:
                    pic.codecPicParams.av1PicParams.forceIntraRefreshWithFrameCnt = frames;
                    break;
            }
        }

        const NVENCSTATUS st = api_.nvEncEncodePicture(encoder_, &pic);
        if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
            api_.nvEncUnmapInputResource(encoder_, slot.mapped);
            slot.mapped = nullptr;
            release(index);
            return std::unexpected(platform::Error{std::format("NVENC: nvEncEncodePicture ({})", static_cast<int>(st)), 0});
        }
        {
            std::scoped_lock lock(mutex_);
            in_flight_.push_back(index);
        }
        cv_.notify_all();
        return {};
    }

    platform::Result<std::optional<EncodedPacket>> encode(ID3D11Texture2D* nv12, std::uint64_t timestamp_us,
                                                          bool force_keyframe) override {
        if (auto r = submit(nv12, timestamp_us, force_keyframe); !r) return std::unexpected(r.error());
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return !output_.empty(); });
        if (output_.empty()) return std::optional<EncodedPacket>{};
        EncodedPacket p = std::move(output_.front());
        output_.pop_front();
        return std::optional{std::move(p)};
    }

    bool set_bitrate(std::uint32_t kbps) override {
        config_.bitrate_kbps = kbps;
        apply_rate_control(encode_config_);
        NV_ENC_RECONFIGURE_PARAMS re{};
        re.version = NV_ENC_RECONFIGURE_PARAMS_VER;
        re.reInitEncodeParams = init_params_;
        re.reInitEncodeParams.encodeConfig = &encode_config_;
        return api_.nvEncReconfigureEncoder(encoder_, &re) == NV_ENC_SUCCESS;
    }

    void request_intra_refresh() override { refresh_requested_ = true; }

    [[nodiscard]] std::string name() const override { return name_; }
    [[nodiscard]] std::string backend() const override { return "NVENC directo"; }
    [[nodiscard]] const EncoderConfig& config() const override { return config_; }
    [[nodiscard]] bool intra_refresh_active() const override { return intra_refresh_; }

private:
    NvencEncoder() = default;

    [[nodiscard]] std::uint32_t refresh_frames() const { return std::max<std::uint32_t>(config_.fps / 6, 4); }

    void release(std::size_t index) {
        std::scoped_lock lock(mutex_);
        slots_[index].busy = false;
        cv_.notify_all();
    }

    void apply_rate_control(NV_ENC_CONFIG& c) const {
        const std::uint32_t bps = config_.bitrate_kbps * 1000;
        c.rcParams.rateControlMode = config_.rate_control == RateControl::Cbr ? NV_ENC_PARAMS_RC_CBR : NV_ENC_PARAMS_RC_VBR;
        c.rcParams.averageBitRate = bps;
        c.rcParams.maxBitRate = config_.rate_control == RateControl::Cbr ? bps : bps / 2 * 3;
        // VBV de 1 frame: cada frame ocupa lo mismo y la red no hace cola.
        const std::uint32_t frame_bits = bps / std::max<std::uint32_t>(config_.fps, 1) * (config_.low_latency ? 1 : 2);
        c.rcParams.vbvBufferSize = frame_bits;
        c.rcParams.vbvInitialDelay = frame_bits;
        c.rcParams.zeroReorderDelay = 1;
    }

    platform::Result<void> init(const gpu::Device& device) {
        dll_ = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dll_ == nullptr) return std::unexpected(platform::Error{"nvEncodeAPI64.dll no está instalado (driver de NVIDIA)", 0});
        auto max_version = reinterpret_cast<MaxVersionFn>(GetProcAddress(dll_, "NvEncodeAPIGetMaxSupportedVersion"));
        auto create_instance = reinterpret_cast<CreateInstanceFn>(GetProcAddress(dll_, "NvEncodeAPICreateInstance"));
        if (max_version == nullptr || create_instance == nullptr) {
            return std::unexpected(platform::Error{"nvEncodeAPI64.dll incompleto", 0});
        }
        uint32_t driver_version = 0;
        max_version(&driver_version);
        const uint32_t needed = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (driver_version < needed) {
            return std::unexpected(platform::Error{
                std::format("el driver de NVIDIA soporta NVENC {}.{}; LanCast necesita {}.{} (actualiza el driver)",
                            driver_version >> 4, driver_version & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION),
                0});
        }
        api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (create_instance(&api_) != NV_ENC_SUCCESS) return std::unexpected(platform::Error{"NvEncodeAPICreateInstance", 0});

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        open.device = device.device.Get();
        open.apiVersion = NVENCAPI_VERSION;
        if (const NVENCSTATUS st = api_.nvEncOpenEncodeSessionEx(&open, &encoder_); st != NV_ENC_SUCCESS) {
            encoder_ = nullptr;
            return std::unexpected(platform::Error{std::format("NVENC: no se pudo abrir la sesión ({})", static_cast<int>(st)), 0});
        }

        if (config_.ten_bit && config_.codec == Codec::H264) {
            return std::unexpected(platform::Error{"NVENC: H.264 no admite 10 bits (HDR)", 0});
        }
        GUID codec_guid = NV_ENC_CODEC_H264_GUID;
        GUID profile_guid = NV_ENC_H264_PROFILE_HIGH_GUID;
        if (config_.codec == Codec::Hevc) {
            codec_guid = NV_ENC_CODEC_HEVC_GUID;
            profile_guid = config_.ten_bit ? NV_ENC_HEVC_PROFILE_MAIN10_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID;
        } else if (config_.codec == Codec::Av1) {
            codec_guid = NV_ENC_CODEC_AV1_GUID;
            profile_guid = NV_ENC_AV1_PROFILE_MAIN_GUID;
        }
        const GUID preset = config_.low_latency ? NV_ENC_PRESET_P1_GUID : NV_ENC_PRESET_P4_GUID;

        NV_ENC_PRESET_CONFIG preset_config{};
        preset_config.version = NV_ENC_PRESET_CONFIG_VER;
        preset_config.presetCfg.version = NV_ENC_CONFIG_VER;
        if (const NVENCSTATUS st = api_.nvEncGetEncodePresetConfigEx(encoder_, codec_guid, preset, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
                                                                     &preset_config);
            st != NV_ENC_SUCCESS) {
            return std::unexpected(platform::Error{
                std::format("NVENC: {} no soportado por esta GPU ({})", to_string(config_.codec), static_cast<int>(st)), 0});
        }
        encode_config_ = preset_config.presetCfg;
        encode_config_.version = NV_ENC_CONFIG_VER;
        encode_config_.profileGUID = profile_guid;
        encode_config_.gopLength = NVENC_INFINITE_GOPLENGTH;
        encode_config_.frameIntervalP = 1;  // Sin frames B.
        apply_rate_control(encode_config_);

        intra_refresh_ = config_.intra_refresh;
        const std::uint32_t slices = config_.size.height >= 1080 ? 4 : 2;
        switch (config_.codec) {
            case Codec::H264: {
                auto& h = encode_config_.encodeCodecConfig.h264Config;
                h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                h.repeatSPSPPS = 1;
                h.enableIntraRefresh = intra_refresh_ ? 1 : 0;
                h.intraRefreshPeriod = intra_refresh_ ? config_.fps * 3600 : 0;  // Solo bajo demanda.
                h.intraRefreshCnt = intra_refresh_ ? refresh_frames() : 0;
                if (config_.slices) {
                    h.sliceMode = 3;  // Número de slices por frame.
                    h.sliceModeData = slices;
                }
                auto& v = h.h264VUIParameters;
                v.videoSignalTypePresentFlag = 1;
                v.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
                v.videoFullRangeFlag = 0;
                v.colourDescriptionPresentFlag = 1;
                v.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
                v.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
                v.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
                break;
            }
            case Codec::Hevc: {
                auto& h = encode_config_.encodeCodecConfig.hevcConfig;
                h.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                h.repeatSPSPPS = 1;
                h.enableIntraRefresh = intra_refresh_ ? 1 : 0;
                h.intraRefreshPeriod = intra_refresh_ ? config_.fps * 3600 : 0;
                h.intraRefreshCnt = intra_refresh_ ? refresh_frames() : 0;
                if (config_.slices) {
                    h.sliceMode = 3;
                    h.sliceModeData = slices;
                }
                auto& v = h.hevcVUIParameters;
                v.videoSignalTypePresentFlag = 1;
                v.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
                v.videoFullRangeFlag = 0;
                v.colourDescriptionPresentFlag = 1;
                v.colourPrimaries = config_.ten_bit ? NV_ENC_VUI_COLOR_PRIMARIES_BT2020 : NV_ENC_VUI_COLOR_PRIMARIES_BT709;
                v.transferCharacteristics = config_.ten_bit ? NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084
                                                            : NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
                v.colourMatrix = config_.ten_bit ? NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL : NV_ENC_VUI_MATRIX_COEFFS_BT709;
                if (config_.ten_bit) {
                    h.inputBitDepth = NV_ENC_BIT_DEPTH_10;
                    h.outputBitDepth = NV_ENC_BIT_DEPTH_10;
                }
                break;
            }
            case Codec::Av1: {
                auto& a = encode_config_.encodeCodecConfig.av1Config;
                a.idrPeriod = NVENC_INFINITE_GOPLENGTH;
                a.repeatSeqHdr = 1;
                a.enableIntraRefresh = intra_refresh_ ? 1 : 0;
                a.intraRefreshPeriod = intra_refresh_ ? config_.fps * 3600 : 0;
                a.intraRefreshCnt = intra_refresh_ ? refresh_frames() : 0;
                a.colorPrimaries = config_.ten_bit ? NV_ENC_VUI_COLOR_PRIMARIES_BT2020 : NV_ENC_VUI_COLOR_PRIMARIES_BT709;
                a.transferCharacteristics = config_.ten_bit ? NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084
                                                            : NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
                a.matrixCoefficients = config_.ten_bit ? NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL : NV_ENC_VUI_MATRIX_COEFFS_BT709;
                if (config_.ten_bit) {
                    a.inputBitDepth = NV_ENC_BIT_DEPTH_10;
                    a.outputBitDepth = NV_ENC_BIT_DEPTH_10;
                }
                a.colorRange = 0;
                break;
            }
        }

        init_params_ = NV_ENC_INITIALIZE_PARAMS{};
        init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
        init_params_.encodeGUID = codec_guid;
        init_params_.presetGUID = preset;
        init_params_.encodeWidth = config_.size.width;
        init_params_.encodeHeight = config_.size.height;
        init_params_.darWidth = config_.size.width;
        init_params_.darHeight = config_.size.height;
        init_params_.frameRateNum = config_.fps;
        init_params_.frameRateDen = 1;
        init_params_.enableEncodeAsync = 1;
        init_params_.enablePTD = 1;
        init_params_.encodeConfig = &encode_config_;
        init_params_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        init_params_.maxEncodeWidth = config_.size.width;
        init_params_.maxEncodeHeight = config_.size.height;
        if (const NVENCSTATUS st = api_.nvEncInitializeEncoder(encoder_, &init_params_); st != NV_ENC_SUCCESS) {
            return std::unexpected(platform::Error{
                std::format("NVENC: no acepta {}x{} @ {} fps ({})", config_.size.width, config_.size.height, config_.fps,
                            static_cast<int>(st)),
                0});
        }

        for (Slot& s : slots_) {
            NV_ENC_CREATE_BITSTREAM_BUFFER b{};
            b.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            if (api_.nvEncCreateBitstreamBuffer(encoder_, &b) != NV_ENC_SUCCESS) {
                return std::unexpected(platform::Error{"NVENC: nvEncCreateBitstreamBuffer", 0});
            }
            s.bitstream = b.bitstreamBuffer;
            s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            NV_ENC_EVENT_PARAMS ev{};
            ev.version = NV_ENC_EVENT_PARAMS_VER;
            ev.completionEvent = s.event;
            if (api_.nvEncRegisterAsyncEvent(encoder_, &ev) != NV_ENC_SUCCESS) {
                return std::unexpected(platform::Error{"NVENC: nvEncRegisterAsyncEvent", 0});
            }
        }

        name_ = std::format("NVIDIA NVENC {}", to_string(config_.codec));
        running_ = true;
        output_thread_ = std::thread([this] { output_loop(); });
        log::info("Codificador: {} directo ({}x{} @ {} fps, {} kbps, {}, preset {}{})", name_, config_.size.width,
                  config_.size.height, config_.fps, config_.bitrate_kbps, to_string(config_.rate_control),
                  config_.low_latency ? "P1" : "P4", intra_refresh_ ? ", intra-refresh" : "");
        return {};
    }

    // Recoge los frames terminados en orden (el codificador asíncrono avisa con un evento por frame).
    void output_loop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        while (running_) {
            std::size_t index = kSlots;
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(50), [&] { return !in_flight_.empty() || !running_; });
                if (!running_) break;
                if (in_flight_.empty()) continue;
                index = in_flight_.front();
            }
            Slot& slot = slots_[index];
            if (WaitForSingleObject(slot.event, 100) != WAIT_OBJECT_0) continue;

            EncodedPacket packet;
            NV_ENC_LOCK_BITSTREAM lock_params{};
            lock_params.version = NV_ENC_LOCK_BITSTREAM_VER;
            lock_params.outputBitstream = slot.bitstream;
            if (api_.nvEncLockBitstream(encoder_, &lock_params) == NV_ENC_SUCCESS) {
                const auto* bytes = static_cast<const std::uint8_t*>(lock_params.bitstreamBufferPtr);
                packet.data.assign(bytes, bytes + lock_params.bitstreamSizeInBytes);
                packet.timestamp_us = lock_params.outputTimeStamp;
                packet.keyframe = lock_params.pictureType == NV_ENC_PIC_TYPE_IDR || lock_params.pictureType == NV_ENC_PIC_TYPE_I;
                api_.nvEncUnlockBitstream(encoder_, slot.bitstream);
            }
            if (slot.mapped != nullptr) {
                api_.nvEncUnmapInputResource(encoder_, slot.mapped);
                slot.mapped = nullptr;
            }

            OutputCallback cb;
            {
                std::scoped_lock lock(mutex_);
                in_flight_.pop_front();
                slot.busy = false;
                cb = callback_;
                if (!cb && !packet.data.empty()) output_.push_back(std::move(packet));
            }
            cv_.notify_all();
            if (cb && !packet.data.empty()) cb(std::move(packet));
        }
    }

    EncoderConfig config_;
    std::string name_ = "NVIDIA NVENC";
    HMODULE dll_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* encoder_ = nullptr;
    NV_ENC_CONFIG encode_config_{};
    NV_ENC_INITIALIZE_PARAMS init_params_{};
    bool intra_refresh_ = false;
    std::atomic<bool> refresh_requested_{false};

    std::array<Slot, kSlots> slots_{};
    std::size_t next_slot_ = 0;
    std::map<ID3D11Texture2D*, NV_ENC_REGISTERED_PTR> registered_;
    std::deque<std::size_t> in_flight_;
    std::deque<EncodedPacket> output_;
    std::atomic<bool> running_{false};
    std::thread output_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    OutputCallback callback_;
};

}  // namespace

platform::Result<std::unique_ptr<Encoder>> create_nvenc_encoder(const gpu::Device& device, const EncoderConfig& config) {
    auto e = NvencEncoder::create(device, config);
    if (!e) return std::unexpected(e.error());
    return std::unique_ptr<Encoder>{std::move(*e)};
}

}  // namespace lancast::mf
