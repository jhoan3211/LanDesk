#include "lancast/host/host_service.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

#include "lancast/core/audio_resample.hpp"
#include "lancast/core/encoding.hpp"
#include "lancast/core/log.hpp"
#include "lancast/core/media.hpp"
#include "lancast/platform/audio.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/input.hpp"
#include "lancast/platform/marker_window.hpp"
#include "lancast/platform/media_sender.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/net.hpp"
#include "lancast/platform/platform.hpp"
#include "lancast/platform/service.hpp"
#include "lancast/platform/session_crypto.hpp"
#include "streamer.hpp"

namespace lancast::host {
namespace {

constexpr std::size_t kHandshakeMax = 8 * 1024;
constexpr int kMaxFailures = 5;
constexpr std::uint64_t kLockoutUs = 10ull * 60 * 1'000'000;

template <typename T>
std::optional<T> parse(std::span<const std::uint8_t> frame, proto::MsgType expected) {
    if (frame.empty() || static_cast<proto::MsgType>(frame[0]) != expected) return std::nullopt;
    ByteReader r(frame.subspan(1));
    return T::read(r);
}

std::array<std::uint8_t, 16> parse_device_id(const std::string& hex) {
    std::array<std::uint8_t, 16> id{};
    if (auto bytes = from_hex(hex); bytes && bytes->size() == 16) std::copy(bytes->begin(), bytes->end(), id.begin());
    return id;
}

}  // namespace

struct Session {
    std::string client_id;  // Id del equipo cliente (hex).
    std::unique_ptr<net::SecureChannel> channel;
    session::Keys keys;
    std::array<std::uint8_t, 16> session_id{};
    std::string client_name;
    net::Endpoint address;
    proto::ClientCaps caps;
    std::unique_ptr<net::MediaSender> media;
    std::unique_ptr<net::DatagramCipher> media_rx;
    std::unique_ptr<net::DatagramCipher> audio_cipher;
    std::vector<std::uint8_t> audio_scratch;
    std::uint32_t audio_seq = 0;
    audio::Resampler resampler;
    std::vector<std::int16_t> resampled;
    std::atomic<std::uint32_t> audio_target_rate{0};  // 0 = la del sistema.
    std::atomic<std::uint8_t> audio_target_channels{2};
    std::unique_ptr<Streamer> streamer;  // Stream principal.
    std::unique_ptr<Streamer> second;    // Segunda ventana del cliente (otro monitor), opcional.

    Streamer* streamer_for(std::uint8_t index) const { return index == 1 && second ? second.get() : streamer.get(); }
    audio::LoopbackCapture audio;
    std::atomic<bool> audio_enabled{false};
    input::Injector injector;
    std::atomic<bool> view_only{false};
    std::uint64_t started_us = 0;
    std::mutex clipboard_mutex;
    std::string last_remote_clipboard;
    std::unique_ptr<sys::MarkerWindow> latency_marker;  // Solo mientras el cliente mide la latencia.
};

struct HostService::Impl {
    std::atomic<bool> service_mode{false};
    HostService& owner;
    mutable std::mutex mutex;
    AppSettings settings;
    std::array<std::uint8_t, 16> device_id{};

    std::atomic<bool> running{false};
    net::TcpListener control_listener;
    net::TcpListener file_listener;
    net::UdpSocket media_socket;
    net::UdpSocket discovery_socket;
    std::thread accept_thread;
    std::thread file_thread;
    std::thread media_thread;
    std::thread discovery_thread;
    std::vector<std::thread> handlers;

    bool listening = false;
    std::string error;
    std::optional<std::string> pin;
    std::string pin_requester;
    std::atomic<bool> pairing_cancelled{false};
    std::map<std::uint32_t, std::pair<int, std::uint64_t>> failures;
    std::shared_ptr<Session> session;
    std::atomic<bool> handshake_in_progress{false};
    std::uint32_t pending_ip = 0;
    bool force_view_only = false;

    explicit Impl(HostService& o) : owner(o) {}

    std::shared_ptr<Session> current() {
        std::scoped_lock lock(mutex);
        return session;
    }

    HostSettings host_settings() {
        std::scoped_lock lock(mutex);
        return settings.host;
    }

