#include "lancast/platform/service.hpp"

#include "win.hpp"

#include <sddl.h>
#include <aclapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "lancast/core/log.hpp"
#include "lancast/platform/crypto.hpp"
#include "lancast/platform/system.hpp"

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace lancast::service {
namespace {

constexpr wchar_t kDisplayName[] = L"LanCast Host";
constexpr wchar_t kDescription[] =
    L"Permite controlar este equipo con LanCast también en la pantalla de bloqueo y en los avisos de UAC.";
constexpr wchar_t kSasEvent[] = L"Global\\LanCastSAS";
constexpr wchar_t kAgentStopEvent[] = L"Global\\LanCastAgentStop";
constexpr wchar_t kPolicyKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
constexpr wchar_t kFirewallRule[] = L"LanCast (servicio)";

std::filesystem::path known_folder(const KNOWNFOLDERID& id) {
    PWSTR path = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

auto win32_error(std::string context, DWORD code = GetLastError()) {
    return platform::detail::hr_error(std::move(context), HRESULT_FROM_WIN32(code));
}

struct ScHandle {
    SC_HANDLE h = nullptr;
    ScHandle() = default;
    explicit ScHandle(SC_HANDLE handle) : h(handle) {}
    ~ScHandle() {
        if (h != nullptr) CloseServiceHandle(h);
    }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    explicit operator bool() const noexcept { return h != nullptr; }
};

// Ejecuta un comando oculto (ya elevado) y espera su fin.
DWORD run_hidden(std::wstring cmdline) {
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return GetLastError();
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

std::wstring system_exe(const wchar_t* name) {
    wchar_t dir[MAX_PATH]{};
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(L"\"") + dir + L"\\" + name + L"\"";
}

bool wait_for_state(SC_HANDLE svc, DWORD wanted, DWORD timeout_ms) {
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    SERVICE_STATUS st{};
    while (QueryServiceStatus(svc, &st)) {
        if (st.dwCurrentState == wanted) return true;
        if (GetTickCount64() > until) return false;
        Sleep(200);
    }
    return false;
}

void stop_service(SC_HANDLE svc) {
    SERVICE_STATUS st{};
    if (QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED) {
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        wait_for_state(svc, SERVICE_STOPPED, 15000);
    }
}

// Carpeta de datos accesible solo por SYSTEM y Administradores (los secretos van con DPAPI de equipo).
platform::Result<void> secure_data_dir(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sd,
                                                              nullptr)) {
        return win32_error("Descriptor de seguridad");
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    std::wstring path = dir.wstring();
    const DWORD err = SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
                                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                            dacl, nullptr);
    LocalFree(sd);
    if (err != ERROR_SUCCESS) return win32_error("Permisos de la carpeta de datos", err);
    return {};
}

bool copy_with_retry(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    for (int i = 0; i < 40; ++i) {  // El agente puede tardar un momento en liberar el ejecutable.
        if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
        Sleep(250);
    }
    return false;
}

// Copia configuración y emparejamientos del usuario actual a la carpeta del servicio.
void import_user_data(const std::filesystem::path& dir) {
    AppSettings user = sys::load_settings();
    const auto user_paired = sys::load_paired();
    if (user.host.receive_folder.empty()) {
        user.host.receive_folder = platform::detail::to_utf8(sys::default_receive_dir().wstring());
    }

    // Desde aquí, sys:: trabaja sobre la carpeta del servicio con DPAPI de equipo.
    SetEnvironmentVariableW(L"LANCAST_DATA_DIR", dir.c_str());
    crypto::set_machine_scope(true);
    sys::save_settings(user);
    auto paired = sys::load_paired();
    for (const auto& d : user_paired) {
        if (d.role != "client") continue;  // Solo los equipos que controlan este PC.
        std::erase_if(paired, [&](const sys::PairedDevice& p) { return p.id_hex == d.id_hex && p.role == d.role; });
        paired.push_back(d);
    }
    sys::save_paired(paired);
    SetEnvironmentVariableW(L"LANCAST_DATA_DIR", nullptr);
    crypto::set_machine_scope(false);
    log::info("Servicio: importados la configuración y {} emparejamiento(s)", paired.size());
}

void enable_software_sas() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kPolicyKey, L"SoftwareSASGeneration", RRF_RT_REG_DWORD, nullptr, &value, &size) ==
            ERROR_SUCCESS &&
        (value & 1) != 0) {
        return;  // Ya permitido para servicios.
    }
    value |= 1;
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, kPolicyKey, L"SoftwareSASGeneration", REG_DWORD, &value, sizeof(value));
}

