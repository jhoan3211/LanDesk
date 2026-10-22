#pragma once

// Servicio de host: escucha conexiones (TCP control + archivos, UDP medios, descubrimiento), empareja
// clientes con PIN y gestiona una sesión de streaming a la vez.

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lancast/core/protocol.hpp"
#include "lancast/core/settings.hpp"
#include "lancast/platform/files.hpp"
#include "lancast/platform/system.hpp"

struct HWND__;

namespace lancast::host {

struct StreamStats {
    double fps = 0;
    double bitrate_mbps = 0;
    double capture_to_encoded_ms = 0;  // Captura + conversión + codificación.
    double capture_wait_ms = 0;        // Windows presentó el frame -> empieza la conversión.
    double encode_ms = 0;              // Conversión NV12 + codificador por hardware.
    std::uint64_t frames = 0;
    std::uint64_t keyframes = 0;
    double skipped_percent = 0;  // Cuadros no enviados porque el codificador seguía ocupado.
};

struct Status {
    bool listening = false;
    std::string error;
    std::vector<std::string> addresses;  // IPs locales con puerto de control.

    // Emparejamiento pendiente: el usuario del cliente debe escribir este PIN.
    std::optional<std::string> pin;
    std::string pin_requester;

    bool session_active = false;
    std::string client_name;
    std::string client_address;
    std::uint64_t session_started_us = 0;
    std::optional<proto::StreamStarted> stream;
    std::string encoder_name;
    StreamStats stats;
    std::vector<std::string> viewers;  // Espectadores (solo ven): "nombre (IP)".
    bool view_only = false;
    // Calibración del codificador (se hace una vez por tarjeta y driver, en segundo plano).
    bool calibrating = false;
    double encode_420_ms = 0;  // A la resolución nativa del monitor principal.
    double encode_444_ms = 0;
    Size calibrated_size;
};

class HostService {
public:
    HostService();
    ~HostService();
    HostService(const HostService&) = delete;
    HostService& operator=(const HostService&) = delete;

    void start(const AppSettings& settings);
    void stop();
    // Aplica permisos/carpetas; reinicia los puertos si cambiaron.
    void apply_settings(const AppSettings& settings);

    [[nodiscard]] Status status() const;
    void disconnect();
    // Espectadores conectados (solo ven lo mismo que quien controla).
    void disconnect_viewers();
    void set_view_only(bool view_only);
    // Agente del servicio de Windows: anuncia bloqueo/UAC y acepta Ctrl+Alt+Supr.
    void set_service_mode(bool service_mode);
    void cancel_pairing();

    [[nodiscard]] std::vector<sys::PairedDevice> paired_clients() const;
    void revoke(const std::string& id_hex);

    files::Manager& files() noexcept { return files_; }
    void set_window(HWND__* hwnd) noexcept { hwnd_ = hwnd; }
    // La interfaz avisa de cambios del portapapeles local (WM_CLIPBOARDUPDATE).
    void on_local_clipboard(const std::string& text);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    files::Manager files_;
    HWND__* hwnd_ = nullptr;
};

}  // namespace lancast::host