    bool ip_allowed(const net::Endpoint& peer) {
        const HostSettings hs = host_settings();
        if (hs.private_networks_only && !peer.is_private()) return false;
        if (!hs.ip_whitelist.empty() && !peer.is_loopback()) {
            return std::ranges::any_of(hs.ip_whitelist, [&](const std::string& ip) {
                const auto e = net::Endpoint::parse(ip, 0);
                return e && e->ip == peer.ip;
            });
        }
        return true;
    }

    // ---------------------------------------------------------------- arranque / parada
    void start(const AppSettings& s) {
        stop();
        {
            std::scoped_lock lock(mutex);
            settings = s;
            device_id = parse_device_id(s.device_id);
            error.clear();
            listening = false;
        }
        owner.files_.set_receive_dir(s.host.receive_folder.empty() ? sys::default_receive_dir()
                                                                   : std::filesystem::path{platform_path(s.host.receive_folder)});
        owner.files_.set_overwrite(s.host.overwrite_existing);
        if (!s.host.enabled) return;

        auto ctl = net::TcpListener::bind(s.host.control_port);
        auto fil = net::TcpListener::bind(static_cast<std::uint16_t>(s.host.control_port + 1));
        auto med = net::UdpSocket::bind(s.host.media_port);
        if (!ctl || !fil || !med) {
            std::scoped_lock lock(mutex);
            error = !ctl ? ctl.error().message() : !fil ? fil.error().message() : med.error().message();
            log::error("Host: {}", error);
            return;
        }
        control_listener = std::move(*ctl);
        file_listener = std::move(*fil);
        media_socket = std::move(*med);
        media_socket.set_buffers(8 * 1024 * 1024);
        if (auto disc = net::UdpSocket::bind(kDiscoveryPort, true)) {
            discovery_socket = std::move(*disc);
        } else {
            log::warn("Descubrimiento desactivado: {}", disc.error().message());
        }

        running = true;
        accept_thread = std::thread([this] { accept_loop(); });
        file_thread = std::thread([this] { file_loop(); });
        media_thread = std::thread([this] { media_loop(); });
        if (discovery_socket.valid()) discovery_thread = std::thread([this] { discovery_loop(); });


        std::scoped_lock lock(mutex);
        listening = true;
        log::info("Host escuchando en TCP {} (control), {} (archivos), UDP {} (medios)", s.host.control_port,
                  s.host.control_port + 1, s.host.media_port);
    }

    static std::wstring platform_path(const std::string& utf8) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
        return w;
    }

    void stop() {
        running = false;
        pairing_cancelled = true;
        if (auto s = current()) s->channel->shutdown();
        control_listener.close();
        file_listener.close();
        media_socket.shutdown();
        media_socket.close();
        discovery_socket.shutdown();
        discovery_socket.close();
        for (std::thread* t : {&accept_thread, &file_thread, &media_thread, &discovery_thread}) {
            if (t->joinable()) t->join();
        }
        std::vector<std::thread> pending;
        {
            std::scoped_lock lock(mutex);
            pending.swap(handlers);
        }
        for (auto& t : pending) {
            if (t.joinable()) t.join();
        }
        owner.files_.detach();
        std::scoped_lock lock(mutex);
        listening = false;
        pairing_cancelled = false;
    }

    // ---------------------------------------------------------------- hilos de red
    void accept_loop() {
        while (running) {
            net::Endpoint peer;
            auto stream = control_listener.accept(250, &peer);
            if (!stream) continue;
            if (!ip_allowed(peer)) {
                log::warn("Conexión rechazada desde {} (fuera de la red privada / lista blanca)", peer.ip_string());
                continue;
            }
            std::scoped_lock lock(mutex);
            // Limpia hilos terminados.
            handlers.emplace_back([this, s = std::move(*stream), peer]() mutable { handle_connection(std::move(s), peer); });
        }
    }

    void file_loop() {
        while (running) {
            net::Endpoint peer;
            auto stream = file_listener.accept(250, &peer);
            if (!stream) continue;
            auto s = current();
            if (!s || s->address.ip != peer.ip || !host_settings().allow_files) continue;
            stream->set_recv_timeout(5000);
            std::vector<std::uint8_t> frame;
            if (!stream->recv_frame(frame, 64)) continue;
            const auto hello = parse<proto::FileHello>(frame, proto::MsgType::FileHello);
            if (!hello || hello->session_id != s->session_id) continue;
            stream->set_recv_timeout(0);
            owner.files_.attach(std::make_unique<net::SecureChannel>(std::move(*stream), s->keys.file_h2c, s->keys.file_c2h));
            log::info("Canal de archivos abierto con {}", peer.ip_string());
        }
    }