void disable_software_sas() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kPolicyKey, L"SoftwareSASGeneration", RRF_RT_REG_DWORD, nullptr, &value, &size) ==
            ERROR_SUCCESS &&
        value == 1) {
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kPolicyKey, L"SoftwareSASGeneration");
    }
}

void set_firewall(const std::filesystem::path& exe, bool add) {
    const std::wstring netsh = system_exe(L"netsh.exe");
    run_hidden(netsh + L" advfirewall firewall delete rule name=\"" + kFirewallRule + L"\"");
    if (!add) return;
    for (const wchar_t* proto : {L"TCP", L"UDP"}) {
        run_hidden(netsh + L" advfirewall firewall add rule name=\"" + kFirewallRule +
                   L"\" dir=in action=allow profile=private protocol=" + proto + L" program=\"" + exe.wstring() + L"\"");
    }
}

// --- Proceso de servicio ---

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop = nullptr;
HANDLE g_session_changed = nullptr;

void report(DWORD state, DWORD exit_code = NO_ERROR) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
    g_status.dwWaitHint = state == SERVICE_RUNNING || state == SERVICE_STOPPED ? 0 : 10000;
    SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI control_handler(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            report(SERVICE_STOP_PENDING);
            SetEvent(g_stop);
            return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE:
            SetEvent(g_session_changed);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// Lanza "LanCast.exe --agent" como SYSTEM dentro de la sesión indicada (escritorio interactivo).
HANDLE launch_agent(DWORD session) {
    HANDLE self = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &self)) return nullptr;
    HANDLE token = nullptr;
    const BOOL dup = DuplicateTokenEx(self, MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary, &token);
    CloseHandle(self);
    if (!dup) return nullptr;
    if (!SetTokenInformation(token, TokenSessionId, &session, sizeof(session))) {
        log::error("Servicio: no se pudo asignar la sesión {} ({})", session, GetLastError());
        CloseHandle(token);
        return nullptr;
    }
    void* env = nullptr;
    CreateEnvironmentBlock(&env, token, FALSE);
    std::wstring cmd = L"\"" + sys::exe_path().wstring() + L"\" --agent";
    STARTUPINFOW si{sizeof(si)};
    wchar_t desktop[] = L"winsta0\\default";
    si.lpDesktop = desktop;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessAsUserW(token, nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                         CREATE_UNICODE_ENVIRONMENT | ABOVE_NORMAL_PRIORITY_CLASS, env, nullptr, &si, &pi);
    const DWORD err = GetLastError();
    if (env != nullptr) DestroyEnvironmentBlock(env);
    CloseHandle(token);
    if (!ok) {
        log::error("Servicio: no se pudo lanzar el agente en la sesión {} ({})", session, err);
        return nullptr;
    }
    CloseHandle(pi.hThread);
    log::info("Servicio: agente iniciado en la sesión {} (pid {})", session, pi.dwProcessId);
    return pi.hProcess;
}

void stop_agent(HANDLE& process, HANDLE agent_stop) {
    if (process == nullptr) return;
    SetEvent(agent_stop);
    if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0) {
        log::warn("Servicio: el agente no terminó a tiempo; se fuerza el cierre");
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 2000);
    }
    CloseHandle(process);
    process = nullptr;
    ResetEvent(agent_stop);
}

