// Modo agente ("LanCast.exe --agent"): lo lanza el servicio de Windows como SYSTEM dentro de la sesión de
// consola. Ejecuta solo el host (sin interfaz) con la configuración de %ProgramData%\LanCast; cuando un equipo
// nuevo pide emparejarse muestra el PIN en una ventana pequeña siempre visible.

#include "agent.hpp"

#include <windows.h>
#include <timeapi.h>

#include <format>
#include <string>

#include "lancast/core/log.hpp"
#include "lancast/core/version.hpp"
#include "lancast/host/host_service.hpp"
#include "lancast/platform/crypto.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/net.hpp"
#include "lancast/platform/platform.hpp"
#include "lancast/platform/service.hpp"
#include "lancast/platform/system.hpp"

namespace lancast::app {
namespace {

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Ventana mínima con el PIN (GDI, sin ImGui: el agente no tiene interfaz).
class PinPopup {
public:
    ~PinPopup() { hide(); }

    void show(const std::string& pin, const std::string& requester) {
        const std::wstring text =
            widen(std::format("{} quiere conectarse a este equipo.\nEscribe este PIN en {}:", requester, requester));
        const std::wstring code = widen(pin.size() == 6 ? pin.substr(0, 3) + " " + pin.substr(3) : pin);
        if (hwnd_ != nullptr && text == text_ && code == code_) return;
        text_ = text;
        code_ = code;
        if (hwnd_ == nullptr) create();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void hide() {
        if (hwnd_ != nullptr) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        if (font_ != nullptr) DeleteObject(font_);
        if (font_big_ != nullptr) DeleteObject(font_big_);
        font_ = font_big_ = nullptr;
    }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* self = reinterpret_cast<PinPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_PAINT && self != nullptr) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(230, 230, 235));
            SelectObject(dc, self->font_);
            RECT top = rc;
            top.left += 16;
            top.right -= 16;
            top.top += 14;
            top.bottom = rc.bottom / 2;
            DrawTextW(dc, self->text_.c_str(), -1, &top, DT_CENTER | DT_WORDBREAK);
            SetTextColor(dc, RGB(90, 160, 255));
            SelectObject(dc, self->font_big_);
            RECT bottom = rc;
            bottom.top = rc.bottom / 2;
            DrawTextW(dc, self->code_.c_str(), -1, &bottom, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (msg == WM_CLOSE) return 0;  // Se cierra sola al terminar el emparejamiento.
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void create() {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"LanCastAgentPin";
        RegisterClassW(&wc);
        const UINT dpi = GetDpiForSystem();
        const int w = MulDiv(420, static_cast<int>(dpi), 96);
        const int h = MulDiv(210, static_cast<int>(dpi), 96);
        font_ = CreateFontW(-MulDiv(16, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, 0, L"Segoe UI");
        font_big_ = CreateFontW(-MulDiv(44, static_cast<int>(dpi), 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0,
                                0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"LanCast · Emparejar",
                                WS_POPUP | WS_CAPTION | WS_VISIBLE, work.right - w - 24, work.bottom - h - 24, w, h, nullptr,
                                nullptr, inst, this);
    }

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT font_big_ = nullptr;
    std::wstring text_;
    std::wstring code_;
};

}  // namespace

int run_agent() {
    // Configuración y emparejamientos del servicio (carpeta protegida, DPAPI de equipo).
    const auto dir = service::data_dir();
    SetEnvironmentVariableW(L"LANCAST_DATA_DIR", dir.c_str());
    crypto::set_machine_scope(true);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    platform::enable_per_monitor_dpi_awareness();
    log::set_file(dir / L"agent.log");
    log::info("LanCast {} agente iniciado (sesión {})", kAppVersion, WTSGetActiveConsoleSessionId());
    timeBeginPeriod(1);

    HANDLE stop = static_cast<HANDLE>(service::open_agent_stop_event());
    {
        net::WinsockScope winsock;
        mf::MfScope mf;
        host::HostService host;
        host.set_service_mode(true);
        AppSettings settings = sys::load_settings();
        settings.host.enabled = true;
        host.start(settings);
        PinPopup popup;
        bool running = true;
        while (running) {
            const DWORD r = MsgWaitForMultipleObjects(stop != nullptr ? 1 : 0, stop != nullptr ? &stop : nullptr, FALSE, 250,
                                                      QS_ALLINPUT);
            if (stop != nullptr && r == WAIT_OBJECT_0) break;
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) running = false;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            const auto st = host.status();
            if (st.pin) {
                popup.show(*st.pin, st.pin_requester.empty() ? std::string{"Un equipo"} : st.pin_requester);
            } else {
                popup.hide();
            }
        }
        log::info("Agente detenido");
        host.stop();
    }
    if (stop != nullptr) CloseHandle(stop);
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}

}  // namespace lancast::app
