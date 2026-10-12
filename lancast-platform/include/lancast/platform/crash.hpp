#pragma once

// Volcados de memoria (minidumps) ante un cierre inesperado: excepción no controlada, abort(), terminate(),
// llamada virtual pura o parámetro inválido del CRT. Un hilo preparado de antemano escribe el .dmp (así funciona
// también ante un desbordamiento de pila). Se conservan los últimos 10.

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace lancast::crash {

// role: "app", "agente" o "servicio" (va en el nombre del archivo).
void install(const std::filesystem::path& dir, std::string_view role);

// Volcados existentes, del más nuevo al más viejo.
[[nodiscard]] std::vector<std::filesystem::path> list(const std::filesystem::path& dir);

// El volcado más nuevo que el usuario todavía no vio (para avisarle al iniciar) y marcarlo como visto.
[[nodiscard]] std::optional<std::filesystem::path> unseen(const std::filesystem::path& dir);
void mark_seen(const std::filesystem::path& dir);

// Solo para pruebas ("LanCast.exe --crash-test"): provoca un acceso inválido.
[[noreturn]] void crash_for_test();

}  // namespace lancast::crash