    void media_loop() {
        std::vector<std::uint8_t> buf(2048);
        std::vector<std::uint8_t> payload;
        while (running) {
            net::Endpoint from;
            const auto n = media_socket.recv_from(buf, from, 200);
            if (!n) break;
            if (*n == 0) continue;
            const auto header = media::PacketHeader::read({buf.data(), *n});
            if (!header || header->type != media::PacketType::ClientHello) continue;
            auto s = current();
            if (!s || !s->media_rx || from.ip != s->address.ip) continue;
            if (!s->media_rx->open({buf.data(), *n}, payload)) continue;
            const auto old = s->media->endpoint();
            if (!old || !(*old == from)) {
                s->media->set_endpoint(from);
                log::info("Medios: enviando a {}", from.to_string());
                if (s->streamer) s->streamer->request_keyframe();
                if (s->second) s->second->request_keyframe();
            }
        }
    }

    void discovery_loop() {
        std::vector<std::uint8_t> buf(512);
        while (running) {
            net::Endpoint from;
            const auto n = discovery_socket.recv_from(buf, from, 250);
            if (!n) break;
            if (*n < 5) continue;
            ByteReader r({buf.data(), *n});
            if (r.u32() != proto::kDiscoveryMagic || r.u8() != 0) continue;
            if (!ip_allowed(from)) continue;
            proto::DiscoveryReply reply;
            {
                std::scoped_lock lock(mutex);
                reply.host_name = settings.device_name;
                reply.device_id = device_id;
                reply.control_port = settings.host.control_port;
                reply.web_port = 0;  // Cliente web: pendiente (fase del celular).
                reply.busy = session != nullptr;
            }
            ByteWriter w;
            reply.write(w);
            discovery_socket.send_to(from, w.data());
        }
    }