void send_sas() {
    using SendSasFn = void(WINAPI*)(BOOL);
    if (HMODULE sas = LoadLibraryExW(L"sas.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); sas != nullptr) {
        if (auto fn = reinterpret_cast<SendSasFn>(reinterpret_cast<void*>(GetProcAddress(sas, "SendSAS"))); fn != nullptr) {
            fn(FALSE);
            log::info("Servicio: Ctrl+Alt+Supr enviado");
        }
        FreeLibrary(sas);
    }
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, control_handler, nullptr);
    if (g_status_handle == nullptr) return;
    report(SERVICE_START_PENDING);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_session_changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // Los objetos creados por SYSTEM solo los abren SYSTEM y Administradores (DACL por defecto).
    HANDLE sas = CreateEventW(nullptr, FALSE, FALSE, kSasEvent);
    HANDLE agent_stop = CreateEventW(nullptr, TRUE, FALSE, kAgentStopEvent);
    report(SERVICE_RUNNING);
    log::info("Servicio LanCast iniciado");

    HANDLE agent = nullptr;
    DWORD agent_session = 0xFFFFFFFF;
    ULONGLONG next_launch = 0;
    int failures = 0;
    for (;;) {
        const DWORD session = WTSGetActiveConsoleSessionId();
        if (agent != nullptr && session != 0xFFFFFFFF && session != agent_session) {
            log::info("Servicio: la sesión de consola cambió ({} -> {})", agent_session, session);
            stop_agent(agent, agent_stop);
        }
        if (agent == nullptr && session != 0xFFFFFFFF && GetTickCount64() >= next_launch) {
            agent = launch_agent(session);
            agent_session = session;
            if (agent == nullptr) next_launch = GetTickCount64() + 5000;
        }

        HANDLE waits[4] = {g_stop, g_session_changed, sas, agent};
        const DWORD count = agent != nullptr ? 4 : 3;
        const DWORD r = WaitForMultipleObjects(count, waits, FALSE, 1000);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 2) send_sas();
        if (r == WAIT_OBJECT_0 + 3) {
            DWORD code = 0;
            GetExitCodeProcess(agent, &code);
            CloseHandle(agent);
            agent = nullptr;
            failures = code == 0 ? 0 : std::min(failures + 1, 6);
            next_launch = GetTickCount64() + 500ull * (1ull << failures);  // Espera creciente si falla seguido.
            log::warn("Servicio: el agente terminó (código {}); se relanza", code);
        }
    }

    stop_agent(agent, agent_stop);
    CloseHandle(agent_stop);
    CloseHandle(sas);
    CloseHandle(g_session_changed);
    CloseHandle(g_stop);
    log::info("Servicio LanCast detenido");
    report(SERVICE_STOPPED);
}

}  // namespace

std::string_view to_string(State s) noexcept {
    switch (s) {
        case State::NotInstalled: return "no instalado";
        case State::Stopped: return "detenido";
        case State::Starting: return "iniciando";
        case State::Running: return "en ejecución";
        case State::Stopping: return "deteniéndose";
        case State::Unknown: break;
    }
    return "desconocido";
}

std::filesystem::path data_dir() { return known_folder(FOLDERID_ProgramData) / L"LanCast"; }
std::filesystem::path installed_exe() { return known_folder(FOLDERID_ProgramFiles) / L"LanCast" / L"LanCast.exe"; }

Status query() {
    Status out;
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        out.state = State::Unknown;
        return out;
    }
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!svc) return out;
    SERVICE_STATUS st{};
    out.state = State::Unknown;
    if (QueryServiceStatus(svc.h, &st)) {
        switch (st.dwCurrentState) {
            case SERVICE_STOPPED: out.state = State::Stopped; break;
            case SERVICE_START_PENDING: out.state = State::Starting; break;
            case SERVICE_RUNNING: out.state = State::Running; break;
            case SERVICE_STOP_PENDING: out.state = State::Stopping; break;
            default: break;
        }
    }
    DWORD needed = 0;
    QueryServiceConfigW(svc.h, nullptr, 0, &needed);
    std::vector<std::uint8_t> buf(needed);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    if (needed > 0 && QueryServiceConfigW(svc.h, cfg, needed, &needed) && cfg->lpBinaryPathName != nullptr) {
        std::wstring path = cfg->lpBinaryPathName;
        if (!path.empty() && path.front() == L'"') {
            path = path.substr(1, path.find(L'"', 1) - 1);
        } else {
            path = path.substr(0, path.find(L' '));
        }
        out.binary = path;
    }
    return out;
}

