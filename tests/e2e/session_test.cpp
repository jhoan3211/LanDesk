// Prueba de extremo a extremo: host y cliente reales sobre loopback (captura, codificación, FEC, cifrado,
// decodificación, emparejamiento con PIN, reconexión emparejada, archivos y rechazo de PIN incorrecto).

#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "lancast/client/client_session.hpp"
#include "lancast/core/bitrate.hpp"
#include "lancast/core/encoding.hpp"
#include "lancast/core/log.hpp"
#include "lancast/host/host_service.hpp"
#include "lancast/platform/crypto.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/net.hpp"
#include "lancast/platform/platform.hpp"
#include "lancast/platform/system.hpp"

using namespace lancast;
namespace fs = std::filesystem;

namespace {

constexpr std::uint16_t kPort = 48984;  // Distinto del de la app real para no chocar si está abierta.

template <typename F>
bool wait_for(F&& cond, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 20) {
        if (cond()) return true;
        Sleep(20);
    }
    return cond();
}

AppSettings make_settings(const char* name, const fs::path& receive) {
    AppSettings s;
    std::array<std::uint8_t, 16> id{};
    crypto::random_bytes(id);
    s.device_id = to_hex(id);
    s.device_name = name;
    s.host.control_port = kPort;
    s.host.media_port = kPort + 14;
    s.host.web_enabled = false;
    s.host.receive_folder = receive.string();
    s.client.receive_folder = receive.string();
    return s;
}

proto::StreamConfig make_config() {
    proto::StreamConfig c;
    c.monitor_id = 0;  // Todos los monitores.
    c.max_width = 1280;
    c.max_height = 720;
    c.fps = 165;  // = Hz del monitor de pruebas: sin espera de turno
    c.codec = CodecChoice::Auto;
    c.audio = true;
    c.fec_percent = 15;
    return c;
}

proto::ClientCaps make_caps(const char* name) {
    proto::ClientCaps caps;
    caps.client_name = name;
    caps.decoders = mf::available_decoders();
    caps.supports_slices = true;
    caps.supports_444 = true;
    caps.supports_hdr = true;
    caps.display_width = 1920;
    caps.display_height = 1080;
    return caps;
}

class EndToEnd : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        platform::enable_per_monitor_dpi_awareness();
        root_ = fs::temp_directory_path() / L"lancast-e2e";
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_ / L"data");
        SetEnvironmentVariableW(L"LANCAST_DATA_DIR", (root_ / L"data").c_str());
        log::set_min_level(log::Level::Debug);
        log::set_file(root_ / L"e2e.log");
    }

    net::WinsockScope winsock_;
    mf::MfScope mf_;
    static fs::path root_;
};
fs::path EndToEnd::root_;

}  // namespace

