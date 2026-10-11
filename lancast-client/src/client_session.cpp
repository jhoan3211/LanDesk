#include "lancast/client/client_session.hpp"

#include <windows.h>

#include <algorithm>
#include <format>
#include <random>

#include "lancast/core/encoding.hpp"
#include "lancast/core/log.hpp"
#include "lancast/platform/crypto.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/session_crypto.hpp"
#include "lancast/platform/system.hpp"

namespace lancast::client {
namespace {

constexpr std::size_t kHandshakeMax = 8 * 1024;

template <typename T>
std::optional<T> parse(std::span<const std::uint8_t> frame, proto::MsgType expected) {
    if (frame.empty() || static_cast<proto::MsgType>(frame[0]) != expected) return std::nullopt;
    ByteReader r(frame.subspan(1));
    return T::read(r);
}

double ema(double old, double sample, double alpha = 0.1) { return old == 0 ? sample : old * (1 - alpha) + sample * alpha; }

}  // namespace

std::string_view to_string(State s) noexcept {
    switch (s) {
        case State::Idle:
            return "Sin conexión";
        case State::Connecting:
            return "Conectando…";
        case State::NeedPin:
            return "Esperando PIN";
        case State::Authenticating:
            return "Autenticando…";
        case State::Connected:
            return "Conectado";
        case State::Failed:
            return "Error";
        case State::Closed:
            return "Desconectado";
    }
    return "?";
}

std::vector<DiscoveredHost> discover_hosts(std::uint32_t timeout_ms) {
    std::vector<DiscoveredHost> out;
    auto sock = net::UdpSocket::bind(0);
    if (!sock) return out;
    sock->set_broadcast(true);
    ByteWriter req;
    req.u32(proto::kDiscoveryMagic).u8(0);
    sock->send_to({0xFFFFFFFFu, kDiscoveryPort}, req.data());
    for (const auto& a : net::local_addresses()) {
        sock->send_to({a.broadcast, kDiscoveryPort}, req.data());
    }
    sock->send_to({0x7F000001u, kDiscoveryPort}, req.data());  // Host en este mismo equipo.

    const auto paired = sys::load_paired();
    const std::uint64_t deadline = gpu::now_us() + static_cast<std::uint64_t>(timeout_ms) * 1000;
    std::vector<std::uint8_t> buf(1024);
    while (gpu::now_us() < deadline) {
        net::Endpoint from;
        const auto n = sock->recv_from(buf, from, 50);
        if (!n) break;
        if (*n == 0) continue;
        ByteReader r({buf.data(), *n});
        const auto reply = proto::DiscoveryReply::read(r);
        if (!reply) continue;
        const std::string id = to_hex(reply->device_id);
        if (std::ranges::any_of(out, [&](const DiscoveredHost& h) { return h.id_hex == id; })) continue;
        DiscoveredHost h;
        h.name = reply->host_name;
        h.endpoint = net::Endpoint{from.ip, reply->control_port};
        h.id_hex = id;
        h.busy = reply->busy;
        h.web_port = reply->web_port;
        h.paired = std::ranges::any_of(paired, [&](const sys::PairedDevice& d) { return d.role == "host" && d.id_hex == id; });
        out.push_back(std::move(h));
    }
    // Si el host local respondió por loopback y por la LAN, se queda una sola entrada (la primera).
    return out;
}

ClientSession::ClientSession(const AppSettings& settings) : settings_(settings) {
    frame_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    files_.set_receive_dir(settings.client.receive_folder.empty() ? sys::default_receive_dir()
                                                                  : std::filesystem::path{std::u8string{
                                                                        settings.client.receive_folder.begin(),
                                                                        settings.client.receive_folder.end()}});
    simulated_loss_ = settings.client.simulate_loss_percent;
}

ClientSession::~ClientSession() {
    disconnect();
    if (frame_event_ != nullptr) CloseHandle(frame_event_);
}

void ClientSession::connect(const net::Endpoint& host, const proto::ClientCaps& caps, const proto::StreamConfig& initial) {
    disconnect();
    stopping_ = false;
    {
        std::scoped_lock lock(mutex_);
        host_ = host;
        state_ = State::Connecting;
        message_.clear();
        host_info_.reset();
        stream_.reset();
        pin_.reset();
        frames_.clear();
        assembler_.reset();
        waiting_keyframe_ = true;
        stats_ = Stats{};
        best_rtt_us_ = 1e12;
    }
    control_ = std::thread([this, host, caps, initial] { control_thread(host, caps, initial); });
}

void ClientSession::submit_pin(const std::string& pin) {
    std::scoped_lock lock(mutex_);
    pin_ = pin;
    cv_.notify_all();
}

void ClientSession::disconnect() {
    stopping_ = true;
    cv_.notify_all();
    {
        std::scoped_lock lock(mutex_);
        if (channel_) {
            channel_->send(proto::encode(proto::MsgType::Disconnect, proto::TextMessage{"El cliente se desconectó"}));
            channel_->shutdown();
        }
    }
    if (control_.joinable()) control_.join();
    if (media_.joinable()) media_.join();
    files_.detach();
    player_.stop();
    std::scoped_lock lock(mutex_);
    channel_.reset();
    if (state_ != State::Failed) state_ = state_ == State::Idle ? State::Idle : State::Closed;
}

State ClientSession::state() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

std::string ClientSession::message() const {
    std::scoped_lock lock(mutex_);
    return message_;
}

std::string ClientSession::host_name() const {
    std::scoped_lock lock(mutex_);
    return host_name_;
}

net::Endpoint ClientSession::host_endpoint() const {
    std::scoped_lock lock(mutex_);
    return host_;
}

std::optional<proto::HostInfo> ClientSession::host_info() const {
    std::scoped_lock lock(mutex_);
    return host_info_;
}

std::optional<proto::StreamStarted> ClientSession::stream() const {
    std::scoped_lock lock(mutex_);
    return stream_;
}

void ClientSession::set_state(State s, std::string msg) {
    std::scoped_lock lock(mutex_);
    state_ = s;
    if (!msg.empty() || s == State::Connected) message_ = std::move(msg);
}

void ClientSession::fail(const std::string& msg) {
    log::warn("Cliente: {}", msg);
    set_state(State::Failed, msg);
}

void ClientSession::send_message(const std::vector<std::uint8_t>& encoded) {
    std::scoped_lock lock(mutex_);
    if (channel_ && state_ == State::Connected) channel_->send(encoded);
}

void ClientSession::request_stream(const proto::StreamConfig& config) {
    send_message(proto::encode(proto::MsgType::StreamConfig, config));
}

void ClientSession::request_keyframe() { request_recovery(proto::RecoveryReason::NeedKeyframe); }

void ClientSession::request_recovery(proto::RecoveryReason reason) {
    const std::uint64_t now = gpu::now_us();
    // Una ola de intra-refresh dura unos pocos frames: no tiene sentido pedir otra enseguida.
    const std::uint64_t min_gap = reason == proto::RecoveryReason::FrameLoss ? 300'000 : 30'000;
    {
        std::scoped_lock lock(mutex_);
        if (now - last_idr_request_us_ < min_gap) return;
        last_idr_request_us_ = now;
        ++stats_.keyframe_requests;
    }
    send_message({static_cast<std::uint8_t>(proto::MsgType::RequestKeyframe), static_cast<std::uint8_t>(reason)});
}

bool ClientSession::handshake(net::TcpStream& stream, const net::Endpoint& host) {
    crypto::Ecdh ecdh;
    proto::Hello hello;
    if (auto id = from_hex(settings_.device_id); id && id->size() == 16) std::copy(id->begin(), id->end(), hello.device_id.begin());
    hello.device_name = settings_.device_name;
    hello.public_key = ecdh.public_key();
    crypto::random_bytes(hello.nonce);
    const auto hello_raw = proto::encode(proto::MsgType::Hello, hello);
    stream.set_recv_timeout(10000);
    if (!stream.send_frame(hello_raw)) {
        fail("No se pudo enviar el saludo al host");
        return false;
    }

    std::vector<std::uint8_t> ack_raw;
    if (!stream.recv_frame(ack_raw, kHandshakeMax)) {
        fail("El host no respondió (¿versión distinta de LanCast?)");
        return false;
    }
    const auto ack = parse<proto::HelloAck>(ack_raw, proto::MsgType::HelloAck);
    if (!ack) {
        fail("Respuesta inválida del host");
        return false;
    }
    {
        std::scoped_lock lock(mutex_);
        host_name_ = ack->device_name;
    }
    if (!ack->accepted) {
        fail(ack->message);
        return false;
    }
    if (ack->version != proto::kVersion) {
        fail("Versión de LanCast distinta en el otro equipo. Instala la misma versión en los dos PC.");
        return false;
    }

    const crypto::Digest th = session::transcript(hello_raw, ack_raw);
    const auto z = ecdh.derive(ack->public_key);
    if (!z) {
        fail("Clave del host inválida");
        return false;
    }

    const std::string host_id = to_hex(ack->device_id);
    auto paired = sys::load_paired();
    const auto known = std::ranges::find_if(paired, [&](const sys::PairedDevice& d) { return d.role == "host" && d.id_hex == host_id; });
    const bool use_pairing = ack->has_pairing && known != paired.end();
    stream.send_frame(proto::encode(proto::MsgType::AuthStart, proto::AuthStart{use_pairing}));

    std::vector<std::uint8_t> frame;
    if (!stream.recv_frame(frame, kHandshakeMax)) {
        fail("El host cerró la conexión");
        return false;
    }
    const auto start_ack = parse<proto::AuthStartAck>(frame, proto::MsgType::AuthStartAck);
    if (!start_ack) {
        fail("Respuesta inválida del host");
        return false;
    }

    crypto::Key256 auth_key{};
    if (start_ack->pin_required) {
        set_state(State::NeedPin, std::format("Escribe el PIN que muestra {}", ack->device_name));
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(115), [&] { return pin_.has_value() || stopping_.load(); }) || stopping_) {
            lock.unlock();
            if (!stopping_) fail("Tiempo de espera del PIN agotado");
            return false;
        }
        const std::string pin = *pin_;
        state_ = State::Authenticating;
        lock.unlock();
        auth_key = session::auth_key_from_pin(pin, th);
    } else {
        set_state(State::Authenticating);
        auth_key = session::auth_key_from_secret(known->secret, th);
    }

    stream.send_frame(proto::encode(proto::MsgType::AuthProof, proto::AuthProof{session::proof(auth_key, "client", th, *z)}));
    stream.set_recv_timeout(15000);
    if (!stream.recv_frame(frame, kHandshakeMax)) {
        fail("El host cerró la conexión durante la autenticación");
        return false;
    }
    const auto result = parse<proto::AuthResult>(frame, proto::MsgType::AuthResult);
    if (!result || !result->ok) {
        if (use_pairing) {
            // El host ya no reconoce el emparejamiento: se olvida para pedir PIN la próxima vez.
            std::erase_if(paired, [&](const sys::PairedDevice& d) { return d.role == "host" && d.id_hex == host_id; });
            sys::save_paired(paired);
        }
        fail(result ? result->message : "Autenticación rechazada");
        return false;
    }
    if (!crypto::equal_ct(result->mac, session::proof(auth_key, "host", th, *z))) {
        fail("El host no pudo demostrar su identidad (posible suplantación)");
        return false;
    }

    const session::Keys keys = session::derive_keys(*z, auth_key, th);
    paired = sys::load_paired();  // Releer: puede haber cambiado mientras se esperaba el PIN.
    if (start_ack->pin_required) {
        std::erase_if(paired, [&](const sys::PairedDevice& d) { return d.role == "host" && d.id_hex == host_id; });
        paired.push_back(sys::PairedDevice{host_id, ack->device_name, host.to_string(),
                                           {keys.pairing_secret.begin(), keys.pairing_secret.end()}, "host"});
        sys::save_paired(paired);
    } else {
        for (auto& d : paired) {
            if (d.role == "host" && d.id_hex == host_id && d.last_address != host.to_string()) {
                d.last_address = host.to_string();
                sys::save_paired(paired);
            }
        }
    }

    stream.set_recv_timeout(15000);
    {
        std::scoped_lock lock(mutex_);
        channel_ = std::make_unique<net::SecureChannel>(std::move(stream), keys.control_c2h, keys.control_h2c);
        media_rx_key_ = keys.media_h2c;
        media_tx_key_ = keys.media_c2h;
        state_ = State::Connected;
        message_.clear();
    }

    // Canal de archivos (TCP dedicado, puerto de control + 1).
    if (auto fs = net::TcpStream::connect({host.ip, static_cast<std::uint16_t>(host.port + 1)}, 3000)) {
        if (fs->send_frame(proto::encode(proto::MsgType::FileHello, proto::FileHello{result->session_id}))) {
            files_.attach(std::make_unique<net::SecureChannel>(std::move(*fs), keys.file_c2h, keys.file_h2c));
        }
    } else {
        log::warn("Canal de archivos no disponible: {}", fs.error().message());
    }
    return true;
}

