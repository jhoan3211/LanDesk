// Cliente real de LanCast (el del PC) para probar el host del teléfono (android/core, Kotlin/JVM).
//
//   lancast-test-client <IP> <puerto> <carpeta de datos>
//
// Imprime "NEED_PIN" y espera "PIN <6 dígitos>" por la entrada estándar; después "CONNECTED", la plataforma del host,
// los cuadros recibidos durante 4 s ("FRAMES <n> KEY <k> BYTES <b> CHECK <suma>") y manda control remoto
// ("INPUT_SENT"). Termina al cerrarse su entrada estándar.

#include <windows.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "lancast/client/client_session.hpp"
#include "lancast/core/encoding.hpp"
#include "lancast/platform/crypto.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/net.hpp"

using namespace lancast;

namespace {
void say(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "uso: lancast-test-client <IP> <puerto> <carpeta>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[3];
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"LANCAST_DATA_DIR", dir.c_str());
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    net::WinsockScope winsock;
    mf::MfScope mf;

    AppSettings s;
    std::array<std::uint8_t, 16> id{};
    crypto::random_bytes(id);
    s.device_id = to_hex(id);
    s.device_name = "PC-Interop";
    s.client.receive_folder = (dir / "recibidos").string();

    proto::ClientCaps caps;
    caps.client_name = "PC-Interop";
    caps.decoders = {Codec::H264, Codec::Hevc};
    caps.display_width = 2560;
    caps.display_height = 1440;
    caps.display_refresh = 165;
    proto::StreamConfig cfg;
    cfg.monitor_id = 1;
    cfg.max_width = 2560;
    cfg.max_height = 1440;
    cfg.fps = 165;
    cfg.bitrate_kbps = 20000;
    cfg.auto_bitrate = false;
    cfg.fec_percent = 15;

    const auto ep = net::Endpoint::parse(argv[1], static_cast<std::uint16_t>(std::stoi(argv[2])));
    if (!ep) {
        say("ERROR dirección inválida");
        return 2;
    }
    client::ClientSession session(s);
    session.connect(*ep, caps, cfg);

    std::uint64_t deadline = GetTickCount64() + 30000;
    bool pin_asked = false;
    while (session.state() != client::State::Connected && GetTickCount64() < deadline) {
        if (session.state() == client::State::NeedPin && !pin_asked) {
            pin_asked = true;
            say("NEED_PIN");
            std::string line;
            if (!std::getline(std::cin, line) || line.rfind("PIN ", 0) != 0) return 3;
            session.submit_pin(line.substr(4));
            deadline = GetTickCount64() + 30000;
        }
        if (session.state() == client::State::Failed || session.state() == client::State::Closed) {
            say("ERROR " + session.message());
            return 4;
        }
        Sleep(20);
    }
    if (session.state() != client::State::Connected) {
        say("ERROR no conectó: " + session.message());
        return 4;
    }
    say("CONNECTED");
    for (int i = 0; i < 100 && !session.host_info(); ++i) Sleep(20);
    if (const auto info = session.host_info()) {
        say("HOST " + info->host_name + " PLATFORM " + std::to_string(info->platform) + " SCREEN " +
            std::to_string(info->monitors.empty() ? 0 : info->monitors[0].rect.width) + "x" +
            std::to_string(info->monitors.empty() ? 0 : info->monitors[0].rect.height));
    }

    // Cuadros durante 4 s (suma simple del contenido para compararla con lo que mandó el host).
    std::uint64_t frames = 0;
    std::uint64_t keys = 0;
    std::uint64_t bytes = 0;
    std::uint64_t check = 0;
    const std::uint64_t until = GetTickCount64() + 4000;
    while (GetTickCount64() < until) {
        WaitForSingleObject(session.frame_event(0), 50);
        for (auto& f : session.take_frames(0)) {
            ++frames;
            if (f.frame.info.keyframe) ++keys;
            bytes += f.frame.data.size();
            for (std::uint8_t b : f.frame.data) check = check * 31 + b;
        }
    }
    say("FRAMES " + std::to_string(frames) + " KEY " + std::to_string(keys) + " BYTES " + std::to_string(bytes) + " CHECK " +
        std::to_string(check));

    session.send_message(proto::encode(proto::MsgType::MouseAbs, proto::MouseAbs{0.5f, 0.25f, 0}));
    session.send_message(proto::encode(proto::MsgType::MouseButton, proto::MouseButton{proto::MouseButtonId::Left, true}));
    session.send_message(proto::encode(proto::MsgType::MouseButton, proto::MouseButton{proto::MouseButtonId::Left, false}));
    session.send_message(proto::encode(proto::MsgType::Text, proto::TextMessage{"Desde el PC: ñandú"}));
    say("INPUT_SENT");

    std::string line;
    while (std::getline(std::cin, line)) {
    }
    session.disconnect();
    CoUninitialize();
    std::fflush(stdout);
    std::_Exit(0);
}
