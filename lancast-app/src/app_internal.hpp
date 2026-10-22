#pragma once

// Utilidades internas compartidas por los archivos de la aplicación.

#include <windows.h>

#include <string>
#include <string_view>
#include <filesystem>

#include "lancast/host/host_service.hpp"

namespace lancast::app {

class App;

namespace detail {

inline constexpr UINT kTrayMessage = WM_APP + 1;
inline constexpr UINT kTrayId = 1;
inline constexpr UINT_PTR kBannerButton = 100;
inline constexpr UINT kMsgBannerDisconnect = WM_APP + 2;  // Botón "Desconectar" del aviso de sesión.
inline constexpr UINT kMsgClientDisconnect = WM_APP + 3;  // Ctrl+Alt+Shift+Q y botones de desconexión.

extern App* g_app;

std::wstring widen(std::string_view s);
std::string narrow(std::wstring_view w);
}  // namespace detail

// Estadísticas del host en modo servicio (live_stats.cpp): carpeta legible por cualquier usuario del equipo.
std::filesystem::path service_stats_dir();
void write_service_host_stats(const host::Status& hs);

namespace detail {

LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK keyboard_proc(int code, WPARAM wp, LPARAM lp);
LRESULT CALLBACK banner_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

}  // namespace detail
}  // namespace lancast::app
