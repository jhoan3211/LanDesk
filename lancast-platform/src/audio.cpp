#include "lancast/platform/audio.hpp"

#include "win.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>

#include "lancast/core/log.hpp"
#include "lancast/platform/system.hpp"
#include "win.hpp"

namespace lancast::audio {
namespace {

using platform::detail::ComPtr;

constexpr REFERENCE_TIME kHns = 10'000;  // 1 ms en unidades de 100 ns.

std::int16_t to_s16(float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<std::int16_t>(std::lround(v * 32767.0f));
}

ComPtr<IMMDevice> default_render_device() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return {};
    ComPtr<IMMDevice> device;
    enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    return device;
}

}  // namespace

void LoopbackCapture::start(Callback callback) {
    stop();
    callback_ = std::move(callback);
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void LoopbackCapture::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

Format LoopbackCapture::format() const {
    std::scoped_lock lock(mutex_);
    return format_;
}

void LoopbackCapture::run() {
    platform::detail::ComScope com;
    sys::RealtimeThread mmcss(L"Pro Audio");
    std::vector<std::int16_t> out;

    while (running_) {
        ComPtr<IMMDevice> device = default_render_device();
        ComPtr<IAudioClient> client;
        if (!device || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            Sleep(500);
            continue;
        }
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client->GetMixFormat(&mix))) {
            Sleep(500);
            continue;
        }
        bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
            is_float = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }
        const int src_channels = mix->nChannels;
        const int bits = mix->wBitsPerSample;
        {
            std::scoped_lock lock(mutex_);
            format_ = Format{mix->nSamplesPerSec, 2};
        }
        const Format fmt = format();

        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 20 * kHns, 0, mix, nullptr);
        CoTaskMemFree(mix);
        ComPtr<IAudioCaptureClient> capture;
        if (FAILED(hr) || FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) {
            log::warn("Audio: no se pudo iniciar la captura loopback (0x{:08X})", static_cast<std::uint32_t>(hr));
            Sleep(1000);
            continue;
        }
        log::info("Audio: capturando {} Hz, {} canales de origen", fmt.sample_rate, src_channels);

        bool device_ok = true;
        while (running_ && device_ok) {
            Sleep(4);
            UINT32 packet = 0;
            while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                out.assign(static_cast<std::size_t>(frames) * 2, 0);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data != nullptr) {
                    for (UINT32 f = 0; f < frames; ++f) {
                        float l = 0;
                        float r = 0;
                        if (is_float && bits == 32) {
                            const auto* s = reinterpret_cast<const float*>(data) + static_cast<std::size_t>(f) * src_channels;
                            l = s[0];
                            r = src_channels > 1 ? s[1] : s[0];
                        } else if (bits == 16) {
                            const auto* s = reinterpret_cast<const std::int16_t*>(data) + static_cast<std::size_t>(f) * src_channels;
                            l = s[0] / 32768.0f;
                            r = (src_channels > 1 ? s[1] : s[0]) / 32768.0f;
                        } else if (bits == 32) {
                            const auto* s = reinterpret_cast<const std::int32_t*>(data) + static_cast<std::size_t>(f) * src_channels;
                            l = static_cast<float>(s[0] / 2147483648.0);
                            r = static_cast<float>((src_channels > 1 ? s[1] : s[0]) / 2147483648.0);
                        }
                        out[2 * f] = to_s16(l);
                        out[2 * f + 1] = to_s16(r);
                    }
                }
                capture->ReleaseBuffer(frames);
                if (callback_) callback_(out, frames, fmt);
            }
            if (FAILED(hr)) device_ok = false;  // Dispositivo cambiado o desconectado: reiniciar.
        }
        client->Stop();
    }
}