    // ---------------------------------------------------------------- handshake y sesión
    void handle_connection(net::TcpStream stream, net::Endpoint peer) {
        stream.set_recv_timeout(10000);
        std::vector<std::uint8_t> hello_raw;
        if (!stream.recv_frame(hello_raw, kHandshakeMax)) return;
        const auto hello = parse<proto::Hello>(hello_raw, proto::MsgType::Hello);
        if (!hello) {
            log::warn("Handshake inválido desde {}", peer.ip_string());
            return;
        }
        if (hello->version != proto::kVersion) {
            proto::HelloAck reject;
            reject.accepted = false;
            reject.message = std::format("Versión de LanCast distinta (este equipo usa el protocolo {}, el otro el {}). "
                                         "Instala la misma versión en los dos PC.",
                                         proto::kVersion, hello->version);
            stream.send_frame(proto::encode(proto::MsgType::HelloAck, reject));
            log::warn("Conexión de {} con otra versión del protocolo ({})", peer.ip_string(), hello->version);
            return;
        }

        const std::string client_id = to_hex(hello->device_id);

        // El mismo equipo reintenta (p. ej. canceló el PIN): se descarta el emparejamiento anterior.
        bool retry_same_ip = false;
        {
            std::scoped_lock lock(mutex);
            retry_same_ip = handshake_in_progress && !session && pending_ip == peer.ip;
        }
        if (retry_same_ip) {
            pairing_cancelled = true;
            for (int i = 0; i < 100 && handshake_in_progress; ++i) Sleep(20);
        }

        crypto::Ecdh ecdh;
        proto::HelloAck ack;
        {
            std::scoped_lock lock(mutex);
            ack.device_id = device_id;
            ack.device_name = settings.device_name;
            const auto& f = failures[peer.ip];
            if (f.first >= kMaxFailures && gpu::now_us() - f.second < kLockoutUs) {
                ack.accepted = false;
                ack.message = "Demasiados intentos fallidos. Espera 10 minutos.";
            } else if ((session && session->client_id != client_id) || handshake_in_progress) {
                // Si es el mismo equipo que ya tiene sesión (se reconecta tras un corte), se le deja autenticarse:
                // la sesión vieja se reemplaza solo después de que demuestre su identidad.
                ack.accepted = false;
                ack.message = session ? std::format("{} ya tiene una sesión activa", settings.device_name)
                                      : "Hay otro emparejamiento en curso";
            }
        }
        const auto paired = sys::load_paired();
        const auto known = std::ranges::find_if(paired, [&](const sys::PairedDevice& d) {
            return d.role == "client" && d.id_hex == client_id;
        });
        ack.has_pairing = known != paired.end();
        ack.public_key = ecdh.public_key();
        crypto::random_bytes(ack.nonce);
        const auto ack_raw = proto::encode(proto::MsgType::HelloAck, ack);
        if (!stream.send_frame(ack_raw) || !ack.accepted) return;

        bool expected = false;
        if (!handshake_in_progress.compare_exchange_strong(expected, true)) return;
        {
            std::scoped_lock lock(mutex);
            pending_ip = peer.ip;
        }
        struct Guard {
            Impl& impl;
            bool active = true;
            void release() {
                if (!active) return;
                active = false;
                std::scoped_lock lock(impl.mutex);
                impl.pin.reset();
                impl.pin_requester.clear();
                impl.handshake_in_progress = false;
            }
            ~Guard() { release(); }
        } guard{*this};

        const crypto::Digest th = session::transcript(hello_raw, ack_raw);
        const auto z = ecdh.derive(hello->public_key);
        if (!z) return;

        std::vector<std::uint8_t> frame;
        if (!stream.recv_frame(frame, kHandshakeMax)) return;
        const auto start = parse<proto::AuthStart>(frame, proto::MsgType::AuthStart);
        if (!start) return;

        const bool use_pairing = start->use_pairing && ack.has_pairing;
        crypto::Key256 auth_key{};
        std::string pin_value;
        if (use_pairing) {
            auth_key = session::auth_key_from_secret(known->secret, th);
        } else {
            pin_value = session::random_pin();
            auth_key = session::auth_key_from_pin(pin_value, th);
            pairing_cancelled = false;
            std::scoped_lock lock(mutex);
            pin = pin_value;
            pin_requester = std::format("{} ({})", hello->device_name, peer.ip_string());
            log::info("Emparejamiento solicitado por {}: PIN {}", pin_requester, pin_value);
        }
        if (!stream.send_frame(proto::encode(proto::MsgType::AuthStartAck, proto::AuthStartAck{!use_pairing}))) return;

        // Hasta 2 minutos para que el usuario escriba el PIN en el cliente.
        stream.set_recv_timeout(use_pairing ? 10000 : 120000);
        std::thread watcher;
        std::atomic<bool> done{false};
        if (!use_pairing) {
            watcher = std::thread([&] {
                while (!done && running && !pairing_cancelled) Sleep(100);
                if (!done) stream.shutdown();
            });
        }
        const bool got = stream.recv_frame(frame, kHandshakeMax);
        done = true;
        if (watcher.joinable()) watcher.join();
        if (!got) return;
        const auto proof = parse<proto::AuthProof>(frame, proto::MsgType::AuthProof);
        if (!proof) return;

        const auto expected_proof = session::proof(auth_key, "client", th, *z);
        if (!crypto::equal_ct(expected_proof, proof->mac)) {
            {
                std::scoped_lock lock(mutex);
                auto& f = failures[peer.ip];
                if (gpu::now_us() - f.second > kLockoutUs) f.first = 0;
                ++f.first;
                f.second = gpu::now_us();
            }
            log::warn("Autenticación fallida desde {} ({})", peer.ip_string(), use_pairing ? "emparejamiento" : "PIN");
            stream.send_frame(proto::encode(proto::MsgType::AuthResult,
                                            proto::AuthResult{false, use_pairing ? "El emparejamiento no es válido. Vuelve a emparejar." : "PIN incorrecto", {}, {}}));
            return;
        }

        auto s = std::make_shared<Session>();
        s->keys = session::derive_keys(*z, auth_key, th);
        crypto::random_bytes(s->session_id);
        s->client_name = hello->device_name;
        s->client_id = client_id;
        s->address = peer;
        s->started_us = gpu::now_us();

        if (!use_pairing) {
            auto list = sys::load_paired();
            std::erase_if(list, [&](const sys::PairedDevice& d) { return d.role == "client" && d.id_hex == client_id; });
            list.push_back(sys::PairedDevice{client_id, hello->device_name, peer.ip_string(),
                                             {s->keys.pairing_secret.begin(), s->keys.pairing_secret.end()}, "client"});
            sys::save_paired(list);
            log::info("Emparejado con {}", hello->device_name);
        }
        proto::AuthResult result{true, "Conectado", session::proof(auth_key, "host", th, *z), s->session_id};
        if (!stream.send_frame(proto::encode(proto::MsgType::AuthResult, result))) return;

        stream.set_recv_timeout(15000);  // El cliente hace ping cada segundo.
        s->channel = std::make_unique<net::SecureChannel>(std::move(stream), s->keys.control_h2c, s->keys.control_c2h);
        s->media = std::make_unique<net::MediaSender>(media_socket, s->keys.media_h2c);
        s->media_rx = std::make_unique<net::DatagramCipher>(s->keys.media_c2h);
        // Reconexión del mismo equipo: cerrar la sesión anterior (quedó colgada por el corte de red).
        std::shared_ptr<Session> old;
        {
            std::scoped_lock lock(mutex);
            if (session && session->client_id == client_id) old = session;
        }
        if (old) {
            log::info("{} se reconectó: se reemplaza la sesión anterior", hello->device_name);
            old->channel->shutdown();
            for (int i = 0; i < 150 && current() == old; ++i) Sleep(20);
        }
        {
            std::scoped_lock lock(mutex);
            if (session) return;  // Otra sesión ganó la carrera.
            s->view_only = force_view_only;
            session = s;
        }
        guard.release();
        log::info("Sesión iniciada con {} ({})", s->client_name, peer.ip_string());
        run_session(*s);
        end_session(s);
    }

