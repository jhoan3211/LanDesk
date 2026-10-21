#pragma once

// Sesión de cliente: conexión y emparejamiento, canal de control cifrado, recepción UDP con FEC,
// sincronización de reloj, audio y archivos. La decodificación y presentación las hace la interfaz
// (mismo hilo que la swapchain) con los frames que entrega `take_frames()`.

#include <array>
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
    // Medición de latencia real (captura en el host -> imagen dibujada en el cliente).
    bool latency_test_running = false;
    int latency_samples = 0;
    double real_latency_ms = 0;      // Mediana de la última medición.
    double real_latency_min_ms = 0;
    double real_latency_max_ms = 0;
};

// Historial para los gráficos en tiempo real.
struct FrameSample {  // Un cuadro presentado.
    std::uint64_t time_us = 0;
    float encode_ms = 0;   // Captura -> bitstream (host).
    float network_ms = 0;  // Bitstream -> cuadro completo aquí.
    float decode_ms = 0;
    float present_ms = 0;
    float total_ms = 0;    // Captura -> presentado.
    std::uint32_t bytes = 0;
    bool keyframe = false;
};
struct NetSample {  // Una ventana de 250 ms.
    std::uint64_t time_us = 0;
    float mbps = 0;
    float loss_pct = 0;           // Paquetes perdidos en la red.
    std::uint32_t fec_recovered = 0;  // Cuadros reparados con la paridad.
    std::uint32_t frames_lost = 0;
    std::uint32_t keyframe_requests = 0;
    float rtt_ms = 0;
    float fps = 0;
};

// Lo que el cliente debe buscar en la imagen durante la medición de latencia.
struct LatencyTarget {
    float x = 0;
    float y = 0;
    bool white = false;
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
    // true si la sesión terminó por un problema de red (no por el usuario, el host o la autenticación):
    // la interfaz puede reconectar sola.
    [[nodiscard]] bool should_retry() const noexcept { return retryable_.load(); }
    [[nodiscard]] std::string message() const;
    [[nodiscard]] std::string host_name() const;
    [[nodiscard]] net::Endpoint host_endpoint() const;
    [[nodiscard]] std::optional<proto::HostInfo> host_info() const;
    // index 0 = stream principal, 1 = segunda ventana (otro monitor del host).
    [[nodiscard]] std::optional<proto::StreamStarted> stream(int index = 0) const;

    void request_stream(const proto::StreamConfig& config);  // config.stream_index elige el stream.
    // FEC automático: la paridad se ajusta según las pérdidas que ve el cliente (sin pérdidas baja hasta 5 %; con
    // pérdidas sube enseguida, hasta 40 %). Cambiarla no reinicia el codificador ni corta la imagen.
    void set_auto_fec(bool on) noexcept { auto_fec_ = on; }
    [[nodiscard]] int current_fec() const noexcept { return fec_current_.load(); }
    void stop_second_stream();
    [[nodiscard]] std::optional<std::string> take_second_stream_error();
    void request_keyframe(int index = 0);
    // FrameLoss: el host repara con intra-refresh si el stream lo usa; NeedKeyframe: keyframe completo.
    void request_recovery(proto::RecoveryReason reason, int index = 0);
    void send_message(const std::vector<std::uint8_t>& encoded);

    // Frames completos en orden (todos: los P-frames dependen de los anteriores).
    [[nodiscard]] std::vector<Frame> take_frames(int index = 0);
    [[nodiscard]] void* frame_event(int index = 0) const noexcept { return slots_[static_cast<std::size_t>(index)].event; }

    [[nodiscard]] std::optional<proto::CursorShape> take_cursor_shape();
    [[nodiscard]] proto::CursorPos cursor(int index = 0) const;
    [[nodiscard]] std::optional<std::string> take_clipboard();

