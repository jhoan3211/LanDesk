#include "app.hpp"
#include "app_internal.hpp"


#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <format>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "lancast/core/geometry.hpp"
#include "lancast/core/log.hpp"
#include "lancast/platform/input.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/platform.hpp"
#include "lancast/platform/system.hpp"
#include "resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace lancast::app {
using namespace detail;

// ------------------------------------------------------------------------------------------------ bandeja

void App::create_tray() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LANCAST));
    wcscpy_s(nid.szTip, L"LanCast");
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void App::remove_tray() {
    if (!tray_added_) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    tray_added_ = false;
}

void App::notify(const std::wstring& title, const std::wstring& text) {
    if (!tray_added_) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::tray_menu() {
    HMENU menu = CreatePopupMenu();
    const host::Status st = host_.status();
    AppendMenuW(menu, MF_STRING, 1, L"Abrir LanCast");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (st.session_active && host_.files().connected() ? 0 : MF_GRAYED), 2,
                L"Enviar archivos al cliente…");
    AppendMenuW(menu, MF_STRING | (st.session_active ? 0 : MF_GRAYED), 3, L"Desconectar la sesión");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"Salir");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case 1:
            ShowWindow(hwnd_, SW_SHOW);
            ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
            break;
        case 2: {
            const auto paths = sys::pick_files(hwnd_);
            if (!paths.empty()) host_.files().send(paths);
            break;
        }
        case 3:
            host_.disconnect();
            break;
        case 4:
            running_ = false;
            break;
        default:
            break;
    }
}

void App::update_session_banner() {
    const host::Status st = host_.status();
    if (!st.session_active) {
        if (banner_ != nullptr) {
            DestroyWindow(banner_);
            banner_ = nullptr;
        }
        return;
    }
    const std::wstring text = widen(std::format("LanCast · Sesión activa desde {} ({})", st.client_name, st.client_address));
    const HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi = 96;
    UINT dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
    const int w = MulDiv(520, static_cast<int>(dpi), 96);
    const int h = MulDiv(36, static_cast<int>(dpi), 96);
    if (banner_ == nullptr) {
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        banner_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"LanCastBanner", text.c_str(),
                                  WS_POPUP | WS_VISIBLE, (mi.rcWork.left + mi.rcWork.right - w) / 2, mi.rcWork.top + 4, w, h,
                                  hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        HFONT font = CreateFontW(-MulDiv(15, static_cast<int>(dpi), 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 L"Segoe UI");
        SetPropW(banner_, L"font", font);
        HWND button = CreateWindowExW(0, L"BUTTON", L"Desconectar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      w - MulDiv(110, static_cast<int>(dpi), 96), MulDiv(5, static_cast<int>(dpi), 96),
                                      MulDiv(102, static_cast<int>(dpi), 96), h - MulDiv(10, static_cast<int>(dpi), 96),
                                      banner_, reinterpret_cast<HMENU>(kBannerButton), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    } else {
        SetWindowTextW(banner_, text.c_str());
        InvalidateRect(banner_, nullptr, TRUE);
    }
}

}  // namespace lancast::app
