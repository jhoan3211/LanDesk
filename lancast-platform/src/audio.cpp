#include "lancast/platform/audio.hpp"

#include "win.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>

#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

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

ComPtr<IMMDevice> default_capture_device() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return {};
    ComPtr<IMMDevice> device;
    enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
    return device;
}

ComPtr<IMMDevice> device_by_id(const std::wstring& id) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return {};
    ComPtr<IMMDevice> device;
    enumerator->GetDevice(id.c_str(), &device);
    return device;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

void LoopbackCapture::start(Callback callback, Source source) {
    stop();
    callback_ = std::move(callback);
    source_ = source;
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
        const bool mic = source_ == Source::Microphone;
        ComPtr<IMMDevice> device = mic ? default_capture_device() : default_render_device();
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

        if (mic && !raw_) {
            // Modo comunicaciones: Windows aplica sus efectos de voz (cancelación de eco y de ruido si el driver los
            // tiene). Va antes de Initialize.
            ComPtr<IAudioClient2> client2;
            if (SUCCEEDED(client.As(&client2))) {
                AudioClientProperties props{};
                props.cbSize = sizeof(props);
                props.eCategory = AudioCategory_Communications;
                client2->SetClientProperties(&props);
            }
        }
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, mic ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK, 20 * kHns, 0, mix, nullptr);
        CoTaskMemFree(mix);
        echo_cancellation_ = false;
        if (mic && !raw_ && SUCCEEDED(hr)) {
            // Windows 11: la referencia del eco es la salida predeterminada (null), donde el cliente reproduce el host.
            ComPtr<IAcousticEchoCancellationControl> aec;
            if (SUCCEEDED(client->GetService(IID_PPV_ARGS(&aec))) && SUCCEEDED(aec->SetEchoCancellationRenderEndpoint(nullptr))) {
                echo_cancellation_ = true;
            }
        }
        ComPtr<IAudioCaptureClient> capture;
        if (FAILED(hr) || FAILED(client->GetService(IID_PPV_ARGS(&capture))) || FAILED(client->Start())) {
            log::warn("Audio: no se pudo iniciar la captura {} (0x{:08X})", mic ? "del micrófono" : "loopback",
                      static_cast<std::uint32_t>(hr));
            Sleep(1000);
            continue;
        }
        log::info("Audio: capturando {} a {} Hz, {} canales de origen{}", mic ? "el micrófono" : "el sonido del equipo", fmt.sample_rate,
                  src_channels, !mic ? "" : echo_cancellation_ ? " · cancelación de eco de Windows activa" : " · sin cancelación de eco del sistema");

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

void Player::start(const Format& format, std::wstring device_id) {
    stop();
    format_ = format;
    device_id_ = std::move(device_id);
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
        ComPtr<IMMDevice> device = device_id_.empty() ? default_render_device() : device_by_id(device_id_);
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
            {
                std::scoped_lock lock(tap_mutex_);
                if (tap_) tap_({dst, static_cast<std::size_t>(frames) * ch}, format_);
            }
            render->ReleaseBuffer(frames, 0);
        }
        client->Stop();
        CloseHandle(event);
    }
}

struct EchoCanceller::State {
    SpeexEchoState* echo = nullptr;
    SpeexPreprocessState* pre = nullptr;
    int frame = 0;
    std::mutex mutex;
    std::deque<std::int16_t> ref;  // Lo reproducido que todavía no se usó como referencia.
    std::vector<std::int16_t> mic;  // Resto del micrófono que no completa un bloque.
    std::vector<std::int16_t> ref_block;
    std::vector<std::int16_t> out_block;
};

