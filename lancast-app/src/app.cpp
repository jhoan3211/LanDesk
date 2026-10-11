#include "app.hpp"

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
namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kBannerButton = 100;
App* g_app = nullptr;

std::wstring widen(std::string_view s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app != nullptr) return g_app->wnd_proc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK keyboard_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_app != nullptr && g_app->keyboard_hook(wp, *reinterpret_cast<KBDLLHOOKSTRUCT*>(lp))) return 1;
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK banner_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == kBannerButton && g_app != nullptr) PostMessageW(GetWindow(hwnd, GW_OWNER), WM_APP + 2, 0, 0);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(178, 34, 34));
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            wchar_t text[256]{};
            GetWindowTextW(hwnd, text, 256);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            HFONT font = reinterpret_cast<HFONT>(GetPropW(hwnd, L"font"));
            HGDIOBJ old = SelectObject(dc, font);
            RECT tr = rc;
            tr.left += MulDiv(14, GetDpiForWindow(hwnd), 96);
            tr.right -= MulDiv(120, GetDpiForWindow(hwnd), 96);
            DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST:
            return HTCAPTION;  // Se puede arrastrar.
        case WM_DESTROY:
            if (HFONT f = reinterpret_cast<HFONT>(RemovePropW(hwnd, L"font")); f != nullptr) DeleteObject(f);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

App::App(LaunchOptions options) : options_(std::move(options)) {
    g_app = this;
    settings_ = sys::load_settings();
    if (options_.no_host) settings_.host.enabled = false;
    if (!settings_.client.recent_addresses.empty()) address_input_ = settings_.client.recent_addresses.front();
    show_stats_ = settings_.client.show_stats;
    relative_mouse_ = false;
}

App::~App() {
    discovery_running_ = false;
    if (discovery_thread_.joinable()) discovery_thread_.join();
    disconnect_client();
    host_.stop();
    if (keyboard_hook_ != nullptr) UnhookWindowsHookEx(keyboard_hook_);
    if (remote_cursor_ != nullptr) DestroyCursor(remote_cursor_);
    if (banner_ != nullptr) DestroyWindow(banner_);
    remove_tray();
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    if (hwnd_ != nullptr) DestroyWindow(hwnd_);
    g_app = nullptr;
}

// ------------------------------------------------------------------------------------------------ ventana

bool App::create_window() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_LANCAST));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LanCastMain";
    RegisterClassExW(&wc);

    WNDCLASSEXW bc{sizeof(bc)};
    bc.lpfnWndProc = banner_proc;
    bc.hInstance = inst;
    bc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    bc.lpszClassName = L"LanCastBanner";
    RegisterClassExW(&bc);

    const HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96;
    UINT dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    const int w = MulDiv(1180, static_cast<int>(dx), 96);
    const int h = MulDiv(780, static_cast<int>(dx), 96);
    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"LanCast", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, w, h, nullptr, nullptr, inst, nullptr);
    if (hwnd_ == nullptr) return false;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    dpi_ = GetDpiForWindow(hwnd_);
    AddClipboardFormatListener(hwnd_);
    host_.set_window(hwnd_);

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;  // Mouse
    rid.hwndTarget = hwnd_;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    return true;
}

bool App::create_swapchain() {
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allow = FALSE;
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow));
        tearing_supported_ = allow != FALSE;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.device.Get(), hwnd_, &desc, nullptr, nullptr, &swapchain_))) return false;
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi_device;
    if (SUCCEEDED(device_.device.As(&dxgi_device))) dxgi_device->SetMaximumFrameLatency(1);

    RECT rc;
    GetClientRect(hwnd_, &rc);
    resize_swapchain(static_cast<UINT>(rc.right - rc.left), static_cast<UINT>(rc.bottom - rc.top));
    return true;
}

