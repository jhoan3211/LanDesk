#include "lancast/platform/updater.hpp"

#include "win.hpp"

#include <shellapi.h>
#include <winhttp.h>

#include <format>
#include <fstream>

#include "lancast/core/encoding.hpp"
#include "lancast/core/json_reader.hpp"
#include "lancast/core/log.hpp"
#include "lancast/platform/crypto.hpp"
#include "lancast/platform/signature.hpp"
#include "lancast/platform/system.hpp"

#pragma comment(lib, "winhttp.lib")

namespace lancast::update {
namespace {

namespace fs = std::filesystem;

struct HInternet {
    HINTERNET h = nullptr;
    ~HInternet() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
};

std::unexpected<platform::Error> win_error(const char* what) {
    return std::unexpected(platform::Error{what, HRESULT_FROM_WIN32(GetLastError())});
}

fs::path old_exe_path() { return fs::path{sys::exe_path()}.replace_extension(L".old.exe"); }

}  // namespace

platform::Result<std::vector<std::uint8_t>> https_get(const std::string& url, std::size_t max_bytes,
                                                      const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                                                      const std::atomic<bool>* cancel) {
    const std::wstring wurl = platform::detail::to_wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return std::unexpected(platform::Error{"URL no válida (solo HTTPS)", 0});
    }

    HInternet session{WinHttpOpen(std::format(L"LanCast/{}", platform::detail::to_wide(kAppVersion)).c_str(),
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.h == nullptr) return win_error("WinHttpOpen");
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    HInternet connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
    if (connection.h == nullptr) return win_error("No se pudo conectar con el servidor de actualizaciones");
    HInternet request{WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)};
    if (request.h == nullptr) return win_error("WinHttpOpenRequest");
    const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
    if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        return win_error("No se pudo descargar (¿sin internet?)");
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) return std::unexpected(platform::Error{std::format("El servidor respondió HTTP {}", status), 0});
    std::uint64_t total = 0;
    wchar_t length[32]{};
    size = sizeof(length);
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        total = std::wcstoull(length, nullptr, 10);
    }

    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> chunk(64 * 1024);
    while (true) {
        if (cancel != nullptr && cancel->load()) return std::unexpected(platform::Error{"Descarga cancelada", 0});
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), static_cast<DWORD>(chunk.size()), &read)) {
            return win_error("Error durante la descarga");
        }
        if (read == 0) break;
        body.insert(body.end(), chunk.begin(), chunk.begin() + read);
        if (body.size() > max_bytes) return std::unexpected(platform::Error{"Respuesta demasiado grande", 0});
        if (progress) progress(body.size(), total);
    }
    return body;
}

std::optional<std::string> find_sha256(std::string_view text) {
    for (std::size_t pos = text.find("SHA-256"); pos != std::string_view::npos; pos = text.find("SHA-256", pos + 1)) {
        std::size_t i = pos + 7;
        while (i < text.size() && (text[i] == ':' || text[i] == ' ' || text[i] == '`' || text[i] == '*')) ++i;
        std::string hex;
        while (i < text.size() && hex.size() < 64 && std::isxdigit(static_cast<unsigned char>(text[i]))) {
            hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[i]))));
            ++i;
        }
        if (hex.size() == 64) return hex;
    }
    return std::nullopt;
}

platform::Result<std::optional<ReleaseInfo>> check(Version current) {
    auto body = https_get(std::format("https://api.github.com/repos/{}/releases/latest", kUpdateRepo), 4u * 1024 * 1024);
    if (!body) return std::unexpected(body.error());
    const auto doc = json::parse({reinterpret_cast<const char*>(body->data()), body->size()});
    if (!doc) return std::unexpected(platform::Error{"Respuesta de GitHub no válida", 0});

    ReleaseInfo info;
    info.tag = (*doc)["tag_name"].string_or("");
    const auto version = parse_version(info.tag);
    if (!version) return std::unexpected(platform::Error{"La última versión no tiene un número válido: " + info.tag, 0});
    info.version = *version;
    if (!(current < info.version)) return std::optional<ReleaseInfo>{};

    info.notes = (*doc)["body"].string_or("");
    info.page_url = (*doc)["html_url"].string_or("");
    if (const json::Array* assets = (*doc)["assets"].array()) {
        for (const json::Value& a : *assets) {
            if (a["name"].string_or("") == "LanCast.exe") {
                info.download_url = a["browser_download_url"].string_or("");
                info.size = static_cast<std::uint64_t>(a["size"].number().value_or(0));
            }
        }
    }
    if (info.download_url.empty()) return std::unexpected(platform::Error{"La versión no incluye LanCast.exe", 0});
    info.sha256 = find_sha256(info.notes).value_or("");
    return std::optional{std::move(info)};
}

