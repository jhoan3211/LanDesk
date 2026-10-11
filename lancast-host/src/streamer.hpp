#pragma once

// Tubería de video del host: captura (DXGI) -> escalado/NV12 (procesador de video) -> codificador por
// hardware (MF) -> paquetización con FEC -> UDP cifrado. Corre en un hilo propio y se reconfigura en vivo.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "lancast/core/protocol.hpp"
#include "lancast/host/host_service.hpp"
#include "lancast/platform/media_sender.hpp"

namespace lancast::host {

class Streamer {
public:
    struct Callbacks {
        std::function<void(const proto::StreamStarted&)> started;
        std::function<void(const std::string&)> error;
        std::function<void(const proto::CursorShape&)> cursor_shape;
        std::function<void(const proto::CursorPos&)> cursor_pos;
    };

    Streamer(net::MediaSender& sender, Callbacks callbacks);
    ~Streamer();
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    void configure(const proto::StreamConfig& config, const std::vector<Codec>& client_decoders,
                   std::uint32_t audio_rate, std::uint8_t audio_channels);
    void request_keyframe() noexcept { keyframe_requested_ = true; }
    void stop();

    [[nodiscard]] StreamStats stats() const;
    [[nodiscard]] std::string encoder_name() const;
    // Rectángulo del escritorio virtual que se está transmitiendo (para mapear la entrada).
    [[nodiscard]] std::optional<Rect> source() const;
    [[nodiscard]] std::optional<proto::StreamStarted> current() const;

private:
    void run();

    net::MediaSender& sender_;
    Callbacks callbacks_;
    std::thread thread_;
    std::atomic<bool> running_{true};
    std::atomic<bool> keyframe_requested_{true};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<proto::StreamConfig> pending_;
    std::vector<Codec> client_decoders_;
    std::uint32_t audio_rate_ = 0;
    std::uint8_t audio_channels_ = 0;
    std::optional<proto::StreamStarted> current_;
    std::string encoder_name_;
    StreamStats stats_;
};

}  // namespace lancast::host
