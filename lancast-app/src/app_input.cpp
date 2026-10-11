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

// ------------------------------------------------------------------------------------------------ teclado

bool App::keyboard_hook(WPARAM wp, const KBDLLHOOKSTRUCT& k) {
    const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    // Estado propio de los modificadores: cuando las teclas se reenvían al host se "tragan" y Windows no
    // las registra, así que GetAsyncKeyState no sirve para detectar los atajos.
    switch (k.vkCode) {
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_CONTROL:
            mod_ctrl_[k.vkCode == VK_RCONTROL ? 1 : 0] = down;
            break;
        case VK_LMENU:
        case VK_RMENU:
        case VK_MENU:
            mod_alt_[k.vkCode == VK_RMENU ? 1 : 0] = down;
            break;
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_SHIFT:
            mod_shift_[k.vkCode == VK_RSHIFT ? 1 : 0] = down;
            break;
        default:
            break;
    }
    if (!streaming() || GetForegroundWindow() != hwnd_) return false;
    const bool ctrl = mod_ctrl_[0] || mod_ctrl_[1];
    const bool alt = mod_alt_[0] || mod_alt_[1];
    const bool shift = mod_shift_[0] || mod_shift_[1];
    if (ctrl && alt && shift) {
        const DWORD vk = k.vkCode;
        bool handled = true;
        if (vk >= '0' && vk <= '9') {
            if (down) switch_monitor(vk - '0');
        } else if (vk == 'S') {
            if (down) {
                show_stats_ = !show_stats_;
                settings_.client.show_stats = show_stats_;
                save_settings();
            }
        } else if (vk == 'M') {
            if (down) show_menu_ = !show_menu_;
        } else if (vk == 'F') {
            if (down) toggle_fullscreen();
        } else if (vk == 'Q') {
            if (down) PostMessageW(hwnd_, detail::kMsgClientDisconnect, 0, 0);
        } else if (vk == 'R') {
            if (down) set_relative_mouse(!relative_mouse_);
        } else if (vk == 'V') {
            if (down) {
                settings_.client.view_only = !settings_.client.view_only;
                toast(settings_.client.view_only ? "Modo solo ver" : "Control activado");
            }
        } else {
            handled = false;
        }
        if (handled) {
            if (session_) session_->send_message(proto::encode(proto::MsgType::ReleaseAll));
            return true;
        }
    }
    if (!control_enabled() || show_menu_ || pin_modal_open_) return false;
    if ((k.flags & LLKHF_INJECTED) != 0) return false;  // Evita bucles si host y cliente están en el mismo equipo.
    session_->send_message(proto::encode(proto::MsgType::Key, proto::Key{static_cast<std::uint16_t>(k.scanCode),
                                                                         (k.flags & LLKHF_EXTENDED) != 0, down}));
    return true;
}

// ------------------------------------------------------------------------------------------------ WndProc

LRESULT App::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool forward_input = streaming() && control_enabled() && !ui_captures_mouse();
    if (ImGui::GetCurrentContext() != nullptr && !(streaming() && !show_menu_ && !pin_modal_open_ &&
                                                   (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR))) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    }

    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                resize_pending_ = true;
                pending_w_ = LOWORD(lp);
                pending_h_ = HIWORD(lp);
            }
            return 0;
        case WM_DPICHANGED: {
            const auto* rc = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            apply_dpi(HIWORD(wp));
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = POINT{MulDiv(640, static_cast<int>(dpi_), 96), MulDiv(420, static_cast<int>(dpi_), 96)};
            return 0;
        }
        case WM_CLOSE:
            if (settings_.minimize_to_tray && settings_.host.enabled && tray_added_ && !streaming()) {
                ShowWindow(hwnd, SW_HIDE);
                notify(L"LanCast sigue activo", L"Este equipo puede recibir conexiones. Clic derecho en el icono para salir.");
            } else {
                running_ = false;
            }
            return 0;
        case WM_DESTROY:
            running_ = false;
            return 0;
        case kTrayMessage:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                case NIN_SELECT:
                    ShowWindow(hwnd, SW_SHOW);
                    ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    tray_menu();
                    break;
                default:
                    break;
            }
            return 0;
        case kMsgBannerDisconnect:
            host_.disconnect();
            return 0;
        case kMsgClientDisconnect:
            disconnect_client();
            toast("Desconectado");
            return 0;
        case WM_CLIPBOARDUPDATE:
            if (auto text = input::get_clipboard_text(hwnd)) {
                host_.on_local_clipboard(*text);
                if (session_ && session_->state() == client::State::Connected && settings_.client.clipboard_sync &&
                    *text != last_remote_clipboard_) {
                    session_->send_message(proto::encode(proto::MsgType::Clipboard, proto::TextMessage{*text}));
                }
            }
            return 0;
        case WM_DROPFILES: {
            auto drop = reinterpret_cast<HDROP>(wp);
            std::vector<std::filesystem::path> paths;
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH * 4]{};
                DragQueryFileW(drop, i, path, static_cast<UINT>(std::size(path)));
                paths.emplace_back(path);
            }
            DragFinish(drop);
            if (session_ && session_->files().connected()) {
                session_->files().send(paths);
                toast(std::format("Enviando {} elemento(s) a {}", paths.size(), session_->host_name()));
            } else if (host_.status().session_active && host_.files().connected()) {
                host_.files().send(paths);
                toast("Enviando al cliente conectado");
            } else {
                toast("Conéctate a otro equipo para enviarle archivos");
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && streaming() && control_enabled() && !relative_mouse_ &&
                !ui_captures_mouse()) {
                SetCursor(remote_cursor_visible_ ? (remote_cursor_ ? remote_cursor_ : LoadCursorW(nullptr, IDC_ARROW)) : nullptr);
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            if (forward_input && !relative_mouse_) {
                pending_mouse_ = PointF{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
                // Sin esperar al bucle de dibujo: se envía ya (como mucho 1000 veces por segundo).
                if (gpu::now_us() - last_mouse_send_us_ >= 1000) send_mouse_position();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            if (forward_input) pending_mouse_ = PointF{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
            handle_mouse_button(msg, wp);
            return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP ? TRUE : 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (forward_input) {
                session_->send_message(proto::encode(
                    proto::MsgType::MouseWheel, proto::MouseWheel{GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL}));
            }
            return 0;
        case WM_INPUT:
            if (relative_mouse_ && streaming() && control_enabled()) {
                RAWINPUT raw{};
                UINT size = sizeof(raw);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                        static_cast<UINT>(-1) &&
                    raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
                    (raw.data.mouse.lLastX != 0 || raw.data.mouse.lLastY != 0)) {
                    session_->send_message(proto::encode(proto::MsgType::MouseRel,
                                                         proto::MouseRel{raw.data.mouse.lLastX, raw.data.mouse.lLastY}));
                }
            }
            break;
        case WM_KILLFOCUS:
        case WM_ACTIVATEAPP:
            if (msg == WM_KILLFOCUS || wp == FALSE) {
                mod_ctrl_[0] = mod_ctrl_[1] = mod_alt_[0] = mod_alt_[1] = mod_shift_[0] = mod_shift_[1] = false;
                if (session_ && session_->state() == client::State::Connected) {
                    session_->send_message(proto::encode(proto::MsgType::ReleaseAll));
                }
                if (relative_mouse_) set_relative_mouse(false);
            }
            break;
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            if (streaming()) return 0;  // Evita que Alt abra el menú de la ventana.
            break;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace lancast::app