void ClientSession::control_thread(net::Endpoint host, proto::ClientCaps caps, proto::StreamConfig initial) {
    auto stream = net::TcpStream::connect(host, 4000);
    if (!stream) {
        fail(std::format("No se pudo conectar a {}: {}", host.to_string(), stream.error().message()));
        return;
    }
    if (!handshake(*stream, host)) return;
    log::info("Conectado a {} ({})", host_name(), host.to_string());

    send_message(proto::encode(proto::MsgType::ClientCaps, caps));
    send_message(proto::encode(proto::MsgType::StreamConfig, initial));

    std::vector<std::uint8_t> msg;
    std::string close_reason;
    while (!stopping_) {
        net::SecureChannel* ch = nullptr;
        {
            std::scoped_lock lock(mutex_);
            ch = channel_.get();
        }
        if (ch == nullptr || !ch->recv(msg, 8u * 1024 * 1024)) break;
        if (msg.empty()) continue;
        ByteReader r({msg.data() + 1, msg.size() - 1});
        switch (static_cast<proto::MsgType>(msg[0])) {
            case proto::MsgType::HostInfo:
                if (auto h = proto::HostInfo::read(r)) {
                    const bool first = !host_info();
                    {
                        std::scoped_lock lock(mutex_);
                        media_port_ = h->media_port;
                        host_info_ = std::move(*h);
                    }
                    if (first) media_ = std::thread([this] { media_thread(); });
                }
                break;
            case proto::MsgType::StreamStarted:
                if (auto s = proto::StreamStarted::read(r)) {
                    log::info("Stream: {} {}x{} @ {} fps, {} Mbps{}", lancast::to_string(s->codec), s->width, s->height, s->fps,
                              s->bitrate_kbps / 1000, s->notes.empty() ? "" : " — " + s->notes);
                    const audio::Format fmt{s->audio_rate, s->audio_channels};
                    audio::Format old_fmt;
                    {
                        std::scoped_lock lock(mutex_);
                        old_fmt = player_format_;
                        player_format_ = audio::Format{0, 0};  // Pausa la entrada de audio mientras se reinicia.
                    }
                    if (fmt.sample_rate == 0) {
                        player_.stop();
                    } else if (fmt.sample_rate != old_fmt.sample_rate || fmt.channels != old_fmt.channels) {
                        player_.start(fmt);
                    }
                    std::scoped_lock lock(mutex_);
                    player_format_ = fmt;
                    stream_ = std::move(*s);
                    waiting_keyframe_ = true;
                    message_ = stream_->notes;
                }
                break;
            case proto::MsgType::StreamError:
                if (auto t = proto::TextMessage::read(r)) {
                    std::scoped_lock lock(mutex_);
                    message_ = "Error del host: " + t->text;
                }
                break;
            case proto::MsgType::Pong:
                if (auto p = proto::PingPong::read(r)) {
                    const std::uint64_t now = gpu::now_us();
                    const double rtt = static_cast<double>(now - p->client_time_us);
                    std::scoped_lock lock(mutex_);
                    stats_.rtt_ms = ema(stats_.rtt_ms, rtt / 1000.0, 0.3);
                    // NTP simplificado: se usa la muestra de menor RTT de los últimos 10 s.
                    if (rtt <= best_rtt_us_ || now - best_rtt_time_us_ > 10'000'000) {
                        best_rtt_us_ = rtt;
                        best_rtt_time_us_ = now;
                        clock_offset_us_ = static_cast<std::int64_t>(p->host_time_us) -
                                           static_cast<std::int64_t>(p->client_time_us + static_cast<std::uint64_t>(rtt / 2));
                    }
                }
                break;
            case proto::MsgType::CursorShape:
                if (auto c = proto::CursorShape::read(r)) {
                    std::scoped_lock lock(mutex_);
                    cursor_shape_ = std::move(*c);
                }
                break;
            case proto::MsgType::CursorPos:
                if (auto c = proto::CursorPos::read(r)) {
                    std::scoped_lock lock(mutex_);
                    cursor_pos_ = *c;
                }
                break;
            case proto::MsgType::Clipboard:
                if (auto t = proto::TextMessage::read(r)) {
                    std::scoped_lock lock(mutex_);
                    clipboard_ = t->text;
                }
                break;
            case proto::MsgType::LatencyMark:
                if (auto m = proto::LatencyMark::read(r)) {
                    std::scoped_lock lock(mutex_);
                    if (latency_running_ && m->id == latency_probe_id_) latency_mark_ = *m;
                }
                break;
            case proto::MsgType::Disconnect:
                if (auto t = proto::TextMessage::read(r)) close_reason = t->text;
                stopping_ = true;
                break;
            default:
                break;
        }
    }
    stopping_ = true;
    {
        std::scoped_lock lock(mutex_);
        if (state_ == State::Connected) {
            state_ = State::Closed;
            message_ = close_reason.empty() ? "Se perdió la conexión con el host" : close_reason;
        }
    }
    SetEvent(frame_event_);
}