    // La interfaz informa los tiempos de decodificación/presentación de cada frame.
    void report_presented(const media::EncodedFrameInfo& info, std::uint64_t arrival_us, double decode_ms,
                          double present_ms, std::size_t frame_bytes = 0);
    [[nodiscard]] Stats stats() const;
    // Últimos ~10 s para los gráficos (cuadros presentados y ventanas de red de 250 ms).
    [[nodiscard]] std::vector<FrameSample> frame_history() const;
    [[nodiscard]] std::vector<NetSample> net_history() const;

    // Medición de latencia real: el host alterna un marcador blanco/negro y el cliente detecta el cambio.
    void start_latency_test(int samples = 10);
    [[nodiscard]] std::optional<LatencyTarget> latency_target() const;
    void latency_observed(std::uint64_t present_us);  // La interfaz vio el color esperado en pantalla.
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
    void fail(const std::string& msg, bool retryable = false);
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
    std::atomic<bool> user_disconnect_{false};
    std::atomic<bool> retryable_{false};
    std::thread control_;
    std::thread media_;

    std::unique_ptr<net::SecureChannel> channel_;
    std::array<std::uint8_t, 32> media_rx_key_{};
    std::array<std::uint8_t, 32> media_tx_key_{};
    std::uint16_t media_port_ = 0;
    std::optional<proto::HostInfo> host_info_;

    // Video: un "slot" por stream (principal y segunda ventana). Campos protegidos por mutex_, salvo el
    // ensamblador y el unidor de slices, que solo usa el hilo de medios.
    struct StreamSlot {
        std::optional<proto::StreamStarted> info;
        media::FrameAssembler assembler;
        media::SliceJoiner joiner;
        std::uint64_t slice_frames_lost = 0;
        std::deque<Frame> frames;
        void* event = nullptr;
        bool waiting_keyframe = true;
        std::uint64_t last_idr_request_us = 0;
        std::uint64_t last_frame_us = 0;
        proto::CursorPos cursor;
    };
    std::array<StreamSlot, 2> slots_;
    static void reset_slot(StreamSlot& slot);
    std::optional<std::string> second_error_;
    std::atomic<std::uint32_t> simulated_loss_{0};

    // Cursor y portapapeles
    std::optional<proto::CursorShape> cursor_shape_;
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

    // Historial fino (gráficos): ventanas de 250 ms.
    std::deque<FrameSample> frame_history_;
    std::deque<NetSample> net_history_;
    std::uint64_t fine_start_us_ = 0;
    std::uint64_t fine_bytes_ = 0;
    std::uint64_t fine_packets_ = 0;
    std::uint64_t fine_presented_ = 0;
    std::uint64_t fine_min_seq_ = ~0ull;
    std::uint64_t fine_max_seq_ = 0;
    std::uint64_t fine_last_recovered_ = 0;
    std::uint64_t fine_last_lost_ = 0;
    std::uint64_t fine_last_idr_ = 0;
    // FEC automático (protegido por mutex_, salvo los atómicos).
    std::atomic<bool> auto_fec_{false};
    std::atomic<int> fec_current_{10};
    std::optional<proto::StreamConfig> last_config_;
    std::optional<proto::StreamConfig> fec_pending_;  // Se envía fuera del candado.
    std::uint64_t fec_clean_since_us_ = 0;
    void auto_fec_tick(const NetSample& s, std::uint64_t now);
    void tick_history(std::uint64_t now);  // Con mutex_ tomado.

    void send_next_probe();
    void finish_latency_test();

    // Medición de latencia (protegido por mutex_).
    bool latency_running_ = false;
    int latency_wanted_ = 0;
    std::uint32_t latency_probe_id_ = 0;
    bool latency_white_ = false;
    std::optional<proto::LatencyMark> latency_mark_;
    std::uint64_t latency_probe_sent_us_ = 0;
    std::vector<double> latency_samples_;
    int latency_failures_ = 0;

    files::Manager files_;
};

}  // namespace lancast::client
