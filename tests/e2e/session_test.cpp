// Prueba de extremo a extremo: host y cliente reales sobre loopback (captura, codificación, FEC, cifrado,
// decodificación, emparejamiento con PIN, reconexión emparejada, archivos y rechazo de PIN incorrecto).

#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "lancast/client/client_session.hpp"
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
    std::printf("  Host: espera de captura %.2f ms, conversión+codificación %.2f ms, %.0f fps\n", hs.capture_wait_ms,
                hs.encode_ms, hs.fps);
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