void ClientSession::media_thread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    sys::RealtimeThread mmcss(L"Games", true);
    auto sock = net::UdpSocket::bind(0);
    if (!sock) {
        fail("No se pudo abrir un puerto UDP");
        return;
    }
    sock->set_buffers(16 * 1024 * 1024);
    net::Endpoint host_media;
    std::array<std::uint8_t, 32> rx_key{};
    std::array<std::uint8_t, 32> tx_key{};
    {
        std::scoped_lock lock(mutex_);
        host_media = net::Endpoint{host_.ip, media_port_};
        rx_key = media_rx_key_;
        tx_key = media_tx_key_;
    }
    net::DatagramCipher rx(rx_key);
    net::DatagramCipher tx(tx_key);
    std::uint64_t tx_seq = 1;
    std::vector<std::uint8_t> buf(2048);
    std::vector<std::uint8_t> payload;
    std::vector<std::uint8_t> sealed;
    std::uint64_t last_hello = 0;
    std::uint64_t last_ping = 0;
    std::mt19937 rng{std::random_device{}()};

    while (!stopping_) {
        const std::uint64_t now = gpu::now_us();
        if (now - last_hello > 500'000) {
            // Registra/renueva el endpoint UDP en el host (atraviesa el firewall del cliente: es tráfico saliente).
            std::uint8_t header[media::kPacketHeaderSize];
            media::PacketHeader{media::PacketType::ClientHello, tx_seq++}.write(header);
            const std::uint8_t hello_body[1] = {1};
            if (tx.seal(header, hello_body, sealed)) sock->send_to(host_media, sealed);
            last_hello = now;
        }
        if (now - last_ping > 1'000'000) {
            send_message(proto::encode(proto::MsgType::Ping, proto::PingPong{gpu::now_us(), 0}));
            last_ping = now;
        }
        // Medición de latencia: si la sonda expiró (marca de reintento), mandar otra o rendirse.
        bool retry = false;
        bool give_up = false;
        {
            std::scoped_lock lock(mutex_);
            // Si el cambio no se ve en 1 s (marcador tapado, otro monitor...), se reintenta.
            if (latency_running_ && static_cast<std::int64_t>(now - latency_probe_sent_us_) > 1'000'000) {
                give_up = ++latency_failures_ > 5;
                retry = !give_up;
            }
        }
        if (give_up) finish_latency_test();
        if (retry) send_next_probe();

        net::Endpoint from;
        const auto n = sock->recv_from(buf, from, 20);
        if (!n) break;
        if (*n == 0 || from.ip != host_media.ip) continue;
        const auto header = media::PacketHeader::read({buf.data(), *n});
        if (!header || !rx.open({buf.data(), *n}, payload)) continue;

        {
            std::scoped_lock lock(mutex_);
            ++window_packets_;
            window_bytes_ += *n;
            window_min_seq_ = std::min(window_min_seq_, header->seq);
            window_max_seq_ = std::max(window_max_seq_, header->seq);
        }
        if (header->type == media::PacketType::Video) {
            if (simulated_loss_ > 0 && std::uniform_int_distribution<std::uint32_t>(0, 99)(rng) < simulated_loss_) continue;
            handle_video(payload);
            // Vigilante: siguen llegando paquetes de video pero hace >250 ms que ningún frame se completa
            // (p. ej. se perdió el keyframe). Con la pantalla quieta el host casi no envía y eso es normal.
            const std::uint64_t t = gpu::now_us();
            if (last_frame_us_ != 0 && t - last_frame_us_ > 250'000) {
                std::uint64_t since_idr = 0;
                {
                    std::scoped_lock lock(mutex_);
                    since_idr = t - last_idr_request_us_;
                }
                if (since_idr > 250'000) request_keyframe();
            }
        } else if (header->type == media::PacketType::Audio) {
            handle_audio(payload);
        }
    }
}