TEST_F(EndToEnd, PairStreamTransferAndReconnect) {
    const AppSettings host_settings = make_settings("Host-Prueba", root_ / L"host-recv");
    const AppSettings client_settings = make_settings("Cliente-Prueba", root_ / L"client-recv");

    host::HostService host;
    host.start(host_settings);
    ASSERT_TRUE(wait_for([&] { return host.status().listening; }, 2000)) << host.status().error;

    // 1) Primera conexión: pide PIN.
    client::ClientSession client(client_settings);
    client.connect({0x7F000001, kPort}, make_caps("Cliente-Prueba"), make_config());
    ASSERT_TRUE(wait_for([&] { return client.state() == client::State::NeedPin; }, 5000)) << client.message();
    ASSERT_TRUE(wait_for([&] { return host.status().pin.has_value(); }, 2000));
    client.submit_pin(*host.status().pin);
    ASSERT_TRUE(wait_for([&] { return client.state() == client::State::Connected; }, 5000)) << client.message();
    ASSERT_TRUE(wait_for([&] { return client.stream().has_value(); }, 5000)) << client.message();
    EXPECT_TRUE(host.status().session_active);

    const auto stream = *client.stream();
    std::printf("  Stream: %s %ux%u @ %u fps, %u Mbps, audio %u Hz, %s%s%s\n", std::string{to_string(stream.codec)}.c_str(),
                stream.width, stream.height, stream.fps, stream.bitrate_kbps / 1000, stream.audio_rate,
                stream.encoder_backend.c_str(), stream.intra_refresh ? ", intra-refresh" : "", stream.slices ? ", slices" : "");
    EXPECT_LE(stream.width, 1280u);
    EXPECT_LE(stream.height, 720u);

    // 2) Recibir y decodificar video durante ~2 s.
    auto device = gpu::create_device(std::nullopt);
    ASSERT_TRUE(device.has_value());
    auto decoder = mf::create_decoder(*device, stream.codec, {stream.width, stream.height});
    ASSERT_TRUE(decoder.has_value()) << decoder.error().message();
    int frames = 0;
    int decoded = 0;
    bool got_key = false;
    const std::uint64_t until = gpu::now_us() + 2'000'000;
    while (gpu::now_us() < until) {
        WaitForSingleObject(client.frame_event(), 50);
        for (auto& f : client.take_frames()) {
            ++frames;
            got_key = got_key || f.frame.info.keyframe;
            if (!got_key) continue;
            auto out = (*decoder)->decode(f.frame.data, f.frame.info.capture_time_us);
            ASSERT_TRUE(out.has_value()) << out.error().message();
            if (out->has_value()) {
                ++decoded;
                client.report_presented(f.frame.info, f.arrival_us, 0, 0);
            }
        }
    }
    std::printf("  Frames recibidos: %d, decodificados: %d\n", frames, decoded);
    EXPECT_TRUE(got_key);
    EXPECT_GE(decoded, 1);  // Con el escritorio quieto el host solo manda frames de mantenimiento.
    const auto hs = host.status().stats;
    std::printf("  Host: espera de captura %.2f ms, conversión+codificación %.2f ms, %.0f fps, %.1f %% saltados\n",
                hs.capture_wait_ms, hs.encode_ms, hs.fps, hs.skipped_percent);
    EXPECT_LT(hs.skipped_percent, 10.0) << "A 720p el codificador debe dar abasto";
    const auto stats = client.stats();
    std::printf("  Latencia: codificación %.2f ms, red %.2f ms, total %.2f ms, RTT %.2f ms\n", stats.encode_ms,
                stats.network_ms, stats.total_ms, stats.rtt_ms);

    // 2b) 3 % de pérdida simulada: con FEC + intra-refresh el video sigue fluyendo.
    client.set_simulated_loss(3);
    int lossy_frames = 0;
    int lossy_decoded = 0;
    const auto requests_before = client.stats().keyframe_requests;
    const std::uint64_t lossy_until = gpu::now_us() + 1'500'000;
    while (gpu::now_us() < lossy_until) {
        WaitForSingleObject(client.frame_event(), 50);
        for (auto& f : client.take_frames()) {
            ++lossy_frames;
            auto out = (*decoder)->decode(f.frame.data, f.frame.info.capture_time_us);
            if (out && out->has_value()) ++lossy_decoded;
            if (!out) {
                std::printf("    error de decodificación: %s\n", out.error().message().c_str());
                break;
            }
        }
    }
    client.set_simulated_loss(0);
    const auto after = client.stats();
    std::printf("  Con 3%% de pérdida: %d frames, %d decodificados, recuperados por FEC %llu, perdidos %llu, pedidos %llu\n",
                lossy_frames, lossy_decoded, static_cast<unsigned long long>(after.frames_recovered),
                static_cast<unsigned long long>(after.frames_lost),
                static_cast<unsigned long long>(after.keyframe_requests - requests_before));
    EXPECT_GT(lossy_decoded, 0);

    // 3) Reconfiguración en vivo (sin reconectar).
    proto::StreamConfig cfg2 = make_config();
    cfg2.max_width = 960;
    cfg2.max_height = 540;
    cfg2.fps = 30;
    cfg2.codec = CodecChoice::H264;
    const std::uint64_t t_reconf = gpu::now_us();
    client.request_stream(cfg2);
    ASSERT_TRUE(wait_for([&] { return client.stream() && client.stream()->epoch != stream.epoch; }, 3000));
    const double reconf_ms = (gpu::now_us() - t_reconf) / 1000.0;
    std::printf("  Reconfiguración a %ux%u H.264 en %.0f ms\n", client.stream()->width, client.stream()->height, reconf_ms);
    EXPECT_EQ(client.stream()->codec, Codec::H264);
    EXPECT_LE(client.stream()->width, 960u);
    EXPECT_LT(reconf_ms, 500.0);

    // 3b) Cambio de audio en vivo (24 kHz mono): no debe reiniciar el video (misma época).
    const std::uint16_t epoch_before = client.stream()->epoch;
    proto::StreamConfig cfg3 = cfg2;
    cfg3.audio_rate = 24000;
    cfg3.audio_channels = 1;
    cfg3.bitrate_kbps = 15000;
    cfg3.auto_bitrate = false;
    client.request_stream(cfg3);
    ASSERT_TRUE(wait_for([&] { return client.stream() && client.stream()->audio_rate == 24000; }, 3000));
    EXPECT_EQ(client.stream()->audio_channels, 1);
    EXPECT_EQ(client.stream()->bitrate_kbps, 15000u);
    EXPECT_EQ(client.stream()->epoch, epoch_before) << "Un cambio de audio/bitrate no debe recrear el codificador";

    // 3a) Ritmo de envío: con un límite bajo cada cuadro tarda más en salir (sube "red") y sigue llegando completo.
    {
        auto measure_network = [&](std::uint32_t pace) {
            proto::StreamConfig paced = cfg3;
            paced.pacing_mbps = pace;
            paced.auto_bitrate = false;
            paced.bitrate_kbps = 20000;
            client.request_stream(paced);
            Sleep(600);
            // Con el escritorio quieto los cuadros son diminutos: se piden cuadros clave (grandes) para medir.
            int got = 0;
            const std::uint64_t start = gpu::now_us();
            std::uint64_t next_key = start;
            while (gpu::now_us() < start + 1'500'000) {
                if (gpu::now_us() >= next_key) {
                    client.request_keyframe();
                    next_key += 200'000;
                }
                WaitForSingleObject(client.frame_event(), 20);
                for (auto& f : client.take_frames()) {
                    client.report_presented(f.frame.info, f.arrival_us, 0, 0, f.frame.data.size());
                    ++got;
                }
            }
            double sum = 0;
            int n = 0;
            for (const auto& fs : client.frame_history()) {
                if (fs.time_us >= start && fs.bytes > 22'000) {  // Más de una tanda: el ritmo se nota.
                    sum += fs.network_ms;
                    ++n;
                }
            }
            return std::pair{got, n > 0 ? sum / n : 0.0};
        };
        const auto [free_frames, free_net] = measure_network(0);
        const auto [paced_frames, paced_net] = measure_network(40);
        std::printf("  Ritmo de envío: sin límite %.2f ms de red en cuadros grandes (%d cuadros) · a 40 Mbps %.2f ms (%d cuadros)\n", free_net,
                    free_frames, paced_net, paced_frames);
        EXPECT_GT(paced_frames, 5) << "Con ritmo el video debe seguir llegando";
        EXPECT_GT(paced_net, free_net + 1.0) << "Con 40 Mbps cada cuadro grande debe tardar más en salir";
        measure_network(0);
    }

    // 3b2) 4:4:4 + HDR: el host empaca el color completo (cuadro W×2H); HDR solo si su monitor está en modo HDR.
    {
        proto::StreamConfig c444 = cfg3;
        c444.chroma_444 = true;
        c444.hdr = true;
        c444.codec = CodecChoice::Hevc;
        const std::uint16_t before = client.stream()->epoch;
        client.request_stream(c444);
        ASSERT_TRUE(wait_for([&] { return client.stream() && client.stream()->epoch != before; }, 3000));
        const auto s444 = *client.stream();
        std::printf("  4:4:4%s: %s %ux%u (codificado %ux%u)%s\n", s444.hdr ? " HDR" : "", std::string{to_string(s444.codec)}.c_str(),
                    s444.width, s444.height, s444.width, s444.chroma_444 ? s444.height * 2 : s444.height,
                    s444.notes.empty() ? "" : (" — " + s444.notes).c_str());
        EXPECT_TRUE(s444.chroma_444);
        auto dec444 = mf::create_decoder(*device, s444.codec, {s444.width, s444.chroma_444 ? s444.height * 2 : s444.height}, s444.hdr);
        ASSERT_TRUE(dec444.has_value()) << dec444.error().message();
        int got444 = 0;
        bool key444 = false;
        const std::uint64_t until444 = gpu::now_us() + 1'500'000;
        while (gpu::now_us() < until444 && got444 < 3) {
            WaitForSingleObject(client.frame_event(), 50);
            for (auto& f : client.take_frames()) {
                if (f.frame.info.epoch != s444.epoch) continue;
                key444 = key444 || f.frame.info.keyframe;
                if (!key444) continue;
                auto out = (*dec444)->decode(f.frame.data, f.frame.info.capture_time_us);
                ASSERT_TRUE(out.has_value()) << out.error().message();
                if (out->has_value()) ++got444;
            }
            if (!key444) client.request_keyframe();
        }
        EXPECT_GE(got444, 1) << "No se decodificó ningún cuadro 4:4:4";
        client.request_stream(cfg3);  // Volver a 4:2:0 para el resto de la prueba.
        ASSERT_TRUE(wait_for([&] { return client.stream() && !client.stream()->chroma_444; }, 3000));
    }

    // 3c) Segundo stream en paralelo (índice 1, sin audio): llegan cuadros por separado y se detiene.
    {
        const std::uint16_t main_epoch = client.stream()->epoch;
        proto::StreamConfig second = cfg3;
        second.stream_index = 1;
        second.audio = false;
        second.max_width = 640;
        second.max_height = 360;
        client.request_stream(second);
        std::optional<std::string> second_error;
        ASSERT_TRUE(wait_for([&] {
            if (auto e = client.take_second_stream_error()) second_error = e;
            return second_error.has_value() || client.stream(1).has_value();
        }, 5000));
        if (second_error) {
            std::printf("  Segundo stream no disponible en este equipo: %s\n", second_error->c_str());
        } else {
            EXPECT_EQ(client.stream(1)->stream_index, 1);
            EXPECT_LE(client.stream(1)->width, 640u);
            int second_frames = 0;
            const std::uint64_t second_until = gpu::now_us() + 1'000'000;
            while (gpu::now_us() < second_until && second_frames == 0) {
                WaitForSingleObject(client.frame_event(1), 50);
                second_frames += static_cast<int>(client.take_frames(1).size());
            }
            std::printf("  Segundo stream: %ux%u, %d cuadros\n", client.stream(1)->width, client.stream(1)->height, second_frames);
            EXPECT_GT(second_frames, 0);
            client.stop_second_stream();
            ASSERT_TRUE(wait_for([&] { return !client.stream(1).has_value(); }, 2000));
        }
        EXPECT_EQ(client.stream()->epoch, main_epoch) << "El segundo stream no debe tocar el principal";
    }

    // 4) Transferencia de archivo (cliente -> host) con verificación.
    const fs::path src = root_ / L"origen.bin";
    {
        std::ofstream f(src, std::ios::binary);
        std::mt19937 rng(1);
        std::vector<char> block(1 << 20);
        for (int i = 0; i < 24; ++i) {
            for (auto& b : block) b = static_cast<char>(rng());
            f.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }
    ASSERT_TRUE(wait_for([&] { return client.files().connected(); }, 3000));
    const std::uint64_t t_file = gpu::now_us();
    client.files().send({src});
    ASSERT_TRUE(wait_for(
        [&] {
            const auto items = client.files().items();
            return !items.empty() && (items[0].state == files::State::Done || items[0].state == files::State::Failed);
        },
        20000));
    const auto item = client.files().items()[0];
    EXPECT_EQ(item.state, files::State::Done) << item.message;
    std::printf("  Archivo de 24 MB en %.0f ms\n", (gpu::now_us() - t_file) / 1000.0);
    const fs::path dst = root_ / L"host-recv" / L"origen.bin";
    ASSERT_TRUE(fs::exists(dst));
    EXPECT_EQ(fs::file_size(dst), fs::file_size(src));

    // Segundo envío del mismo nombre: no sobrescribe, crea "origen (1).bin".
    client.files().send({src});
    ASSERT_TRUE(wait_for([&] { return fs::exists(root_ / L"host-recv" / L"origen (1).bin") &&
                                      client.files().items().back().state == files::State::Done; },
                         20000));

    // 5) Desconectar y reconectar: ya emparejados, sin PIN.
    client.disconnect();
    ASSERT_TRUE(wait_for([&] { return !host.status().session_active; }, 5000));
    client::ClientSession again(client_settings);
    again.connect({0x7F000001, kPort}, make_caps("Cliente-Prueba"), make_config());
    ASSERT_TRUE(wait_for([&] { return again.state() == client::State::Connected || again.state() == client::State::NeedPin ||
                                      again.state() == client::State::Failed; },
                         5000));
    EXPECT_EQ(again.state(), client::State::Connected) << again.message();

    // 5b) El mismo equipo vuelve a conectar sin cerrar la sesión anterior (como tras un corte de red):
    // el host reemplaza la sesión vieja y la vieja queda "reintentable".
    client::ClientSession third(client_settings);
    third.connect({0x7F000001, kPort}, make_caps("Cliente-Prueba"), make_config());
    ASSERT_TRUE(wait_for([&] { return third.state() == client::State::Connected || third.state() == client::State::Failed; }, 8000));
    EXPECT_EQ(third.state(), client::State::Connected) << third.message();
    ASSERT_TRUE(wait_for([&] { return again.state() == client::State::Closed; }, 5000));
    EXPECT_TRUE(again.should_retry()) << again.message();
    third.disconnect();
    ASSERT_TRUE(wait_for([&] { return !host.status().session_active; }, 5000));
    EXPECT_FALSE(third.should_retry());  // Desconexión del usuario: no se reintenta.
    again.disconnect();
    ASSERT_TRUE(wait_for([&] { return !host.status().session_active; }, 5000));

    // 6) Otro equipo con PIN incorrecto: rechazado.
    const AppSettings intruder_settings = make_settings("Intruso", root_ / L"x");
    client::ClientSession intruder(intruder_settings);
    intruder.connect({0x7F000001, kPort}, make_caps("Intruso"), make_config());
    ASSERT_TRUE(wait_for([&] { return intruder.state() == client::State::NeedPin; }, 5000));
    const std::string pin = host.status().pin.value_or("000000");
    intruder.submit_pin(pin == "123456" ? "654321" : "123456");
    ASSERT_TRUE(wait_for([&] { return intruder.state() == client::State::Failed; }, 5000));
    EXPECT_NE(intruder.message().find("PIN"), std::string::npos) << intruder.message();
    EXPECT_FALSE(host.status().session_active);

    host.stop();
}

// El router (DHCP) le cambió la IP al host: el cliente lo encuentra por su id en la red y entra sin PIN.
TEST_F(EndToEnd, ReconnectsWhenHostIpChanged) {
    const AppSettings host_settings = make_settings("Host-IP", root_ / L"host-recv-ip");
    const AppSettings client_settings = make_settings("Cliente-IP", root_ / L"client-recv-ip");
    host::HostService host;
    host.start(host_settings);
    ASSERT_TRUE(wait_for([&] { return host.status().listening; }, 2000)) << host.status().error;
    // Si otra copia de LanCast ocupa el puerto de descubrimiento, este host no lo recibe: no se puede probar.
    bool visible = false;
    for (int i = 0; i < 3 && !visible; ++i) {
        for (const auto& h : client::discover_hosts(800)) visible = visible || h.id_hex == host_settings.device_id;
    }
    if (!visible) {
        host.stop();
        GTEST_SKIP() << "El descubrimiento no llega a este host de prueba (¿LanCast abierto en este PC?)";
    }

    {
        client::ClientSession first(client_settings);
        first.connect({0x7F000001, kPort}, make_caps("Cliente-IP"), make_config());
        ASSERT_TRUE(wait_for([&] { return first.state() == client::State::NeedPin; }, 5000)) << first.message();
        ASSERT_TRUE(wait_for([&] { return host.status().pin.has_value(); }, 2000));
        first.submit_pin(*host.status().pin);
        ASSERT_TRUE(wait_for([&] { return first.state() == client::State::Connected; }, 5000)) << first.message();
        first.disconnect();
    }
    ASSERT_TRUE(wait_for([&] { return !host.status().session_active; }, 5000));

    // La IP guardada deja de existir (192.0.2.x es una red de documentación: nadie responde).
    auto paired = sys::load_paired();
    for (auto& d : paired) {
        if (d.role == "host" && d.id_hex == host_settings.device_id) d.last_address = "192.0.2.123:" + std::to_string(kPort);
    }
    sys::save_paired(paired);

    const std::uint64_t t0 = gpu::now_us();
    client::ClientSession again(client_settings);
    again.connect({0xC000027B, kPort}, make_caps("Cliente-IP"), make_config());
    ASSERT_TRUE(wait_for([&] { return again.state() == client::State::Connected || again.state() == client::State::NeedPin ||
                                      again.state() == client::State::Failed; }, 15000));
    EXPECT_EQ(again.state(), client::State::Connected) << "Debe entrar sin PIN en la IP nueva: " << again.message();
    std::printf("  IP cambiada: encontrado por su id y conectado sin PIN en %.1f s\n", static_cast<double>(gpu::now_us() - t0) / 1e6);
    bool updated = false;
    for (const auto& d : sys::load_paired()) {
        if (d.role == "host" && d.id_hex == host_settings.device_id) updated = d.last_address.rfind("192.0.2.", 0) != 0;
    }
    EXPECT_TRUE(updated) << "Debe guardar la IP nueva";
    again.disconnect();
    host.stop();
}

// Calidad máxima (resolución nativa, 50 % más bitrate, codificador rápido) frente a lo normal: cuánto cambia la
// latencia del host y de la red. Solo informa (depende del codificador del equipo).
TEST_F(EndToEnd, QualityPresetLatency) {
    const AppSettings host_settings = make_settings("Host-Calidad", root_ / L"host-recv-q");
    const AppSettings client_settings = make_settings("Cliente-Calidad", root_ / L"client-recv-q");
    host::HostService host;
    host.start(host_settings);
    ASSERT_TRUE(wait_for([&] { return host.status().listening; }, 2000)) << host.status().error;
    client::ClientSession client(client_settings);
    client.connect({0x7F000001, kPort}, make_caps("Cliente-Calidad"), make_config());
    ASSERT_TRUE(wait_for([&] { return client.state() == client::State::NeedPin; }, 5000)) << client.message();
    ASSERT_TRUE(wait_for([&] { return host.status().pin.has_value(); }, 2000));
    client.submit_pin(*host.status().pin);
    ASSERT_TRUE(wait_for([&] { return client.state() == client::State::Connected && client.host_info(); }, 5000)) << client.message();
    const auto info = *client.host_info();
    const auto& mon = info.monitors.front();

    auto measure = [&](const char* label, proto::StreamConfig cfg) {
        client.request_stream(cfg);
        Sleep(800);
        const std::uint64_t start = gpu::now_us();
        std::uint64_t next_key = start;
        while (gpu::now_us() < start + 2'500'000) {
            if (gpu::now_us() >= next_key) {  // Cuadros completos de vez en cuando (el escritorio quieto da cuadros mínimos).
                client.request_keyframe();
                next_key += 250'000;
            }
            WaitForSingleObject(client.frame_event(), 20);
            for (auto& f : client.take_frames()) client.report_presented(f.frame.info, f.arrival_us, 0, 0, f.frame.data.size());
        }
        double enc = 0, net = 0, big_net = 0;
        int n = 0, big = 0;
        for (const auto& fs : client.frame_history()) {
            if (fs.time_us < start) continue;
            enc += fs.encode_ms;
            net += fs.network_ms;
            ++n;
            if (fs.keyframe) {
                big_net += fs.network_ms;
                ++big;
            }
        }
        const auto st = client.stream();
        std::printf("  %-16s %ux%u%s %s %u Mbps · host %.2f ms · red %.2f ms (keyframes %.2f ms) · %d cuadros\n", label,
                    st ? st->width : 0, st ? st->height : 0, st && st->chroma_444 ? " 4:4:4" : "", st ? std::string{to_string(st->codec)}.c_str() : "",
                    st ? st->bitrate_kbps / 1000 : 0, n ? enc / n : 0.0, n ? net / n : 0.0, big ? big_net / big : 0.0, n);
        return n ? enc / n : 0.0;
    };

    proto::StreamConfig normal = make_config();
    normal.monitor_id = mon.id;
    normal.max_width = 1920;
    normal.max_height = 1080;
    normal.fps = 60;
    normal.chroma_444 = false;
    normal.hdr = false;
    normal.auto_bitrate = true;
    proto::StreamConfig quality = normal;
    quality.max_width = static_cast<std::uint32_t>(mon.rect.width);
    quality.max_height = static_cast<std::uint32_t>(mon.rect.height);
    quality.chroma_444 = false;
    quality.auto_bitrate = false;
    quality.bitrate_kbps = std::clamp<std::uint32_t>(auto_bitrate_mbps(quality.max_width, quality.max_height, 60, Codec::Hevc) * 3 / 2, 40, 400) * 1000;
    const double a = measure("Normal", normal);
    const double b = measure("Calidad máxima", quality);
    proto::StreamConfig with444 = quality;
    with444.chroma_444 = true;
    with444.bitrate_kbps = std::clamp<std::uint32_t>(auto_bitrate_mbps(quality.max_width, quality.max_height * 2, 60, Codec::Hevc), 50, 400) * 1000;
    measure("+ 4:4:4", with444);
    std::printf("  Diferencia en el host: %+.2f ms por cuadro\n", b - a);
    EXPECT_GT(b, 0.0);
    client.disconnect();
    host.stop();
}

// FEC automático: con pérdidas sube solo y sin ellas baja; el cambio no corta la imagen (no espera un keyframe).
TEST_F(EndToEnd, AutoFecFollowsLoss) {
    const AppSettings host_settings = make_settings("Host-FEC", root_ / L"host-recv-fec");
    const AppSettings client_settings = make_settings("Cliente-FEC", root_ / L"client-recv-fec");
    host::HostService host;
    host.start(host_settings);
    ASSERT_TRUE(wait_for([&] { return host.status().listening; }, 2000)) << host.status().error;
    client::ClientSession client(client_settings);
    client.set_auto_fec(true);
    proto::StreamConfig cfg = make_config();
    cfg.max_width = 1280;
    cfg.max_height = 720;
    cfg.fps = 60;
    cfg.chroma_444 = false;
    cfg.hdr = false;
    client.connect({0x7F000001, kPort}, make_caps("Cliente-FEC"), cfg);
    ASSERT_TRUE(wait_for([&] { return client.state() == client::State::NeedPin; }, 5000)) << client.message();
    ASSERT_TRUE(wait_for([&] { return host.status().pin.has_value(); }, 2000));
    client.submit_pin(*host.status().pin);
    ASSERT_TRUE(wait_for([&] { return client.state() == client::State::Connected && client.stream(); }, 5000)) << client.message();
    EXPECT_EQ(client.current_fec(), 10) << "Arranca en 10 %";

    // Consume cuadros y mide el mayor hueco entre cuadros completos.
    std::uint64_t last_frame = 0;
    std::uint64_t worst_gap = 0;
    std::uint64_t next_key = 0;
    auto pump = [&](int ms) {
        const std::uint64_t until = gpu::now_us() + static_cast<std::uint64_t>(ms) * 1000;
        while (gpu::now_us() < until) {
            if (gpu::now_us() >= next_key) {  // Cuadros grandes de vez en cuando: más paquetes, las pérdidas se notan.
                client.request_keyframe();
                next_key = gpu::now_us() + 300'000;
            }
            WaitForSingleObject(client.frame_event(), 20);
            for (auto& f : client.take_frames()) {
                client.report_presented(f.frame.info, f.arrival_us, 0, 0, f.frame.data.size());
                const std::uint64_t now = gpu::now_us();
                if (last_frame != 0) worst_gap = std::max(worst_gap, now - last_frame);
                last_frame = now;
            }
        }
    };
    pump(1500);
    client.set_simulated_loss(5);
    const std::uint64_t t0 = gpu::now_us();
    while (client.current_fec() <= 10 && gpu::now_us() - t0 < 5'000'000) pump(100);
    const int raised = client.current_fec();
    std::printf("  Con 5 %% de pérdida simulada el FEC subió a %d %% en %.1f s\n", raised, static_cast<double>(gpu::now_us() - t0) / 1e6);
    EXPECT_GE(raised, 20);
    client.set_simulated_loss(0);
    pump(1000);
    worst_gap = 0;
    const int before_down = client.current_fec();
    // 15 s sin pérdidas: baja un escalón (margen por si el primer tramo arrancó con algún tropiezo).
    const std::uint64_t t1 = gpu::now_us();
    while (client.current_fec() >= before_down && gpu::now_us() - t1 < 35'000'000) pump(500);
    std::printf("  Sin pérdidas: %d %% -> %d %%; mayor hueco entre cuadros %.0f ms\n", before_down, client.current_fec(),
                static_cast<double>(worst_gap) / 1000.0);
    EXPECT_LT(client.current_fec(), before_down) << "Sin pérdidas el FEC debe bajar";
    EXPECT_LT(worst_gap, 300'000u) << "Cambiar el FEC no debe cortar la imagen";
    client.disconnect();
    host.stop();
}

// Calibración: el host mide su codificador al arrancar (sin sesión) y lo informa en HostInfo.
TEST_F(EndToEnd, HostReportsEncoderCalibration) {
    const AppSettings host_settings = make_settings("Host-Calib", root_ / L"host-recv-cal");
    const AppSettings client_settings = make_settings("Cliente-Calib", root_ / L"client-recv-cal");
    host::HostService host;
    host.start(host_settings);
    ASSERT_TRUE(wait_for([&] { return host.status().listening; }, 2000)) << host.status().error;
    ASSERT_TRUE(wait_for([&] { return host.status().encode_420_ms > 0; }, 20000)) << "El host no midió su codificador";
    const auto st = host.status();
    std::printf("  Host: codificador a %ux%u: %.2f ms (4:4:4 %.2f ms)\n", st.calibrated_size.width, st.calibrated_size.height,
                st.encode_420_ms, st.encode_444_ms);
    client::ClientSession client(client_settings);
    client.connect({0x7F000001, kPort}, make_caps("Cliente-Calib"), make_config());
    ASSERT_TRUE(wait_for([&] { return client.state() == client::State::NeedPin; }, 5000)) << client.message();
    ASSERT_TRUE(wait_for([&] { return host.status().pin.has_value(); }, 2000));
    client.submit_pin(*host.status().pin);
    ASSERT_TRUE(wait_for([&] { return client.state() == client::State::Connected && client.host_info(); }, 5000)) << client.message();
    const auto info = *client.host_info();
    EXPECT_NEAR(info.encode_420_cms / 100.0, st.encode_420_ms, 0.02);
    EXPECT_NEAR(info.encode_444_cms / 100.0, st.encode_444_ms, 0.02);
    client.disconnect();
    host.stop();
}
