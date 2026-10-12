// Host de LanCast para pruebas de interoperabilidad (cliente Android/Kotlin contra el host real).
//
//   lancast-test-host <puerto_control> <carpeta_de_datos> [carpeta_de_datos_de_un_cliente]
//
// Con la carpeta de un cliente, deja a ese cliente ya emparejado (escribe el mismo secreto en los dos lados), para
// abrir la interfaz de prueba sin escribir el PIN.
//
// Imprime "READY" cuando acepta conexiones y "PIN <6 dígitos>" cuando un cliente pide emparejarse. El control
// remoto está desactivado (no mueve el mouse ni escribe en el equipo). Termina al cerrarse su entrada estándar.
//
// Archivos: la línea "SEND <ruta>" en la entrada estándar envía ese archivo al cliente; al recibir uno imprime
// "RECEIVED <ok|fail> <nombre>".

#include <windows.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "lancast/core/encoding.hpp"
#include "lancast/host/host_service.hpp"
#include "lancast/platform/crypto.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/net.hpp"
#include "lancast/platform/system.hpp"

using namespace lancast;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "uso: lancast-test-host <puerto> <carpeta>\n");
        return 2;
    }
    const auto port = static_cast<std::uint16_t>(std::stoi(argv[1]));
    const std::filesystem::path dir = argv[2];
    std::filesystem::create_directories(dir);
    SetEnvironmentVariableW(L"LANCAST_DATA_DIR", dir.c_str());
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    int code = 0;
    {
        net::WinsockScope winsock;
        mf::MfScope mf;
        AppSettings s;
        std::array<std::uint8_t, 16> id{};
        crypto::random_bytes(id);
        s.device_id = to_hex(id);
        if (argc >= 4) {
            // Emparejamiento previo con el cliente de prueba (secreto compartido de 32 bytes).
            const std::filesystem::path client_dir = argv[3];
            std::vector<std::uint8_t> secret(32);
            crypto::random_bytes(secret);
            SetEnvironmentVariableW(L"LANCAST_DATA_DIR", client_dir.c_str());
            const AppSettings client = sys::load_settings();  // Crea el id del cliente si no existe.
            sys::save_paired({sys::PairedDevice{s.device_id, "Host-Interop", "127.0.0.1:" + std::to_string(port), secret, "host"}});
            SetEnvironmentVariableW(L"LANCAST_DATA_DIR", dir.c_str());
            sys::save_paired({sys::PairedDevice{client.device_id, client.device_name, "127.0.0.1", secret, "client"}});
        }
        s.device_name = "Host-Interop";
        s.host.control_port = port;
        s.host.media_port = static_cast<std::uint16_t>(port + 14);
        s.host.allow_input = false;  // La prueba no debe mover el mouse ni escribir.
        s.host.allow_clipboard = false;
        s.host.receive_folder = (dir / "recibidos").string();

        host::HostService host;
        host.files().on_received = [](const files::Item& item) {
            std::printf("RECEIVED %s %s\n", item.state == files::State::Done ? "ok" : "fail", item.name.c_str());
            std::fflush(stdout);
        };
        host.start(s);
        bool ready = false;
        std::string last_pin;
        // Fin al cerrarse stdin (el proceso de prueba terminó).
        bool stdin_open = true;
        std::thread watcher([&] {
            char buf[1024];
            while (std::fgets(buf, sizeof(buf), stdin) != nullptr) {
                std::string line = buf;
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                if (line.starts_with("SEND ")) host.files().send({std::filesystem::path(line.substr(5))});
            }
            stdin_open = false;
        });
        for (int i = 0; i < 6000 && stdin_open; ++i) {  // Máximo 10 minutos.
            const auto st = host.status();
            if (!ready && st.listening) {
                ready = true;
                std::printf("READY\n");
                std::fflush(stdout);
            }
            if (!ready && !st.error.empty()) {
                std::printf("ERROR %s\n", st.error.c_str());
                std::fflush(stdout);
                code = 1;
                break;
            }
            if (st.pin && *st.pin != last_pin) {
                last_pin = *st.pin;
                std::printf("PIN %s\n", last_pin.c_str());
                std::fflush(stdout);
            }
            Sleep(100);
        }
        host.stop();
        if (watcher.joinable()) watcher.detach();
    }
    CoUninitialize();
    std::fflush(stdout);
    std::_Exit(code);
}
