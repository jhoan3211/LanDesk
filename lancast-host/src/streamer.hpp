#pragma once

// Tubería de video del host: captura (DXGI) -> escalado/NV12 (procesador de video) -> codificador por
// hardware (MF) -> paquetización con FEC -> UDP cifrado. Corre en un hilo propio y se reconfigura en vivo.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "lancast/core/protocol.hpp"
#include "lancast/host/host_service.hpp"
#include "lancast/core/media.hpp"
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

    Streamer(net::MediaSender& sender, Callbacks callbacks, std::uint8_t stream_index = 0);
    ~Streamer();
    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    void configure(const proto::StreamConfig& config, const std::vector<Codec>& client_decoders,
                   std::uint32_t audio_rate, std::uint8_t audio_channels, bool client_slices);
    void request_keyframe() noexcept { keyframe_requested_ = true; }
    // Pérdida de un cuadro: si el stream usa intra-refresh se repara con una ola de refresco; si no, keyframe.
    void request_recovery(proto::RecoveryReason reason) noexcept {
        if (reason == proto::RecoveryReason::FrameLoss) {
            refresh_requested_ = true;
        } else {
            keyframe_requested_ = true;
        }
    }
    void stop();

    [[nodiscard]] StreamStats stats() const;
    [[nodiscard]] std::string encoder_name() const;
    // Rectángulo del escritorio virtual que se está transmitiendo (para mapear la entrada).
    [[nodiscard]] std::optional<Rect> source() const;
    [[nodiscard]] std::optional<proto::StreamStarted> current() const;

private:
    void run();
    void sender_loop();

    struct SendJob {
        std::vector<std::uint8_t> data;
        media::EncodedFrameInfo info;
        int fec_percent = 0;
    };

    net::MediaSender& sender_;
    Callbacks callbacks_;
    std::uint8_t stream_index_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{true};
    std::atomic<bool> keyframe_requested_{true};
    std::atomic<bool> refresh_requested_{false};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<proto::StreamConfig> pending_;
    std::vector<Codec> client_decoders_;
    bool client_slices_ = false;
    std::uint32_t audio_rate_ = 0;
    std::uint8_t audio_channels_ = 0;
    std::optional<proto::StreamStarted> current_;
    std::string encoder_name_;
    StreamStats stats_;
    std::uint64_t stats_window_us_ = 0;  // Ventana de 1 s para las estadísticas (protegida por mutex_).
    std::uint64_t window_frames_ = 0;
    std::uint64_t window_bytes_ = 0;
    double window_latency_ms_ = 0;
    double window_wait_ms_ = 0;
    double window_encode_ms_ = 0;

    // Tubería: el hilo de captura/codificación entrega frames y este hilo los paquetiza, cifra y envía.
    std::thread send_thread_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::deque<SendJob> send_queue_;
};

}  // namespace lancast::host