void ClientSession::handle_video(std::span<const std::uint8_t> payload) {
    ByteReader r(payload);
    const auto h = media::VideoShardHeader::read(r);
    if (!h) return;
    std::optional<media::AssembledFrame> frame;
    if (auto unit = assembler_.add(*h, r.rest())) frame = joiner_.add(std::move(*unit));  // Une los slices.
    const std::uint32_t slice_lost = joiner_.take_lost();
    const std::uint32_t lost = assembler_.take_lost() + slice_lost;
    bool need_idr = false;
    bool need_refresh = false;
    {
        std::scoped_lock lock(mutex_);
        slice_frames_lost_ += slice_lost;
        if (lost > 0) {
            if (stream_ && stream_->intra_refresh && !waiting_keyframe_) {
                // Intra-refresh: se sigue mostrando video; la zona dañada se repara en pocos frames.
                need_refresh = true;
            } else {
                waiting_keyframe_ = true;
                need_idr = true;
            }
        }
        if (frame) {
            last_frame_us_ = gpu::now_us();
            if (stream_ && frame->info.epoch != stream_->epoch) {
                // Frame de otra configuración: se descarta hasta tener la nueva.
            } else if (waiting_keyframe_ && !frame->info.keyframe) {
                need_idr = true;
            } else {
                if (frame->info.keyframe) waiting_keyframe_ = false;
                frames_.push_back(Frame{std::move(*frame), last_frame_us_});
                if (frames_.size() > 120) frames_.pop_front();  // La interfaz no está consumiendo.
            }
        }
        stats_.frames_recovered = assembler_.stats().frames_recovered;
        stats_.frames_lost = assembler_.stats().frames_lost + slice_frames_lost_;
    }
    if (need_idr) {
        request_keyframe();
    } else if (need_refresh) {
        request_recovery(proto::RecoveryReason::FrameLoss);
    }
    if (frame) SetEvent(frame_event_);
}

