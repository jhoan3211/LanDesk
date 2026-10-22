#pragma once

// Utilidades del sistema: rutas, configuración persistente, firewall, inicio automático y diálogos.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "lancast/core/settings.hpp"

struct HWND__;

namespace lancast::sys {

[[nodiscard]] std::filesystem::path app_data_dir();   // %APPDATA%\LanCast
[[nodiscard]] std::filesystem::path downloads_dir();  // Carpeta Descargas del usuario
[[nodiscard]] std::filesystem::path exe_path();
[[nodiscard]] std::filesystem::path default_receive_dir();  // Descargas\LanCast

// Carga config.toml (creando valores por defecto: id del equipo, nombre, perfiles) y lo guarda.
[[nodiscard]] AppSettings load_settings();
void save_settings(const AppSettings& settings);

// Secretos de emparejamiento (cifrados con DPAPI) por id de dispositivo remoto.
struct PairedDevice {
    std::string id_hex;
    std::string name;
    std::string last_address;
    std::vector<std::uint8_t> secret;  // 32 bytes
    std::string role;                  // "host" (equipos a los que me conecto) o "client" (equipos que me controlan)
    std::string mac;                   // "aa:bb:cc:dd:ee:ff" del host (para despertarlo con Wake-on-LAN); vacío si no se sabe.
};
[[nodiscard]] std::vector<PairedDevice> load_paired();
void save_paired(const std::vector<PairedDevice>& devices);

[[nodiscard]] bool is_elevated();

// Registra el hilo actual en MMCSS (planificador multimedia de Windows) mientras viva el objeto:
// menos interrupciones y latencia de planificación más estable. task: "Games", "Capture", "Pro Audio"...
class RealtimeThread {
public:
    explicit RealtimeThread(const wchar_t* task, bool critical = false);
    ~RealtimeThread();
    RealtimeThread(const RealtimeThread&) = delete;
    RealtimeThread& operator=(const RealtimeThread&) = delete;

private:
    void* handle_ = nullptr;
};
// Crea reglas de entrada del firewall (perfil Privado) para LanCast.exe, pidiendo elevación (UAC).
bool add_firewall_rules();
bool set_autostart(bool enabled);
void open_in_explorer(const std::filesystem::path& path);
void open_url(const std::string& url);

// Diálogos nativos.
[[nodiscard]] std::vector<std::filesystem::path> pick_files(HWND__* owner);
[[nodiscard]] std::optional<std::filesystem::path> pick_folder(HWND__* owner);

}  // namespace lancast::sys