    void end_session(const std::shared_ptr<Session>& s) {
        if (s->second) s->second->stop();
        if (s->streamer) s->streamer->stop();
        s->audio.stop();
        s->injector.release_all();
        s->channel->shutdown();
        owner.files_.detach();
        {
            std::scoped_lock lock(mutex);
            if (session == s) session.reset();
        }
        log::info("Sesión con {} terminada", s->client_name);
    }

    void send_host_info(Session& s) {
        proto::HostInfo info;
        const HostSettings hs = host_settings();
        {
            std::scoped_lock lock(mutex);
            info.host_name = settings.device_name;
        }
        auto gpus = platform::enumerate_gpus();
        if (gpus) {
            if (auto monitors = platform::enumerate_monitors(*gpus)) {
                for (const auto& m : *monitors) {
                    info.monitors.push_back(proto::RemoteMonitor{m.id, m.friendly_name.empty() ? m.gdi_name : m.friendly_name,
                                                                 m.desktop,
                                                                 static_cast<std::uint32_t>(std::lround(m.refresh_rate.value())),
                                                                 m.primary, m.hdr_enabled});
                }
                const auto primary = std::ranges::find_if(*monitors, [](const MonitorInfo& m) { return m.primary; });
                if (primary != monitors->end() && primary->gpu_index && *primary->gpu_index < gpus->size()) {
                    info.encoders = mf::available_encoders((*gpus)[*primary->gpu_index].luid);
                }
            }
        }
        info.audio_available = hs.allow_audio;
        info.input_allowed = hs.allow_input && !s.view_only;
        info.clipboard_allowed = hs.allow_clipboard;
        info.files_allowed = hs.allow_files;
        info.secure_desktop = service_mode;
        info.media_port = hs.media_port;
        info.file_port = static_cast<std::uint16_t>(hs.control_port + 1);
        s.channel->send(proto::encode(proto::MsgType::HostInfo, info));
    }

    void on_audio(Session& s, std::span<const std::int16_t> pcm, std::uint32_t /*frames*/, const audio::Format& fmt) {
        if (!s.audio_enabled || !s.media->endpoint()) return;
        if (!s.audio_cipher) s.audio_cipher = s.media->make_cipher();
        // Frecuencia y canales elegidos por el cliente (se pueden cambiar en vivo).
        const std::uint32_t rate = s.audio_target_rate.load() != 0 ? s.audio_target_rate.load() : fmt.sample_rate;
        const std::uint8_t channels = s.audio_target_channels.load();
        s.resampler.configure(fmt.sample_rate, rate, channels);
        s.resampler.process(pcm, s.resampled);
        const auto frames = static_cast<std::uint32_t>(s.resampled.size() / channels);

        const std::uint32_t max_frames = 600 / channels;  // 1200 bytes de PCM por datagrama.
        const std::uint64_t now = gpu::now_us();
        for (std::uint32_t off = 0; off < frames; off += max_frames) {
            const std::uint32_t n = std::min(max_frames, frames - off);
            ByteWriter w(32 + n * channels * 2);
            w.u32(++s.audio_seq).u64(now).u32(rate).u8(channels).u16(static_cast<std::uint16_t>(n));
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(s.resampled.data() + static_cast<std::size_t>(off) * channels);
            w.raw({bytes, static_cast<std::size_t>(n) * channels * 2});
            s.media->send(*s.audio_cipher, media::PacketType::Audio, w.data(), s.audio_scratch);
        }
    }