void App::resize_swapchain(UINT width, UINT height) {
    if (width == 0 || height == 0 || !swapchain_) return;
    rtv_.Reset();
    device_.context->OMSetRenderTargets(0, nullptr, nullptr);
    device_.context->Flush();
    swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bb;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb));
    device_.device->CreateRenderTargetView(bb.Get(), nullptr, &rtv_);
    backbuffer_size_ = Size{width, height};
}

void App::apply_dpi(UINT dpi) {
    dpi_ = dpi;
    scale_ = static_cast<float>(dpi) / 96.0f;
    set_style(scale_);
    update_session_banner();
}

void App::toggle_fullscreen() {
    const LONG style = GetWindowLongW(hwnd_, GWL_STYLE);
    if (!fullscreen_) {
        GetWindowPlacement(hwnd_, &saved_placement_);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = true;
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &saved_placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
    }
}

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

// ------------------------------------------------------------------------------------------------ cliente

bool App::streaming() const {
    return session_ && session_->state() == client::State::Connected && session_->stream().has_value();
}

bool App::control_enabled() const {
    if (!session_ || settings_.client.view_only) return false;
    const auto info = session_->host_info();
    return info && info->input_allowed;
}

proto::StreamConfig App::make_stream_config() const {
    const StreamSettings& s = settings_.client.stream;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    const Size native{static_cast<std::uint32_t>(mi.rcMonitor.right - mi.rcMonitor.left),
                      static_cast<std::uint32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top)};
    const Size req = requested_size(s, native);
    proto::StreamConfig c;
    c.monitor_id = s.monitor_id;
    c.max_width = req.width;
    c.max_height = req.height;
    c.fps = s.fps;
    c.codec = s.codec;
    c.auto_bitrate = s.auto_bitrate;
    c.bitrate_kbps = s.bitrate_mbps * 1000;
    c.rate_control = s.rate_control;
    c.chroma_444 = s.chroma_444;
    c.hdr = s.hdr;
    c.audio = s.audio;
    c.fec_percent = static_cast<std::uint8_t>(s.fec_percent);
    return c;
}

void App::apply_stream_settings() {
    save_settings();
    if (session_ && session_->state() == client::State::Connected) session_->request_stream(make_stream_config());
}

void App::switch_monitor(std::uint32_t id) {
    if (!session_) return;
    const auto info = session_->host_info();
    if (id != 0 && (!info || std::ranges::none_of(info->monitors, [&](const proto::RemoteMonitor& m) { return m.id == id; }))) {
        return;
    }
    settings_.client.stream.monitor_id = id;
    apply_stream_settings();
    toast(id == 0 ? "Todos los monitores" : std::format("Monitor {}", id));
}

void App::start_connect(const std::string& address) {
    const auto ep = net::Endpoint::parse(address, kDefaultControlPort);
    if (!ep) {
        toast("Dirección no válida. Usa IP o IP:puerto, por ejemplo 192.168.40.7");
        return;
    }
    disconnect_client();
    session_ = std::make_unique<client::ClientSession>(settings_);

    proto::ClientCaps caps;
    caps.client_name = settings_.device_name;
    caps.decoders = mf::available_decoders();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    caps.display_width = static_cast<std::uint32_t>(mi.rcMonitor.right - mi.rcMonitor.left);
    caps.display_height = static_cast<std::uint32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    MONITORINFOEXW mix{};
    mix.cbSize = sizeof(mix);
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mix);
    if (EnumDisplaySettingsW(mix.szDevice, ENUM_CURRENT_SETTINGS, &dm)) caps.display_refresh = dm.dmDisplayFrequency;
    if (caps.decoders.empty()) {
        toast("Este equipo no tiene decodificador de video por hardware");
        session_.reset();
        return;
    }
    session_->connect(*ep, caps, make_stream_config());
    last_client_state_ = client::State::Connecting;

    auto& recent = settings_.client.recent_addresses;
    std::erase(recent, address);
    recent.insert(recent.begin(), address);
    if (recent.size() > 8) recent.resize(8);
    save_settings();
}

