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

// ------------------------------------------------------------------------------------------------ cliente

bool App::streaming() const {
    return session_ && session_->state() == client::State::Connected && session_->stream().has_value();
}

bool App::ui_captures_mouse() const {
    // El mouse está sobre la barra superior, el menú u otra ventana de LanCast: no se envía al host.
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
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
    c.audio_rate = s.audio_sample_rate;
    c.audio_channels = s.audio_stereo ? 2 : 1;
    c.low_latency = s.low_latency;
    c.intra_refresh = s.intra_refresh;
    c.slices = s.slices;
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
    last_connect_address_ = address;
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
    caps.supports_slices = true;
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

void App::disconnect_client(bool keep_view) {
    second_.reset();
    second_monitor_id_ = 0;
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
    render_boost_.reset();
    presentation_mode_.clear();
    show_menu_ = false;
    pin_modal_open_ = false;
    frame_presented_since_stream_ = false;
    last_stream_epoch_ = 0xFFFF;
    if (!keep_view) {
        reconnecting_ = false;
        was_connected_ = false;
        if (fullscreen_) toggle_fullscreen();
    }
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
    if (ui_captures_mouse()) {
        pending_mouse_.reset();
        return;
    }
    const auto stream = session_->stream();
    const Rect view = view_rect({stream->width, stream->height}, backbuffer_size_, settings_.client.stream.scaling);
    if (const auto n = window_to_normalized(*pending_mouse_, view)) {
        session_->send_message(proto::encode(proto::MsgType::MouseAbs, proto::MouseAbs{n->x, n->y}));
        last_mouse_send_us_ = gpu::now_us();
    }
    pending_mouse_.reset();
}

void App::handle_mouse_button(UINT msg, WPARAM wp) {
    if (!streaming() || !control_enabled()) return;
    if (ui_captures_mouse()) return;
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
    if (!session_) {
        if (reconnecting_ && gpu::now_us() >= next_reconnect_us_) {
            ++reconnect_attempts_;
            next_reconnect_us_ = gpu::now_us() + 2'000'000;
            start_connect(reconnect_address_);
        }
        return;
    }
    const client::State st = session_->state();
    if (st != last_client_state_) {
        switch (st) {
            case client::State::NeedPin:
                reconnecting_ = false;  // El host ya no reconoce el emparejamiento: hace falta el PIN.
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
                render_boost_ = std::make_unique<sys::RealtimeThread>(L"Games");
                if (reconnecting_) toast(std::format("Reconectado a {}", session_->host_name()));
                reconnecting_ = false;
                was_connected_ = true;
                reconnect_address_ = last_connect_address_;
                reconnect_host_name_ = session_->host_name();
                if (keyboard_hook_ == nullptr) keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, GetModuleHandleW(nullptr), 0);
                toast(std::format("Conectado a {} · Ctrl+Alt+Shift+M abre el menú", session_->host_name()));
                break;
            case client::State::Failed:
            case client::State::Closed: {
                const std::string msg = session_->message();
                pin_modal_open_ = false;
                const std::string name = session_->host_name();
                const std::uint64_t now = gpu::now_us();
                // Corte de red con una sesión que funcionaba: reintentar sin PIN hasta 60 s.
                if (session_->should_retry() && (was_connected_ || reconnecting_) && !reconnect_address_.empty()) {
                    if (!reconnecting_) {
                        reconnecting_ = true;
                        reconnect_attempts_ = 0;
                        reconnect_deadline_us_ = now + 60'000'000;
                        log::info("Conexión perdida con {}: reconectando", name);
                    }
                    if (now < reconnect_deadline_us_) {
                        disconnect_client(true);
                        next_reconnect_us_ = now + 2'000'000;
                        last_client_state_ = client::State::Idle;
                        return;
                    }
                }
                disconnect_client();
                toast(reconnecting_ ? std::format("No se pudo reconectar con {}", reconnect_host_name_)
                                    : (msg.empty() ? std::format("Desconectado de {}", name) : msg));
                last_client_state_ = client::State::Idle;
                return;
            }
            default:
                break;
        }
        last_client_state_ = st;
    }
    if (st != client::State::Connected) return;

    if (second_) {
        if (second_->closed_by_user()) {
            close_second_window();
        } else if (auto err = session_->take_second_stream_error()) {
            toast("Segunda ventana: " + *err);
            close_second_window();
        } else if (const auto s2 = session_->stream(1)) {
            second_->set_title(detail::widen(std::format("LanCast · {} · monitor {} ({}x{} @ {} fps)", session_->host_name(),
                                                         s2->monitor_id, s2->width, s2->height, s2->fps)));
        }
    }
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
    session_->set_audio_output(settings_.client.stream.audio_volume, settings_.client.stream.audio_buffer_ms);
}

void App::open_second_window(std::uint32_t monitor_id) {
    if (!session_ || !streaming()) return;
    close_second_window();
    SecondWindow::Hooks hooks;
    hooks.control_enabled = [this] { return control_enabled(); };
    hooks.remote_cursor = [this] { return remote_cursor_; };
    second_ = std::make_unique<SecondWindow>(device_, *session_, std::move(hooks));
    if (!second_->open(detail::widen(std::format("LanCast · {} · monitor {}", session_->host_name(), monitor_id)),
                       MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST))) {
        second_.reset();
        toast("No se pudo abrir la segunda ventana");
        return;
    }
    // Pedir el stream con la resolución del monitor donde quedó la ventana.
    proto::StreamConfig c = make_stream_config();
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(second_->hwnd(), MONITOR_DEFAULTTONEAREST), &mi);
    c.max_width = static_cast<std::uint32_t>(mi.rcMonitor.right - mi.rcMonitor.left);
    c.max_height = static_cast<std::uint32_t>(mi.rcMonitor.bottom - mi.rcMonitor.top);
    c.stream_index = 1;
    c.monitor_id = monitor_id;
    c.audio = false;
    session_->request_stream(c);
    second_monitor_id_ = monitor_id;
    toast(std::format("Monitor {} abierto en otra ventana (doble clic o F11: pantalla completa)", monitor_id));
}

void App::close_second_window() {
    if (!second_) return;
    if (session_ && session_->state() == client::State::Connected) session_->stop_second_stream();
    second_.reset();
    second_monitor_id_ = 0;
}

}  // namespace lancast::app