platform::Result<void> install() {
    if (!sys::is_elevated()) return win32_error("Instalar el servicio requiere administrador", ERROR_ACCESS_DENIED);
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    if (!scm) return win32_error("OpenSCManager");

    // Detener la versión anterior para poder reemplazar el ejecutable.
    ScHandle existing(OpenServiceW(scm.h, kServiceName, SERVICE_ALL_ACCESS));
    if (existing) stop_service(existing.h);

    const std::filesystem::path dir = data_dir();
    if (auto r = secure_data_dir(dir); !r) return r;
    import_user_data(dir);

    const std::filesystem::path target = installed_exe();
    const std::filesystem::path self = sys::exe_path();
    std::error_code ec;
    if (!std::filesystem::equivalent(self, target, ec) && !copy_with_retry(self, target)) {
        return win32_error("Copiar LanCast.exe a Archivos de programa");
    }

    const std::wstring command = L"\"" + target.wstring() + L"\" --service";
    if (existing) {
        if (!ChangeServiceConfigW(existing.h, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(),
                                  nullptr, nullptr, nullptr, nullptr, nullptr, kDisplayName)) {
            return win32_error("ChangeServiceConfig");
        }
    } else {
        existing.h = CreateServiceW(scm.h, kServiceName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                    SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
                                    nullptr, nullptr);
        if (!existing) return win32_error("CreateService");
    }
    SERVICE_DESCRIPTIONW desc{const_cast<LPWSTR>(kDescription)};
    ChangeServiceConfig2W(existing.h, SERVICE_CONFIG_DESCRIPTION, &desc);
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 24 * 3600;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    ChangeServiceConfig2W(existing.h, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    enable_software_sas();
    set_firewall(target, true);

    if (!StartServiceW(existing.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        return win32_error("StartService");
    }
    wait_for_state(existing.h, SERVICE_RUNNING, 10000);
    log::info("Servicio instalado en {}", platform::detail::to_utf8(target.wstring()));
    return {};
}

platform::Result<void> uninstall() {
    if (!sys::is_elevated()) return win32_error("Desinstalar el servicio requiere administrador", ERROR_ACCESS_DENIED);
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS));
    if (!scm) return win32_error("OpenSCManager");
    ScHandle svc(OpenServiceW(scm.h, kServiceName, SERVICE_ALL_ACCESS));
    if (svc) {
        stop_service(svc.h);
        if (!DeleteService(svc.h) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) return win32_error("DeleteService");
    }
    set_firewall({}, false);
    disable_software_sas();
    const std::filesystem::path target = installed_exe();
    std::error_code ec;
    if (!std::filesystem::equivalent(sys::exe_path(), target, ec)) {
        for (int i = 0; i < 20 && std::filesystem::exists(target, ec); ++i) {
            if (DeleteFileW(target.c_str())) break;
            Sleep(250);
        }
        std::filesystem::remove(target.parent_path(), ec);  // Solo si quedó vacía.
    }
    log::info("Servicio desinstalado");
    return {};
}

platform::Result<int> run_elevated(const std::wstring& argument) {
    const std::wstring exe = sys::exe_path().wstring();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = argument.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) return win32_error("Elevación cancelada o rechazada");
    if (info.hProcess == nullptr) return 0;
    WaitForSingleObject(info.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return static_cast<int>(code);
}

int run_service() {
    std::error_code ec;
    std::filesystem::create_directories(data_dir(), ec);
    log::set_file(data_dir() / L"service.log");
    wchar_t name[] = L"LanCastHost";
    SERVICE_TABLE_ENTRYW table[] = {{name, service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        log::error("--service solo lo puede iniciar el administrador de servicios de Windows ({})", GetLastError());
        return 1;
    }
    return 0;
}

bool request_secure_attention() {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kSasEvent);
    if (ev == nullptr) return false;
    const bool ok = SetEvent(ev) != FALSE;
    CloseHandle(ev);
    return ok;
}

void* open_agent_stop_event() { return OpenEventW(SYNCHRONIZE, FALSE, kAgentStopEvent); }

}  // namespace lancast::service