void Player::start(const Format& format) {
    stop();
    format_ = format;
    {
        std::scoped_lock lock(mutex_);
        ring_.assign(static_cast<std::size_t>(format.sample_rate) * format.channels / 2, 0);  // 500 ms
        read_ = 0;
        size_ = 0;
        primed_ = false;
    }
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void Player::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Player::push(std::span<const std::int16_t> samples) {
    std::scoped_lock lock(mutex_);
    if (ring_.empty()) return;
    const std::size_t cap = ring_.size();
    // Si el buffer pasa del objetivo + 60 ms se descarta lo más viejo: la latencia manda.
    const std::size_t max_fill = static_cast<std::size_t>(format_.sample_rate) * format_.channels * (target_ms_.load() + 60) / 1000;
    for (const std::int16_t s : samples) {
        if (size_ == cap) {
            read_ = (read_ + 1) % cap;
            --size_;
        }
        ring_[(read_ + size_) % cap] = s;
        ++size_;
    }
    while (size_ > max_fill) {
        read_ = (read_ + format_.channels) % cap;
        size_ -= std::min<std::size_t>(size_, format_.channels);
    }
}

double Player::buffered_ms() const {
    std::scoped_lock lock(mutex_);
    if (format_.sample_rate == 0 || format_.channels == 0) return 0;
    return static_cast<double>(size_) / format_.channels * 1000.0 / format_.sample_rate;
}

void Player::run() {
    platform::detail::ComScope com;
    sys::RealtimeThread mmcss(L"Pro Audio", true);
    while (running_) {
        ComPtr<IMMDevice> device = default_render_device();
        ComPtr<IAudioClient> client;
        if (!device || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            Sleep(500);
            continue;
        }
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = format_.channels;
        wf.nSamplesPerSec = format_.sample_rate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = static_cast<WORD>(wf.nChannels * 2);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                        20 * kHns, 0, &wf, nullptr);
        ComPtr<IAudioRenderClient> render;
        UINT32 buffer_frames = 0;
        if (FAILED(hr) || FAILED(client->SetEventHandle(event)) || FAILED(client->GetBufferSize(&buffer_frames)) ||
            FAILED(client->GetService(IID_PPV_ARGS(&render))) || FAILED(client->Start())) {
            log::warn("Audio: no se pudo iniciar la reproducción (0x{:08X})", static_cast<std::uint32_t>(hr));
            CloseHandle(event);
            Sleep(1000);
            continue;
        }

        const std::size_t ch = format_.channels;
        bool device_ok = true;
        while (running_ && device_ok) {
            WaitForSingleObject(event, 50);
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) {
                device_ok = false;
                break;
            }
            const UINT32 frames = buffer_frames - padding;
            if (frames == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(render->GetBuffer(frames, &data))) {
                device_ok = false;
                break;
            }
            auto* dst = reinterpret_cast<std::int16_t*>(data);
            std::size_t written = 0;
            const std::size_t prime = static_cast<std::size_t>(format_.sample_rate) * ch * target_ms_.load() / 1000;
            const float volume = volume_.load();
            {
                std::scoped_lock lock(mutex_);
                if (!primed_ && size_ >= prime) primed_ = true;
                if (primed_) {
                    const std::size_t want = static_cast<std::size_t>(frames) * ch;
                    const std::size_t take = std::min(want, size_ - size_ % ch);
                    for (std::size_t i = 0; i < take; ++i) {
                        const float v = static_cast<float>(ring_[(read_ + i) % ring_.size()]) * volume;
                        dst[i] = static_cast<std::int16_t>(std::clamp(v, -32768.0f, 32767.0f));
                    }
                    read_ = (read_ + take) % ring_.size();
                    size_ -= take;
                    written = take;
                    if (take < want) {
                        primed_ = false;
                        if (take > 0 || size_ == 0) underruns_.fetch_add(1);
                    }
                }
            }
            std::fill(dst + written, dst + static_cast<std::size_t>(frames) * ch, std::int16_t{0});
            render->ReleaseBuffer(frames, 0);
        }
        client->Stop();
        CloseHandle(event);
    }
}

}  // namespace lancast::audio