    // Muestra un cuadro blanco/negro en la esquina del área transmitida y avisa cuándo cambió.
    void handle_latency_probe(Session& s, const proto::LatencyProbe& probe) {
        if (probe.id == 0) {
            s.latency_marker.reset();
            return;
        }
        const auto src = s.streamer ? s.streamer->source() : std::nullopt;
        if (!src) return;
        constexpr int kSize = 64;
        constexpr int kMargin = 24;
        if (!s.latency_marker) {
            s.latency_marker = std::make_unique<sys::MarkerWindow>(Rect{src->x + kMargin, src->y + kMargin, kSize, kSize});
        }
        proto::LatencyMark mark;
        mark.id = probe.id;
        mark.white = probe.white;
        mark.host_time_us = s.latency_marker->flash(probe.white);
        mark.x = static_cast<float>(kMargin + kSize / 2) / static_cast<float>(src->width);
        mark.y = static_cast<float>(kMargin + kSize / 2) / static_cast<float>(src->height);
        if (mark.host_time_us != 0) s.channel->send(proto::encode(proto::MsgType::LatencyMark, mark));
    }

    // Segunda ventana del cliente: otro monitor del host en paralelo (sin audio, mismo canal UDP).
    // ¿El monitor pedido (0 = todos) se solapa con lo que ya captura el stream principal?
    static bool overlaps_monitor(const Rect& main_src, std::uint32_t monitor_id) {
        if (monitor_id == 0) return true;
        const auto gpus = platform::enumerate_gpus();
        if (!gpus) return false;
        const auto monitors = platform::enumerate_monitors(*gpus);
        if (!monitors) return false;
        const auto it = std::ranges::find(*monitors, monitor_id, &MonitorInfo::id);
        if (it == monitors->end()) return false;
        const Rect& m = it->desktop;
        return m.x < main_src.right() && main_src.x < m.right() && m.y < main_src.bottom() && main_src.y < m.bottom();
    }

    void configure_second(Session& s, proto::StreamConfig c) {
        if (!c.enabled) {
            if (s.second) {
                s.second->stop();
                s.second.reset();
                log::info("Segundo stream detenido");
            }
            return;
        }
        c.audio = false;
        c.fec_percent = std::min<std::uint8_t>(c.fec_percent, 50);
        // Windows no deja duplicar dos veces la misma pantalla en un proceso: avisar con claridad.
        if (const auto main_src = s.streamer->source(); main_src && overlaps_monitor(*main_src, c.monitor_id)) {
            s.channel->send(proto::encode(proto::MsgType::StreamStopped,
                                          proto::StreamStoppedMsg{1, "Ese monitor ya se ve en la ventana principal; elige otro"}));
            return;
        }
        if (!s.second) {
            Streamer::Callbacks cb;
            cb.started = [&s](const proto::StreamStarted& info) {
                s.channel->send(proto::encode(proto::MsgType::StreamStarted, info));
            };
            cb.error = [&s](const std::string& msg) {
                s.channel->send(proto::encode(proto::MsgType::StreamStopped, proto::StreamStoppedMsg{1, msg}));
            };
            cb.cursor_pos = [&s](const proto::CursorPos& pos) {
                s.channel->send(proto::encode(proto::MsgType::CursorPos, pos));
            };
            s.second = std::make_unique<Streamer>(*s.media, std::move(cb), std::uint8_t{1});
            log::info("Segundo stream: monitor {}", c.monitor_id);
        }
        s.second->configure(c, s.caps, 0, 0);
    }

