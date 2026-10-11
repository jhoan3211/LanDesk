// LanCast.exe: un único ejecutable que es host y cliente a la vez.
//
//   LanCast.exe                      Abre la interfaz.
//   LanCast.exe --minimized          Arranca oculto en la bandeja (inicio automático).
//   LanCast.exe --connect IP[:port]  Conecta al iniciar.
//   LanCast.exe --no-host            Solo cliente (permite una segunda instancia en el mismo PC).
//   LanCast.exe --list [--json]      Inventario de GPU, monitores y códecs en la consola.

#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>

#include <cstdio>
#include <string>

#include "app.hpp"
#include "lancast/core/log.hpp"
#include "lancast/core/report.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/net.hpp"
#include "lancast/platform/platform.hpp"
#include "lancast/platform/system.hpp"

namespace {

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

int run_list(bool json, bool verbose) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    SetConsoleOutputCP(CP_UTF8);
    lancast::platform::enable_per_monitor_dpi_awareness();
    const lancast::SystemReport report = lancast::platform::collect_system_report();
    const std::string text = json ? lancast::format_report_json(report) : lancast::format_report_text(report, verbose);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    return report.gpus.empty() ? 1 : 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    lancast::app::LaunchOptions opts;
    bool list = false;
    bool json = false;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--list" || a == L"-l") {
            list = true;
        } else if (a == L"--json") {
            json = true;
        } else if (a == L"--verbose" || a == L"-v") {
            verbose = true;
        } else if (a == L"--minimized") {
            opts.minimized = true;
        } else if (a == L"--no-host") {
            opts.no_host = true;
        } else if (a == L"--connect" && i + 1 < argc) {
            opts.connect = narrow(argv[++i]);
        } else if (a == L"--pin" && i + 1 < argc) {
            opts.pin = narrow(argv[++i]);
        }
    }
    LocalFree(argv);
    if (list) return run_list(json, verbose);

    // Una sola instancia con host (una segunda con --no-host es válida para probar en el mismo PC).
    HANDLE instance = nullptr;
    if (!opts.no_host) {
        instance = CreateMutexW(nullptr, TRUE, L"Local\\LanCastHostInstance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND other = FindWindowW(L"LanCastMain", nullptr)) {
                ShowWindow(other, SW_SHOW);
                ShowWindow(other, SW_RESTORE);
                SetForegroundWindow(other);
            }
            return 0;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    lancast::platform::enable_per_monitor_dpi_awareness();
    lancast::log::set_file(lancast::sys::app_data_dir() / (opts.no_host ? L"lancast-client.log" : L"lancast.log"));
    lancast::log::info("LanCast 1.0.0 iniciado");
    timeBeginPeriod(1);  // Temporizadores de 1 ms para el ritmo de captura y presentación.

    int code = 0;
    {
        lancast::net::WinsockScope winsock;
        lancast::mf::MfScope mf;
        lancast::app::App app(opts);
        code = app.run();
    }
    timeEndPeriod(1);
    CoUninitialize();
    if (instance != nullptr) CloseHandle(instance);
    return code;
}
