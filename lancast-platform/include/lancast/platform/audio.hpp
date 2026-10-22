#pragma once

// Audio con WASAPI: captura loopback en el host y reproducción con buffer de jitter en el cliente.
// Se transmite PCM de 16 bits sin comprimir (1.5 Mbps en estéreo a 48 kHz): latencia de códec cero
// y CPU mínima, algo irrelevante para el ancho de banda de una LAN cableada.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace lancast::audio {

struct Format {
    std::uint32_t sample_rate = 48000;
    std::uint8_t channels = 2;
};

class LoopbackCapture {
public:
    // callback(muestras intercaladas int16, frames, formato). Se llama desde el hilo de captura cada ~5 ms.
    using Callback = std::function<void(std::span<const std::int16_t>, std::uint32_t, const Format&)>;

    LoopbackCapture() = default;
    ~LoopbackCapture() { stop(); }
    LoopbackCapture(const LoopbackCapture&) = delete;
    LoopbackCapture& operator=(const LoopbackCapture&) = delete;

    enum class Source { Loopback, Microphone };
    // Loopback: lo que suena en el equipo. Microphone: el micrófono predeterminado (de comunicaciones).
    void start(Callback callback, Source source = Source::Loopback);
    void stop();
    [[nodiscard]] Format format() const;
    // Micrófono: ¿Windows le aplica cancelación de eco (modo comunicaciones con referencia en la salida
    // predeterminada, que es donde suena el audio del host)? Se sabe al abrir el dispositivo.
    [[nodiscard]] bool echo_cancellation() const noexcept { return echo_cancellation_; }
    // Solo pruebas: micrófono sin procesar (sin modo comunicaciones), para comparar.
    void set_raw_microphone(bool raw) noexcept { raw_ = raw; }

private:
    void run();
    std::atomic<bool> echo_cancellation_{false};
    std::atomic<bool> raw_{false};

    Callback callback_;
    Source source_ = Source::Loopback;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    Format format_;
};

class Player {
public:
    Player() = default;
    ~Player() { stop(); }
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // device_id vacío = la salida predeterminada.
    void start(const Format& format, std::wstring device_id = {});
    void stop();
    // Encola muestras intercaladas int16 en el formato dado en start().
    void push(std::span<const std::int16_t> samples);
    [[nodiscard]] double buffered_ms() const;
    // Volumen 0..1 y colchón objetivo (ms): se aplican al instante.
    void set_volume(float volume) noexcept { volume_ = volume; }
    void set_target_ms(std::uint32_t ms) noexcept { target_ms_ = ms; }
    [[nodiscard]] std::uint64_t underruns() const noexcept { return underruns_.load(); }
    // Recibe exactamente lo que se entrega a la salida (con volumen y silencios), desde el hilo de reproducción:
    // es la referencia del cancelador de eco.
    using Tap = std::function<void(std::span<const std::int16_t>, const Format&)>;
    void set_tap(Tap tap) {
        std::scoped_lock lock(tap_mutex_);
        tap_ = std::move(tap);
    }

private:
    std::mutex tap_mutex_;
    Tap tap_;
    void run();

    Format format_;
    std::wstring device_id_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::int16_t> ring_;
    std::size_t read_ = 0;
    std::size_t size_ = 0;
    bool primed_ = false;
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<float> volume_{1.0f};
    std::atomic<std::uint32_t> target_ms_{20};
};

// Cancelación de eco acústico (SpeexDSP): resta del micrófono lo que sonó por los parlantes, con un eco de hasta
// `tail_ms`. Todo mono a la misma frecuencia; trabaja en bloques de 10 ms (agrega hasta 10 ms).
class EchoCanceller {
public:
    explicit EchoCanceller(std::uint32_t rate, std::uint32_t tail_ms = 200);
    ~EchoCanceller();
    EchoCanceller(const EchoCanceller&) = delete;
    EchoCanceller& operator=(const EchoCanceller&) = delete;
    [[nodiscard]] std::uint32_t rate() const noexcept { return rate_; }
    // Lo que acaba de entregarse a los parlantes (cualquier hilo).
    void playback(std::span<const std::int16_t> mono);
    // Micrófono: agrega a `out` los bloques ya limpios (puede quedar un resto para la próxima llamada).
    void capture(std::span<const std::int16_t> mono, std::vector<std::int16_t>& out);

private:
    struct State;
    std::unique_ptr<State> st_;
    std::uint32_t rate_;
};

struct Device {
    std::wstring id;
    std::string name;
    // Es la salida predeterminada: lo que se reproduce ahí lo captura el audio del host y volvería al cliente (eco).
    bool default_output = false;
};
// Entrada de un cable de audio virtual (VB-Cable "CABLE Input", VoiceMeeter): lo que se reproduce ahí lo ven las
// demás aplicaciones como un micrófono. Así el micrófono del cliente llega a Discord, Teams o un juego en el host.
[[nodiscard]] std::optional<Device> virtual_mic_device();

}  // namespace lancast::audio