void ClientSession::handle_audio(std::span<const std::uint8_t> payload) {
    ByteReader r(payload);
    (void)r.u32();  // secuencia
    (void)r.u64();  // tiempo de captura
    const std::uint32_t rate = r.u32();
    const std::uint8_t channels = r.u8();
    const std::uint16_t frames = r.u16();
    audio::Format fmt;
    {
        std::scoped_lock lock(mutex_);
        fmt = player_format_;
    }
    if (!r.ok() || channels == 0 || rate != fmt.sample_rate || channels != fmt.channels) return;
    const auto pcm = r.raw(static_cast<std::size_t>(frames) * channels * 2);
    if (pcm.size() != static_cast<std::size_t>(frames) * channels * 2) return;
    std::vector<std::int16_t> samples(static_cast<std::size_t>(frames) * channels);
    std::memcpy(samples.data(), pcm.data(), pcm.size());
    player_.push(samples);
}

std::vector<Frame> ClientSession::take_frames() {
    std::scoped_lock lock(mutex_);
    std::vector<Frame> out(std::make_move_iterator(frames_.begin()), std::make_move_iterator(frames_.end()));
    frames_.clear();
    return out;
}

std::optional<proto::CursorShape> ClientSession::take_cursor_shape() {
    std::scoped_lock lock(mutex_);
    auto out = std::move(cursor_shape_);
    cursor_shape_.reset();
    return out;
}