    void run_session(Session& s) {
        send_host_info(s);
        const HostSettings hs = host_settings();
        if (hs.allow_audio) {
            s.audio.start([this, &s](std::span<const std::int16_t> pcm, std::uint32_t frames, const audio::Format& fmt) {
                on_audio(s, pcm, frames, fmt);
            });
        }

        Streamer::Callbacks cb;
        cb.started = [&s](const proto::StreamStarted& info) { s.channel->send(proto::encode(proto::MsgType::StreamStarted, info)); };
        cb.error = [&s](const std::string& msg) {
            s.channel->send(proto::encode(proto::MsgType::StreamError, proto::TextMessage{msg}));
        };
        cb.cursor_shape = [&s](const proto::CursorShape& shape) { s.channel->send(proto::encode(proto::MsgType::CursorShape, shape)); };
        cb.cursor_pos = [&s](const proto::CursorPos& pos) { s.channel->send(proto::encode(proto::MsgType::CursorPos, pos)); };
        s.streamer = std::make_unique<Streamer>(*s.media, std::move(cb));

        std::vector<std::uint8_t> msg;
        while (running && s.channel->recv(msg, 4u * 1024 * 1024)) {
            if (msg.empty()) continue;
            const auto type = static_cast<proto::MsgType>(msg[0]);
            ByteReader r({msg.data() + 1, msg.size() - 1});
            const HostSettings cur = host_settings();
            const bool control = cur.allow_input && !s.view_only;
            switch (type) {
                case proto::MsgType::ClientCaps:
                    if (auto c = proto::ClientCaps::read(r)) s.caps = std::move(*c);
                    break;
                case proto::MsgType::StreamConfig:
                    if (auto c = proto::StreamConfig::read(r); c && c->stream_index == 1) {
                        configure_second(s, *c);
                    } else if (c) {
                        c->fec_percent = std::min<std::uint8_t>(c->fec_percent, 50);
                        s.audio_enabled = c->audio && cur.allow_audio;
                        if (!cur.allow_audio) c->audio = false;
                        const audio::Format fmt = s.audio.format();
                        const std::uint32_t rate = c->audio_rate == 0 ? fmt.sample_rate : std::clamp<std::uint32_t>(c->audio_rate, 8000, 192000);
                        s.audio_target_rate = c->audio_rate == 0 ? 0 : rate;
                        s.audio_target_channels = c->audio_channels == 1 ? 1 : 2;
                        s.streamer->configure(*c, s.caps, rate, s.audio_target_channels.load());
                    }
                    break;
                case proto::MsgType::RequestKeyframe: {
                    // Cuerpo opcional (1.3+): motivo. Sin cuerpo = el decodificador necesita un keyframe.
                    const auto reason = r.remaining() >= 1 ? static_cast<proto::RecoveryReason>(r.u8())
                                                           : proto::RecoveryReason::NeedKeyframe;
                    const std::uint8_t index = r.remaining() >= 1 ? r.u8() : 0;
                    s.streamer_for(index)->request_recovery(reason);
                    break;
                }
                case proto::MsgType::Ping:
                    if (auto p = proto::PingPong::read(r)) {
                        p->host_time_us = gpu::now_us();
                        s.channel->send(proto::encode(proto::MsgType::Pong, *p));
                    }
                    break;
                case proto::MsgType::MouseAbs:
                    if (auto m = proto::MouseAbs::read(r); m && control) {
                        if (const auto src = s.streamer_for(m->stream_index)->source()) {
                            s.injector.mouse_abs(normalized_to_desktop({m->x, m->y}, *src));
                        }
                    }
                    break;
                case proto::MsgType::MouseRel:
                    if (auto m = proto::MouseRel::read(r); m && control) s.injector.mouse_rel(m->dx, m->dy);
                    break;
                case proto::MsgType::MouseButton:
                    if (auto m = proto::MouseButton::read(r); m && control) s.injector.mouse_button(m->button, m->down);
                    break;
                case proto::MsgType::MouseWheel:
                    if (auto m = proto::MouseWheel::read(r); m && control) s.injector.mouse_wheel(m->delta, m->horizontal);
                    break;
                case proto::MsgType::Key:
                    if (auto k = proto::Key::read(r); k && control) s.injector.key(k->scancode, k->extended, k->down);
                    break;
                case proto::MsgType::ReleaseAll:
                    s.injector.release_all();
                    break;
                case proto::MsgType::Text:
                    if (auto t = proto::TextMessage::read(r); t && control && t->text.size() <= 4096) s.injector.text(t->text);
                    break;
                case proto::MsgType::SecureAttention:
                    if (!control) break;
                    if (service_mode && service::request_secure_attention()) {
                        log::info("Ctrl+Alt+Supr pedido por {}", s.client_name);
                    } else {
                        log::warn("Ctrl+Alt+Supr solo funciona con el host instalado como servicio");
                    }
                    break;
                case proto::MsgType::Clipboard:
                    if (auto t = proto::TextMessage::read(r); t && cur.allow_clipboard && owner.hwnd_ != nullptr) {
                        {
                            std::scoped_lock lock(s.clipboard_mutex);
                            s.last_remote_clipboard = t->text;
                        }
                        input::set_clipboard_text(owner.hwnd_, t->text);
                    }
                    break;
                case proto::MsgType::LatencyProbe:
                    if (auto probe = proto::LatencyProbe::read(r)) handle_latency_probe(s, *probe);
                    break;
                case proto::MsgType::Disconnect:
                    return;
                default:
                    break;
            }
        }
    }
};

