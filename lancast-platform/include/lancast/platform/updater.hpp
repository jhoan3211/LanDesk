#pragma once

// Actualización automática desde GitHub Releases: consulta la última versión, descarga LanCast.exe por
// HTTPS, verifica el SHA-256 publicado en las notas de la versión, reemplaza el ejecutable y reinicia.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "lancast/core/version.hpp"
#include "lancast/platform/error.hpp"

namespace lancast::update {

struct ReleaseInfo {
    Version version;
    std::string tag;
    std::string notes;         // Texto de la versión (Markdown).
    std::string download_url;  // Asset LanCast.exe.
    std::uint64_t size = 0;
    std::string sha256;        // Publicado en las notas como "SHA-256: <hex>".
    std::string page_url;
};

// Descarga simple por HTTPS (WinHTTP, sigue redirecciones). progress(descargado, total).
[[nodiscard]] platform::Result<std::vector<std::uint8_t>> https_get(
    const std::string& url, std::size_t max_bytes,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Última versión publicada; nullopt si no hay una más nueva que `current`.
[[nodiscard]] platform::Result<std::optional<ReleaseInfo>> check(Version current);

// Descarga y verifica el ejecutable nuevo (lo deja en %TEMP%).
[[nodiscard]] platform::Result<std::filesystem::path> download(const ReleaseInfo& release,
                                                               const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                                                               const std::atomic<bool>* cancel = nullptr);

// Reemplaza el .exe en uso (renombrándolo a .old) y lanza la versión nueva con los mismos argumentos.
// Si devuelve éxito, el llamador debe cerrarse.
[[nodiscard]] platform::Result<void> install_and_restart(const std::filesystem::path& new_exe);

// Borra el .old que deja una actualización anterior (llamar al iniciar).
void cleanup_previous();

// Extrae "SHA-256: <64 hex>" de un texto.
[[nodiscard]] std::optional<std::string> find_sha256(std::string_view text);

}  // namespace lancast::update