proto::CursorPos ClientSession::cursor() const {
    std::scoped_lock lock(mutex_);
    return cursor_pos_;
}

std::optional<std::string> ClientSession::take_clipboard() {
    std::scoped_lock lock(mutex_);
    auto out = std::move(clipboard_);
    clipboard_.reset();
    return out;
}

void ClientSession::report_presented(const media::EncodedFrameInfo& info, std::uint64_t arrival_us, double decode_ms,
                                     double present_ms) {
    const std::uint64_t now = gpu::now_us();
    std::scoped_lock lock(mutex_);
    const auto host_to_local = [&](std::uint64_t host_us) {
        return static_cast<double>(static_cast<std::int64_t>(host_us) - clock_offset_us_);
    };
    const double encoded_local = host_to_local(info.capture_time_us + info.encode_time_us);
    const double captured_local = host_to_local(info.capture_time_us);
    stats_.encode_ms = ema(stats_.encode_ms, info.encode_time_us / 1000.0);
    stats_.network_ms = ema(stats_.network_ms, std::max(0.0, (static_cast<double>(arrival_us) - encoded_local) / 1000.0));
    stats_.decode_ms = ema(stats_.decode_ms, decode_ms);
    stats_.present_ms = ema(stats_.present_ms, present_ms);
    stats_.total_ms = ema(stats_.total_ms, std::max(0.0, (static_cast<double>(now) - captured_local) / 1000.0));
    ++window_presented_;
}