HostService::HostService() : impl_(std::make_unique<Impl>(*this)) {}
HostService::~HostService() { impl_->stop(); }

void HostService::start(const AppSettings& settings) { impl_->start(settings); }
void HostService::stop() { impl_->stop(); }

void HostService::apply_settings(const AppSettings& settings) {
    bool restart = false;
    {
        std::scoped_lock lock(impl_->mutex);
        const HostSettings& old = impl_->settings.host;
        restart = old.enabled != settings.host.enabled || old.control_port != settings.host.control_port ||
                  old.media_port != settings.host.media_port ||
                  (settings.host.enabled && !impl_->listening);
        impl_->settings = settings;
    }
    files_.set_receive_dir(settings.host.receive_folder.empty()
                               ? sys::default_receive_dir()
                               : std::filesystem::path{Impl::platform_path(settings.host.receive_folder)});
    files_.set_overwrite(settings.host.overwrite_existing);
    if (restart) impl_->start(settings);
}

Status HostService::status() const {
    Status st;
    std::shared_ptr<Session> s;
    std::uint16_t port = 0;
    {
        std::scoped_lock lock(impl_->mutex);
        st.listening = impl_->listening;
        st.error = impl_->error;
        st.pin = impl_->pin;
        st.pin_requester = impl_->pin_requester;
        st.view_only = impl_->force_view_only;
        port = impl_->settings.host.control_port;
        s = impl_->session;
    }
    if (st.listening) {
        const auto addrs = net::local_addresses();
        const bool any_lan = std::ranges::any_of(addrs, [](const net::LocalAddress& a) { return a.has_gateway; });
        for (const auto& a : addrs) {
            if (!any_lan || a.has_gateway) st.addresses.push_back(net::Endpoint{a.ip, port}.ip_string());
        }
    }
    if (s) {
        st.session_active = true;
        st.client_name = s->client_name;
        st.client_address = s->address.ip_string();
        st.session_started_us = s->started_us;
        if (s->streamer) {
            st.stream = s->streamer->current();
            st.encoder_name = s->streamer->encoder_name();
            st.stats = s->streamer->stats();
        }
    }
    return st;
}

void HostService::disconnect() {
    if (auto s = impl_->current()) {
        s->channel->send(proto::encode(proto::MsgType::Disconnect, proto::TextMessage{"El host cerró la sesión"}));
        s->channel->shutdown();
    }
}

void HostService::set_service_mode(bool service_mode) { impl_->service_mode = service_mode; }

void HostService::set_view_only(bool view_only) {
    std::shared_ptr<Session> s;
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->force_view_only = view_only;
        s = impl_->session;
    }
    if (s) {
        s->view_only = view_only;
        if (view_only) s->injector.release_all();
        impl_->send_host_info(*s);
    }
}

void HostService::cancel_pairing() { impl_->pairing_cancelled = true; }

std::vector<sys::PairedDevice> HostService::paired_clients() const {
    auto all = sys::load_paired();
    std::erase_if(all, [](const sys::PairedDevice& d) { return d.role != "client"; });
    return all;
}

void HostService::revoke(const std::string& id_hex) {
    auto all = sys::load_paired();
    std::erase_if(all, [&](const sys::PairedDevice& d) { return d.role == "client" && d.id_hex == id_hex; });
    sys::save_paired(all);
}

void HostService::on_local_clipboard(const std::string& text) {
    auto s = impl_->current();
    if (!s || !impl_->host_settings().allow_clipboard) return;
    {
        std::scoped_lock lock(s->clipboard_mutex);
        if (text == s->last_remote_clipboard) return;  // Eco de lo que acabamos de recibir.
    }
    s->channel->send(proto::encode(proto::MsgType::Clipboard, proto::TextMessage{text}));
}

}  // namespace lancast::host
