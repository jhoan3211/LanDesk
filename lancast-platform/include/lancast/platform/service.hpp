#pragma once

// Host como servicio de Windows: un servicio SYSTEM ("LanCastHost") que lanza el agente (LanCast.exe --agent)
// como SYSTEM dentro de la sesión de consola. Así el host captura y controla también la pantalla de bloqueo,
// los avisos de UAC (escritorio seguro) y puede generar Ctrl+Alt+Supr con SendSAS.

#include <filesystem>
#include <string>

#include "lancast/platform/error.hpp"

namespace lancast::service {

inline constexpr wchar_t kServiceName[] = L"LanCastHost";

enum class State { NotInstalled, Stopped, Starting, Running, Stopping, Unknown };

struct Status {
    State state = State::NotInstalled;
    std::filesystem::path binary;  // Ejecutable registrado en el servicio.
};

// Consulta sin privilegios.
[[nodiscard]] Status query();
[[nodiscard]] std::string_view to_string(State s) noexcept;

// %ProgramData%\LanCast: configuración y emparejamientos del servicio (solo SYSTEM y Administradores).
[[nodiscard]] std::filesystem::path data_dir();
// %ProgramFiles%\LanCast\LanCast.exe: copia estable del ejecutable que usa el servicio.
[[nodiscard]] std::filesystem::path installed_exe();

// Requieren privilegios de administrador (se llaman desde "LanCast.exe --install-service" elevado).
// install copia el ejecutable, la configuración y los emparejamientos del usuario actual, registra
// e inicia el servicio, habilita SendSAS y abre el firewall para la copia instalada.
[[nodiscard]] platform::Result<void> install();
[[nodiscard]] platform::Result<void> uninstall();

// Relanza este ejecutable elevado (UAC) con un argumento y espera a que termine. Devuelve su código de salida.
[[nodiscard]] platform::Result<int> run_elevated(const std::wstring& argument);

// Punto de entrada del proceso de servicio ("LanCast.exe --service"). Devuelve al detenerse el servicio.
int run_service();

// Desde el agente: pedir al servicio un Ctrl+Alt+Supr (SendSAS). false si no hay servicio.
bool request_secure_attention();

// Evento con nombre que el servicio activa para que el agente termine de forma ordenada.
[[nodiscard]] void* open_agent_stop_event();

}  // namespace lancast::service