Stats ClientSession::stats() const {
    const std::uint64_t now = gpu::now_us();
    std::scoped_lock lock(mutex_);
    auto& self = const_cast<ClientSession&>(*this);
    if (self.stats_window_us_ == 0) self.stats_window_us_ = now;
    const double secs = (now - self.stats_window_us_) / 1e6;
    if (secs >= 1.0) {
        stats_.fps = self.window_presented_ / secs;
        stats_.bitrate_mbps = self.window_bytes_ * 8.0 / secs / 1e6;
        if (self.window_max_seq_ >= self.window_min_seq_ && self.window_packets_ > 0) {
            const double expected = static_cast<double>(self.window_max_seq_ - self.window_min_seq_ + 1);
            stats_.packet_loss_pct = std::max(0.0, (expected - static_cast<double>(self.window_packets_)) * 100.0 / expected);
        } else {
            stats_.packet_loss_pct = 0;
        }
        self.stats_window_us_ = now;
        self.window_presented_ = 0;
        self.window_bytes_ = 0;
        self.window_packets_ = 0;
        self.window_min_seq_ = ~0ull;
        self.window_max_seq_ = 0;
    }
    stats_.audio_buffer_ms = self.player_.buffered_ms();
    return stats_;
}

void ClientSession::start_latency_test(int samples) {
    {
        std::scoped_lock lock(mutex_);
        if (state_ != State::Connected) return;
        latency_running_ = true;
        latency_wanted_ = samples;
        latency_samples_.clear();
        latency_failures_ = 0;
        stats_.latency_test_running = true;
        stats_.latency_samples = 0;
    }
    send_next_probe();
}

