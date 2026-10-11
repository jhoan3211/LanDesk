#include "lancast/platform/system.hpp"

#include "win.hpp"

#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <fstream>
#include <sstream>

#include "lancast/core/encoding.hpp"
#include "lancast/core/log.hpp"
#include "lancast/core/toml.hpp"
#include "lancast/platform/crypto.hpp"
#include "lancast/platform/net.hpp"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace lancast::sys {
namespace {

std::filesystem::path known_folder(const KNOWNFOLDERID& id) {
    PWSTR path = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file_atomic(const std::filesystem::path& p, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const auto tmp = std::filesystem::path{p}.concat(L".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << content;
    }
    std::filesystem::rename(tmp, p, ec);
    if (ec) log::warn("No se pudo guardar {}: {}", platform::detail::to_utf8(p.wstring()), ec.message());
}

}  // namespace

std::filesystem::path app_data_dir() {
    // LANCAST_DATA_DIR permite perfiles separados (pruebas o dos instancias en el mismo equipo).
    wchar_t custom[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LANCAST_DATA_DIR", custom, MAX_PATH) > 0) return custom;
    return known_folder(FOLDERID_RoamingAppData) / L"LanCast";
}
std::filesystem::path downloads_dir() { return known_folder(FOLDERID_Downloads); }
std::filesystem::path default_receive_dir() { return downloads_dir() / L"LanCast"; }

std::filesystem::path exe_path() {
    wchar_t buf[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return buf;
}

AppSettings load_settings() {
    const auto path = app_data_dir() / L"config.toml";
    AppSettings s = settings_from_toml(read_file(path));
    bool changed = !std::filesystem::exists(path);
    if (s.device_id.size() != 32) {
        std::array<std::uint8_t, 16> id{};
        crypto::random_bytes(id);
        s.device_id = to_hex(id);
        changed = true;
    }
    if (s.device_name.empty()) {
        s.device_name = net::computer_name();
        changed = true;
    }
    if (changed) save_settings(s);
    return s;
}

void save_settings(const AppSettings& settings) {
    write_file_atomic(app_data_dir() / L"config.toml", settings_to_toml(settings));
}

std::vector<PairedDevice> load_paired() {
    std::vector<PairedDevice> out;
    const auto doc = toml::Document::parse(read_file(app_data_dir() / L"paired.toml"));
    const auto it = doc.arrays.find("device");
    if (it == doc.arrays.end()) return out;
    for (const toml::Table& t : it->second) {
        PairedDevice d;
        d.id_hex = t.get_string("id").value_or("");
        d.name = t.get_string("name").value_or("");
        d.last_address = t.get_string("address").value_or("");
        d.role = t.get_string("role").value_or("");
        const auto blob = base64_decode(t.get_string("secret").value_or(""));
        if (!blob) continue;
        auto plain = crypto::unprotect(*blob);
        if (!plain || plain->size() != 32 || d.id_hex.size() != 32) continue;
        d.secret = std::move(*plain);
        out.push_back(std::move(d));
    }
    return out;
}

void save_paired(const std::vector<PairedDevice>& devices) {
    toml::Document doc;
    auto& arr = doc.arrays["device"];
    for (const PairedDevice& d : devices) {
        toml::Table t;
        t.set("id", d.id_hex);
        t.set("name", d.name);
        t.set("address", d.last_address);
        t.set("role", d.role);
        t.set("secret", base64_encode(crypto::protect(d.secret)));
        arr.push_back(std::move(t));
    }
    write_file_atomic(app_data_dir() / L"paired.toml", doc.serialize());
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

bool add_firewall_rules() {
    const std::wstring exe = exe_path().wstring();
    // Un solo cmd elevado: borra reglas previas y crea TCP+UDP de entrada solo para el perfil Privado.
    const std::wstring script =
        L"/c netsh advfirewall firewall delete rule name=\"LanCast\" >nul 2>&1 & "
        L"netsh advfirewall firewall add rule name=\"LanCast\" dir=in action=allow profile=private protocol=TCP program=\"" +
        exe +
        L"\" & "
        L"netsh advfirewall firewall add rule name=\"LanCast\" dir=in action=allow profile=private protocol=UDP program=\"" +
        exe + L"\"";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = L"cmd.exe";
    info.lpParameters = script.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) return false;
    if (info.hProcess != nullptr) {
        WaitForSingleObject(info.hProcess, 15000);
        DWORD code = 1;
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hProcess);
        return code == 0;
    }
    return true;
}

bool set_autostart(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    LSTATUS rc = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring value = L"\"" + exe_path().wstring() + L"\" --minimized";
        rc = RegSetValueExW(key, L"LanCast", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, L"LanCast");
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

void open_in_explorer(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void open_url(const std::string& url) {
    ShellExecuteW(nullptr, L"open", platform::detail::to_wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::vector<std::filesystem::path> pick_files(HWND__* owner) {
    std::vector<std::filesystem::path> out;
    platform::detail::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Elegir archivos para enviar");
    if (FAILED(dlg->Show(owner))) return out;
    platform::detail::ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items))) return out;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        platform::detail::ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    return out;
}

std::optional<std::filesystem::path> pick_folder(HWND__* owner) {
    platform::detail::ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (FAILED(dlg->Show(owner))) return std::nullopt;
    platform::detail::ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    std::filesystem::path out{path};
    CoTaskMemFree(path);
    return out;
}

}  // namespace lancast::sys