void App::disconnect_client() {
    if (keyboard_hook_ != nullptr) {
        UnhookWindowsHookEx(keyboard_hook_);
        keyboard_hook_ = nullptr;
    }
    set_relative_mouse(false);
    if (session_) {
        session_->disconnect();
        session_.reset();
    }
    renderer_.reset();
    show_menu_ = false;
    pin_modal_open_ = false;
    frame_presented_since_stream_ = false;
    last_stream_epoch_ = 0xFFFF;
    if (fullscreen_) toggle_fullscreen();
}

void App::set_relative_mouse(bool on) {
    relative_mouse_ = on;
    if (on) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        ClipCursor(&rc);
        while (ShowCursor(FALSE) >= 0) {
        }
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {
        }
    }
}

void App::send_mouse_position() {
    if (!pending_mouse_ || !streaming() || !control_enabled() || relative_mouse_) {
        pending_mouse_.reset();
        return;
    }
    if (show_menu_ && ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) {
        pending_mouse_.reset();
        return;
    }
    const auto stream = session_->stream();
    const Rect view = view_rect({stream->width, stream->height}, backbuffer_size_, settings_.client.stream.scaling);
    if (const auto n = window_to_normalized(*pending_mouse_, view)) {
        session_->send_message(proto::encode(proto::MsgType::MouseAbs, proto::MouseAbs{n->x, n->y}));
    }
    pending_mouse_.reset();
}

void App::handle_mouse_button(UINT msg, WPARAM wp) {
    if (!streaming() || !control_enabled()) return;
    if (show_menu_ && ImGui::GetIO().WantCaptureMouse) return;
    proto::MouseButton b;
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            b.button = proto::MouseButtonId::Left;
            break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            b.button = proto::MouseButtonId::Right;
            break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            b.button = proto::MouseButtonId::Middle;
            break;
        default:
            b.button = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? proto::MouseButtonId::X1 : proto::MouseButtonId::X2;
            break;
    }
    b.down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
    if (b.down) {
        SetCapture(hwnd_);
    } else {
        ReleaseCapture();
    }
    send_mouse_position();  // Primero la posición, luego el clic.
    session_->send_message(proto::encode(proto::MsgType::MouseButton, b));
}

void App::process_client() {
    if (!session_) return;
    const client::State st = session_->state();
    if (st != last_client_state_) {
        switch (st) {
            case client::State::NeedPin:
                pin_input_.clear();
                pin_modal_open_ = true;
                if (options_.pin) {
                    session_->submit_pin(*options_.pin);
                    pin_modal_open_ = false;
                }
                break;
            case client::State::Connected:
                pin_modal_open_ = false;
                renderer_ = std::make_unique<VideoRenderer>(device_);
                if (keyboard_hook_ == nullptr) keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, GetModuleHandleW(nullptr), 0);
                toast(std::format("Conectado a {} · Ctrl+Alt+Shift+M abre el menú", session_->host_name()));
                break;
            case client::State::Failed:
            case client::State::Closed: {
                const std::string msg = session_->message();
                pin_modal_open_ = false;
                const std::string name = session_->host_name();
                disconnect_client();
                toast(msg.empty() ? std::format("Desconectado de {}", name) : msg);
                last_client_state_ = client::State::Idle;
                return;
            }
            default:
                break;
        }
        last_client_state_ = st;
    }
    if (st != client::State::Connected) return;

    if (auto shape = session_->take_cursor_shape()) {
        if (remote_cursor_ != nullptr) DestroyCursor(remote_cursor_);
        remote_cursor_ = input::create_cursor(*shape);
        POINT pt;
        GetCursorPos(&pt);
        SetCursorPos(pt.x, pt.y);  // Fuerza WM_SETCURSOR.
    }
    remote_cursor_visible_ = session_->cursor().visible;
    if (auto text = session_->take_clipboard(); text && settings_.client.clipboard_sync) {
        last_remote_clipboard_ = *text;
        input::set_clipboard_text(hwnd_, *text);
    }
    if (const auto stream = session_->stream(); stream && stream->epoch != last_stream_epoch_) {
        last_stream_epoch_ = stream->epoch;
        if (!stream->notes.empty()) toast(stream->notes);
    }
    session_->set_simulated_loss(settings_.client.simulate_loss_percent);
}