void ClientSession::send_next_probe() {
    proto::LatencyProbe probe;
    {
        std::scoped_lock lock(mutex_);
        latency_white_ = !latency_white_;
        probe.id = ++latency_probe_id_;
        if (probe.id == 0) probe.id = ++latency_probe_id_;
        probe.white = latency_white_;
        latency_mark_.reset();
        latency_probe_sent_us_ = gpu::now_us();
    }
    send_message(proto::encode(proto::MsgType::LatencyProbe, probe));
}

std::optional<LatencyTarget> ClientSession::latency_target() const {
    std::scoped_lock lock(mutex_);
    if (!latency_running_ || !latency_mark_) return std::nullopt;
    return LatencyTarget{latency_mark_->x, latency_mark_->y, latency_mark_->white};
}

void ClientSession::latency_observed(std::uint64_t present_us) {
    bool done = false;
    {
        std::scoped_lock lock(mutex_);
        if (!latency_running_ || !latency_mark_) return;
        const double host_local = static_cast<double>(static_cast<std::int64_t>(latency_mark_->host_time_us) - clock_offset_us_);
        const double ms = (static_cast<double>(present_us) - host_local) / 1000.0;
        if (ms > 0 && ms < 1000) latency_samples_.push_back(ms);
        latency_mark_.reset();
        stats_.latency_samples = static_cast<int>(latency_samples_.size());
        done = static_cast<int>(latency_samples_.size()) >= latency_wanted_;
    }
    if (done) {
        finish_latency_test();
    } else {
        send_next_probe();
    }
}

void ClientSession::finish_latency_test() {
    {
        std::scoped_lock lock(mutex_);
        latency_running_ = false;
        stats_.latency_test_running = false;
        if (!latency_samples_.empty()) {
            std::vector<double> v = latency_samples_;
            std::ranges::sort(v);
            stats_.real_latency_ms = v[v.size() / 2];
            stats_.real_latency_min_ms = v.front();
            stats_.real_latency_max_ms = v.back();
        }
    }
    send_message(proto::encode(proto::MsgType::LatencyProbe, proto::LatencyProbe{0, false}));  // Quitar el marcador.
    log::info("Latencia real medida: {:.1f} ms (mín {:.1f}, máx {:.1f})", stats_.real_latency_ms, stats_.real_latency_min_ms,
              stats_.real_latency_max_ms);
}

}  // namespace lancast::client
