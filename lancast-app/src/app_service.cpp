// Host como servicio de Windows: estado, instalación/desinstalación (elevada) y sección de la interfaz.

#include <filesystem>
#include <format>

#include "app.hpp"
#include "app_internal.hpp"
#include "lancast/core/log.hpp"
#include "lancast/platform/service.hpp"
#include "ui_common.hpp"
#include "imgui.h"

namespace lancast::app {

using namespace ui;

namespace {

// ¿La copia instalada es distinta de este ejecutable? (CopyFile conserva la fecha de modificación.)
bool installed_copy_differs(const std::filesystem::path& installed) {
    std::error_code ec;
    const auto self = sys::exe_path();
    if (installed.empty() || std::filesystem::equivalent(self, installed, ec)) return false;
    if (std::filesystem::file_size(self, ec) != std::filesystem::file_size(installed, ec)) return true;
    return std::filesystem::last_write_time(self, ec) != std::filesystem::last_write_time(installed, ec);
}

}  // namespace

AppSettings App::effective_host_settings() const {
    AppSettings s = settings_;
    if (service_host_) s.host.enabled = false;  // El agente del servicio ya usa los puertos.
    return s;
}

void App::refresh_service_status() {
    service_status_ = service::query();
    const bool running =
        service_status_.state == service::State::Running || service_status_.state == service::State::Starting;
    if (running != service_host_) {
        service_host_ = running;
        log::info("Servicio de Windows: {}", service::to_string(service_status_.state));
        host_.apply_settings(effective_host_settings());
    }
    service_outdated_ = service_host_ && installed_copy_differs(service_status_.binary);
}

void App::start_service_action(bool install) {
    if (service_busy_) return;
    if (service_thread_.joinable()) service_thread_.join();
    if (install && !service_host_) host_.stop();  // Liberar los puertos para el agente.
    service_busy_ = true;
    service_thread_ = std::thread([this, install] {
        auto r = service::run_elevated(install ? L"--install-service" : L"--uninstall-service");
        std::string message;
        if (!r) {
            message = "No se aplicó: " + r.error().message();
        } else if (*r != 0) {
            message = std::format("Falló (código {}). Detalles en service-install.log (carpeta de datos de LanCast)", *r);
        } else {
            message = install ? "Servicio instalado: el host ahora funciona también en la pantalla de bloqueo y UAC"
                              : "Servicio desinstalado: el host vuelve a ejecutarse dentro de la app";
        }
        std::scoped_lock lock(service_mutex_);
        service_message_ = std::move(message);
        service_busy_ = false;
    });
}

void App::poll_service_action() {
    std::optional<std::string> message;
    {
        std::scoped_lock lock(service_mutex_);
        message = std::move(service_message_);
        service_message_.reset();
    }
    if (!message) return;
    if (service_thread_.joinable()) service_thread_.join();
    refresh_service_status();
    if (!service_host_) host_.start(effective_host_settings());  // Si no quedó el servicio, volver al host de la app.
    toast(*message);
}

void App::draw_service_section() {
    section("Servicio de Windows");
    if (service_host_) {
        ImGui::TextColored(kGood, "● El host se ejecuta como servicio de Windows");
        ImGui::TextWrapped(
            "Funciona aunque cierres la sesión: pantalla de bloqueo, avisos de UAC y Ctrl+Alt+Supr (Ctrl+Alt+Fin desde el "
            "cliente). La configuración y los equipos emparejados se copian al servicio al instalarlo o actualizarlo; "
            "el PIN para emparejar aparece en una ventana pequeña abajo a la derecha.");
        if (service_outdated_) ImGui::TextColored(kWarn, "El servicio usa otra versión de LanCast.exe.");
    } else {
        ImGui::TextWrapped(
            "Instálalo para controlar este equipo también en la pantalla de bloqueo, en los avisos de UAC y para enviar "
            "Ctrl+Alt+Supr. Se pedirá permiso de administrador.");
        if (service_status_.state == service::State::Stopped) ImGui::TextColored(kWarn, "El servicio está instalado pero detenido.");
    }
    ImGui::BeginDisabled(service_busy_);
    if (!service_host_) {
        if (primary_button("Instalar como servicio")) start_service_action(true);
    } else {
        if (ImGui::Button(service_outdated_ ? "Actualizar servicio a esta versión" : "Copiar configuración al servicio")) {
            start_service_action(true);
        }
        ImGui::SameLine();
        if (danger_button("Desinstalar servicio")) start_service_action(false);
    }
    if (!service_host_ && service_status_.state != service::State::NotInstalled) {
        ImGui::SameLine();
        if (danger_button("Desinstalar servicio")) start_service_action(false);
    }
    ImGui::EndDisabled();
    if (service_busy_) {
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "Esperando permiso de administrador…");
    }
}

}  // namespace lancast::app
