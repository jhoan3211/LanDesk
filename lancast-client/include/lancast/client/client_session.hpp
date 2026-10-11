#pragma once

// Sesión de cliente: conexión y emparejamiento, canal de control cifrado, recepción UDP con FEC,
// sincronización de reloj, audio y archivos. La decodificación y presentación las hace la interfaz
// (mismo hilo que la swapchain) con los frames que entrega `take_frames()`.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lancast/core/media.hpp"
#include "lancast/core/protocol.hpp"
#include "lancast/core/settings.hpp"
#include "lancast/platform/audio.hpp"
#include "lancast/platform/files.hpp"
#include "lancast/platform/net.hpp"
#include "lancast/platform/secure_channel.hpp"

namespace lancast::client {

enum class State { Idle, Connecting, NeedPin, Authenticating, Connected, Failed, Closed };
[[nodiscard]] std::string_view to_string(State s) noexcept;

struct Stats {
    double fps = 0;              // Frames presentados por segundo.
    double bitrate_mbps = 0;
    double encode_ms = 0;        // Captura -> bitstream (host).
    double network_ms = 0;       // Bitstream listo -> frame completo en el cliente.
    double decode_ms = 0;
    double present_ms = 0;
    double total_ms = 0;         // Captura -> presentado.
    double rtt_ms = 0;
    double packet_loss_pct = 0;
    std::uint64_t frames_recovered = 0;
    std::uint64_t frames_lost = 0;
    std::uint64_t keyframe_requests = 0;
    double audio_buffer_ms = 0;
};

struct DiscoveredHost {
    std::string name;
    net::Endpoint endpoint;
    std::string id_hex;
    bool busy = false;
    bool paired = false;
    std::uint16_t web_port = 0;
};

// Busca hosts en la LAN por broadcast UDP.
[[nodiscard]] std::vector<DiscoveredHost> discover_hosts(std::uint32_t timeout_ms);

struct Frame {
    media::AssembledFrame frame;
    std::uint64_t arrival_us = 0;
};

class ClientSession {
public:
    explicit ClientSession(const AppSettings& settings);
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    void connect(const net::Endpoint& host, const proto::ClientCaps& caps, const proto::StreamConfig& initial);
    void submit_pin(const std::string& pin);
    void disconnect();

    [[nodiscard]] State state() const;
    [[nodiscard]] std::string message() const;
    [[nodiscard]] std::string host_name() const;
    [[nodiscard]] net::Endpoint host_endpoint() const;
    [[nodiscard]] std::optional<proto::HostInfo> host_info() const;
    [[nodiscard]] std::optional<proto::StreamStarted> stream() const;

    void request_stream(const proto::StreamConfig& config);
    void request_keyframe();
    // FrameLoss: el host repara con intra-refresh si el stream lo usa; NeedKeyframe: keyframe completo.
    void request_recovery(proto::RecoveryReason reason);
    void send_message(const std::vector<std::uint8_t>& encoded);

    // Frames completos en orden (todos: los P-frames dependen de los anteriores).
    [[nodiscard]] std::vector<Frame> take_frames();
    [[nodiscard]] void* frame_event() const noexcept { return frame_event_; }

    [[nodiscard]] std::optional<proto::CursorShape> take_cursor_shape();
    [[nodiscard]] proto::CursorPos cursor() const;
    [[nodiscard]] std::optional<std::string> take_clipboard();

    // La interfaz informa los tiempos de decodificación/presentación de cada frame.
    void report_presented(const media::EncodedFrameInfo& info, std::uint64_t arrival_us, double decode_ms,
                          double present_ms);
    [[nodiscard]] Stats stats() const;
    void set_simulated_loss(std::uint32_t percent) noexcept { simulated_loss_ = percent; }
    void set_audio_output(std::uint32_t volume_percent, std::uint32_t buffer_ms) noexcept {
        player_.set_volume(static_cast<float>(volume_percent) / 100.0f);
        player_.set_target_ms(buffer_ms);
    }

    files::Manager& files() noexcept { return files_; }

private:
    void control_thread(net::Endpoint host, proto::ClientCaps caps, proto::StreamConfig initial);
    bool handshake(net::TcpStream& stream, const net::Endpoint& host);
    void media_thread();
    void handle_video(std::span<const std::uint8_t> payload);
    void handle_audio(std::span<const std::uint8_t> payload);
    void fail(const std::string& msg);
    void set_state(State s, std::string msg = {});

    AppSettings settings_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    State state_ = State::Idle;
    std::string message_;
    std::string host_name_;
    net::Endpoint host_;
    std::optional<std::string> pin_;
    std::atomic<bool> stopping_{false};
    std::thread control_;
    std::thread media_;

    std::unique_ptr<net::SecureChannel> channel_;
    std::array<std::uint8_t, 32> media_rx_key_{};
    std::array<std::uint8_t, 32> media_tx_key_{};
    std::uint16_t media_port_ = 0;
    std::optional<proto::HostInfo> host_info_;
    std::optional<proto::StreamStarted> stream_;

    // Video
    media::FrameAssembler assembler_;
    media::SliceJoiner joiner_;
    std::uint64_t slice_frames_lost_ = 0;
    std::deque<Frame> frames_;
    void* frame_event_ = nullptr;
    bool waiting_keyframe_ = true;
    std::uint64_t last_idr_request_us_ = 0;
    std::uint64_t last_frame_us_ = 0;
    std::atomic<std::uint32_t> simulated_loss_{0};

    // Cursor y portapapeles
    std::optional<proto::CursorShape> cursor_shape_;
    proto::CursorPos cursor_pos_;
    std::optional<std::string> clipboard_;

    // Audio
    audio::Player player_;
    audio::Format player_format_{0, 0};

    // Reloj y estadísticas
    std::int64_t clock_offset_us_ = 0;  // host - cliente
    double best_rtt_us_ = 1e12;
    std::uint64_t best_rtt_time_us_ = 0;
    mutable Stats stats_;
    std::uint64_t stats_window_us_ = 0;
    std::uint64_t window_presented_ = 0;
    std::uint64_t window_bytes_ = 0;
    std::uint64_t window_packets_ = 0;
    std::uint64_t window_min_seq_ = ~0ull;
    std::uint64_t window_max_seq_ = 0;

    files::Manager files_;
};

}  // namespace lancast::client