EchoCanceller::EchoCanceller(std::uint32_t rate, std::uint32_t tail_ms) : st_(std::make_unique<State>()), rate_(rate) {
    st_->frame = static_cast<int>(rate / 100);
    st_->echo = speex_echo_state_init(st_->frame, static_cast<int>(rate * tail_ms / 1000));
    int r = static_cast<int>(rate);
    speex_echo_ctl(st_->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &r);
    st_->pre = speex_preprocess_state_init(st_->frame, r);
    speex_preprocess_ctl(st_->pre, SPEEX_PREPROCESS_SET_ECHO_STATE, st_->echo);
    int on = 1;
    int noise = -15;
    speex_preprocess_ctl(st_->pre, SPEEX_PREPROCESS_SET_DENOISE, &on);
    speex_preprocess_ctl(st_->pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noise);
    st_->ref_block.resize(static_cast<std::size_t>(st_->frame));
    st_->out_block.resize(static_cast<std::size_t>(st_->frame));
}

EchoCanceller::~EchoCanceller() {
    speex_preprocess_state_destroy(st_->pre);
    speex_echo_state_destroy(st_->echo);
}

void EchoCanceller::playback(std::span<const std::int16_t> mono) {
    std::scoped_lock lock(st_->mutex);
    st_->ref.insert(st_->ref.end(), mono.begin(), mono.end());
    // Nunca más de 30 ms acumulados: la referencia debe ir por delante del eco (lo que se reproduce tarda en salir
    // por el parlante y volver por el micrófono), así que se usa lo más reciente.
    const std::size_t cap = static_cast<std::size_t>(rate_ * 3 / 100);
    if (st_->ref.size() > cap) st_->ref.erase(st_->ref.begin(), st_->ref.begin() + static_cast<std::ptrdiff_t>(st_->ref.size() - cap));
}

void EchoCanceller::capture(std::span<const std::int16_t> mono, std::vector<std::int16_t>& out) {
    const auto n = static_cast<std::size_t>(st_->frame);
    st_->mic.insert(st_->mic.end(), mono.begin(), mono.end());
    std::size_t used = 0;
    while (st_->mic.size() - used >= n) {
        {
            std::scoped_lock lock(st_->mutex);
            // Sin referencia (nada sonando o el reproductor se atrasó): silencio.
            const std::size_t have = std::min(n, st_->ref.size());
            std::fill(st_->ref_block.begin(), st_->ref_block.end(), std::int16_t{0});
            std::copy_n(st_->ref.begin(), have, st_->ref_block.begin() + static_cast<std::ptrdiff_t>(n - have));
            st_->ref.erase(st_->ref.begin(), st_->ref.begin() + static_cast<std::ptrdiff_t>(have));
        }
        speex_echo_cancellation(st_->echo, st_->mic.data() + used, st_->ref_block.data(), st_->out_block.data());
        speex_preprocess_run(st_->pre, st_->out_block.data());
        out.insert(out.end(), st_->out_block.begin(), st_->out_block.end());
        used += n;
    }
    st_->mic.erase(st_->mic.begin(), st_->mic.begin() + static_cast<std::ptrdiff_t>(used));
}

std::optional<Device> virtual_mic_device() {
    platform::detail::ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return std::nullopt;
    std::wstring default_id;
    {
        ComPtr<IMMDevice> def;
        LPWSTR id = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && SUCCEEDED(def->GetId(&id))) {
            default_id = id;
            CoTaskMemFree(id);
        }
    }
    ComPtr<IMMDeviceCollection> list;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) return std::nullopt;
    UINT count = 0;
    list->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        ComPtr<IPropertyStore> props;
        if (FAILED(list->Item(i, &dev)) || FAILED(dev->OpenPropertyStore(STGM_READ, &props))) continue;
        PROPVARIANT v;
        PropVariantInit(&v);
        std::wstring name;
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v);
        std::wstring lower = name;
        for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
        if (lower.find(L"cable input") == std::wstring::npos && lower.find(L"voicemeeter input") == std::wstring::npos &&
            lower.find(L"voicemeeter vaio") == std::wstring::npos) {
            continue;
        }
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id))) continue;
        Device d{id, utf8(name), default_id == id};
        CoTaskMemFree(id);
        return d;
    }
    return std::nullopt;
}

}  // namespace lancast::audio