platform::Result<fs::path> download(const ReleaseInfo& release,
                                    const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                                    const std::atomic<bool>* cancel) {
    if (release.sha256.empty()) {
        return std::unexpected(platform::Error{"La versión no publica su SHA-256: por seguridad no se instala sola", 0});
    }
    auto data = https_get(release.download_url, 256u * 1024 * 1024, progress, cancel);
    if (!data) return std::unexpected(data.error());
    if (release.size != 0 && data->size() != release.size) {
        return std::unexpected(platform::Error{"El tamaño descargado no coincide", 0});
    }
    if (to_hex(crypto::sha256(*data)) != release.sha256) {
        return std::unexpected(platform::Error{"La verificación SHA-256 falló: el archivo no es el publicado", 0});
    }
    // Validación mínima de que es un ejecutable de Windows.
    if (data->size() < 2 || (*data)[0] != 'M' || (*data)[1] != 'Z') {
        return std::unexpected(platform::Error{"El archivo descargado no es un ejecutable", 0});
    }
    const fs::path out = fs::temp_directory_path() / std::format(L"LanCast-{}.exe", platform::detail::to_wide(release.tag));
    {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
        if (!f) return std::unexpected(platform::Error{"No se pudo guardar la descarga", 0});
    }
    // Si esta copia está firmada, la nueva debe tener una firma válida del mismo editor (además del SHA-256).
    if (const sig::Info self = sig::verify(sys::exe_path()); self.valid) {
        const sig::Info next = sig::verify(out);
        if (!next.valid || next.subject != self.subject) {
            std::error_code ec;
            fs::remove(out, ec);
            return std::unexpected(platform::Error{
                std::format("La versión nueva no está firmada por {} ({}): no se instala",
                            self.subject, next.valid ? "firmada por " + next.subject : next.error),
                0});
        }
        log::info("Firma de la actualización verificada: {}", next.subject);
    }
    log::info("Actualización {} descargada y verificada ({} bytes)", release.tag, data->size());
    return out;
}

platform::Result<void> install_and_restart(const fs::path& new_exe) {
    const fs::path current = sys::exe_path();
    const fs::path old = old_exe_path();
    DeleteFileW(old.c_str());
    // Un .exe en ejecución no se puede sobrescribir, pero sí renombrar.
    if (!MoveFileExW(current.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return win_error("No se pudo reemplazar LanCast.exe (¿carpeta sin permisos de escritura?)");
    }
    if (!CopyFileW(new_exe.c_str(), current.c_str(), FALSE)) {
        const auto err = win_error("No se pudo copiar la versión nueva");
        MoveFileExW(old.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING);  // Deshacer.
        return err;
    }
    DeleteFileW(new_exe.c_str());

    // Relanzar con los mismos argumentos.
    std::wstring cmd = L"\"" + current.wstring() + L"\"";
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc); argv != nullptr) {
        for (int i = 1; i < argc; ++i) cmd += std::wstring{L" \""} + argv[i] + L"\"";
        LocalFree(argv);
    }
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return win_error("La versión nueva se instaló pero no se pudo iniciar");
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    log::info("Actualización instalada; reiniciando");
    return {};
}

void cleanup_previous() {
    std::error_code ec;
    const fs::path old = old_exe_path();
    // El proceso viejo puede tardar un instante en salir.
    for (int i = 0; i < 20 && fs::exists(old, ec); ++i) {
        if (DeleteFileW(old.c_str())) break;
        Sleep(100);
    }
}

}  // namespace lancast::update