// ------------------------------------------------------------------------------------------------ teclado

bool App::keyboard_hook(WPARAM wp, const KBDLLHOOKSTRUCT& k) {
    if (!streaming() || GetForegroundWindow() != hwnd_) return false;
    const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
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
            if (down) PostMessageW(hwnd_, WM_APP + 3, 0, 0);
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
    const bool forward_input = streaming() && control_enabled() && !(show_menu_ && ImGui::GetCurrentContext() &&
                                                                      ImGui::GetIO().WantCaptureMouse);
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
        case WM_APP + 2:  // Botón "Desconectar" del aviso de sesión.
            host_.disconnect();
            return 0;
        case WM_APP + 3:  // Ctrl+Alt+Shift+Q
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
                !(show_menu_ && ImGui::GetIO().WantCaptureMouse)) {
                SetCursor(remote_cursor_visible_ ? (remote_cursor_ ? remote_cursor_ : LoadCursorW(nullptr, IDC_ARROW)) : nullptr);
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            if (forward_input && !relative_mouse_) {
                pending_mouse_ = PointF{static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
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

// ------------------------------------------------------------------------------------------------ bucle

int App::run() {
    if (!create_window()) return 1;
    auto dev = gpu::create_device(std::nullopt);
    if (!dev) {
        MessageBoxW(hwnd_, widen(dev.error().message()).c_str(), L"LanCast", MB_ICONERROR);
        return 1;
    }
    device_ = std::move(*dev);
    if (!create_swapchain()) {
        MessageBoxW(hwnd_, L"No se pudo crear la swapchain de Direct3D 11.", L"LanCast", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    wchar_t windir[MAX_PATH]{};
    GetWindowsDirectoryW(windir, MAX_PATH);
    const std::string fonts = narrow(windir) + "\\Fonts\\";
    font_ = io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), 17.0f);
    if (font_ == nullptr) font_ = io.Fonts->AddFontDefault();
    font_big_ = io.Fonts->AddFontFromFileTTF((fonts + "segoeuib.ttf").c_str(), 17.0f);
    if (font_big_ == nullptr) font_big_ = font_;
    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.device.Get(), device_.context.Get());
    apply_dpi(GetDpiForWindow(hwnd_));

    create_tray();
    host_.start(settings_);
    report_ = platform::collect_system_report();

    ShowWindow(hwnd_, options_.minimized ? SW_HIDE : SW_SHOW);
    if (!options_.minimized) UpdateWindow(hwnd_);
    if (options_.connect) start_connect(*options_.connect);

    std::uint64_t last_host_poll = 0;
    while (running_) {
        HANDLE ev = session_ ? static_cast<HANDLE>(session_->frame_event()) : nullptr;
        const bool visible = IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
        DWORD timeout = 16;
        if (!visible) timeout = 200;
        MsgWaitForMultipleObjectsEx(ev ? 1 : 0, ev ? &ev : nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) running_ = false;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!running_) break;

        send_mouse_position();
        process_client();

        const std::uint64_t now = gpu::now_us();
        if (now - last_host_poll > 250'000) {
            last_host_poll = now;
            const host::Status st = host_.status();
            if (st.session_active != last_session_active_) {
                last_session_active_ = st.session_active;
                if (st.session_active) {
                    notify(L"Sesión activa", widen(std::format("{} ({}) está viendo este equipo", st.client_name, st.client_address)));
                } else {
                    notify(L"Sesión terminada", L"Nadie está conectado a este equipo.");
                }
                update_session_banner();
            }
            if (st.pin && *st.pin != last_pin_) {
                notify(L"Solicitud de emparejamiento", widen(std::format("{} pide conectarse. PIN: {}", st.pin_requester, *st.pin)));
                page_ = Page::Host;
                if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOW);
                ShowWindow(hwnd_, SW_RESTORE);
                SetForegroundWindow(hwnd_);
            }
            last_pin_ = st.pin.value_or("");
            if (tray_added_) {
                NOTIFYICONDATAW nid{sizeof(nid)};
                nid.hWnd = hwnd_;
                nid.uID = kTrayId;
                nid.uFlags = NIF_TIP;
                const std::wstring tip = st.session_active ? widen(std::format("LanCast · Sesión con {}", st.client_name))
                                                           : (st.listening ? L"LanCast · Esperando conexiones" : L"LanCast");
                wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
                Shell_NotifyIconW(NIM_MODIFY, &nid);
            }
        }
        if (settings_dirty_ && now - settings_dirty_since_ > 1'000'000) {
            sys::save_settings(settings_);
            host_.apply_settings(settings_);
            settings_dirty_ = false;
        }
        if (visible) render();
    }
    if (settings_dirty_) sys::save_settings(settings_);
    return 0;
}

