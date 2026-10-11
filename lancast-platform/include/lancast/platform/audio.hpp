#pragma once

// Audio con WASAPI: captura loopback en el host y reproducción con buffer de jitter en el cliente.
// Se transmite PCM de 16 bits sin comprimir (1.5 Mbps en estéreo a 48 kHz): latencia de códec cero
// y CPU mínima, algo irrelevante para el ancho de banda de una LAN cableada.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
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

    void start(Callback callback);
    void stop();
    [[nodiscard]] Format format() const;

private:
    void run();

    Callback callback_;
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

    void start(const Format& format);
    void stop();
    // Encola muestras intercaladas int16 en el formato dado en start().
    void push(std::span<const std::int16_t> samples);
    [[nodiscard]] double buffered_ms() const;
    [[nodiscard]] std::uint64_t underruns() const noexcept { return underruns_.load(); }

private:
    void run();

    Format format_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::int16_t> ring_;
    std::size_t read_ = 0;
    std::size_t size_ = 0;
    bool primed_ = false;
    std::atomic<std::uint64_t> underruns_{0};
};

}  // namespace lancast::audio