void App::render() {
    if (resize_pending_) {
        resize_pending_ = false;
        resize_swapchain(pending_w_, pending_h_);
        if (relative_mouse_) set_relative_mouse(true);
    }

    // Decodificar todo lo recibido (los P-frames dependen de los anteriores) y presentar el último.
    bool new_video = false;
    client::Frame last_frame;
    double decode_ms = 0;
    if (streaming() && renderer_) {
        const auto stream = session_->stream();
        for (auto& f : session_->take_frames()) {
            double ms = 0;
            if (renderer_->decode(f, *stream, ms)) {
                new_video = true;
                decode_ms = ms;
                last_frame = std::move(f);
            }
        }
        if (renderer_->needs_keyframe()) session_->request_keyframe();
    }

    const std::uint64_t now = gpu::now_us();
    // En "solo ver" el cursor remoto se dibuja encima, así que hay que redibujar aunque no llegue video.
    const bool ui_active = !streaming() || show_menu_ || show_stats_ || pin_modal_open_ || now < toast_until_us_ ||
                           !control_enabled();
    if (streaming() && !new_video && !ui_active && now - last_present_us_ < 100'000 && frame_presented_since_stream_) return;
    if (streaming() && !new_video && now - last_present_us_ < 8'000) return;  // Máx. ~120 Hz de interfaz sin video.

    const std::uint64_t t0 = gpu::now_us();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> bb;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb));
    bool drew_video = false;
    if (streaming() && renderer_ && renderer_->has_picture()) {
        const auto stream = session_->stream();
        const Rect view = view_rect({stream->width, stream->height}, backbuffer_size_, settings_.client.stream.scaling);
        drew_video = renderer_->draw(bb.Get(), backbuffer_size_, view);
    }
    if (!drew_video) {
        const float bg[4] = {0.075f, 0.082f, 0.098f, 1.0f};
        const float black[4] = {0, 0, 0, 1};
        device_.context->ClearRenderTargetView(rtv_.Get(), streaming() ? black : bg);
    }
    ID3D11RenderTargetView* rtv = rtv_.Get();
    device_.context->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    const bool vsync = !streaming() || settings_.client.stream.vsync;
    UINT flags = 0;
    if (!vsync && tearing_supported_) flags |= DXGI_PRESENT_ALLOW_TEARING;
    swapchain_->Present(vsync ? 1 : 0, flags);
    last_present_us_ = gpu::now_us();

    if (new_video) {
        frame_presented_since_stream_ = true;
        session_->report_presented(last_frame.frame.info, last_frame.arrival_us, decode_ms, (last_present_us_ - t0) / 1000.0);
    }
}

void App::save_settings() {
    settings_dirty_ = true;
    settings_dirty_since_ = gpu::now_us();
}

void App::toast(std::string text) {
    toast_ = std::move(text);
    toast_until_us_ = gpu::now_us() + 4'000'000;
}

}  // namespace lancast::app
