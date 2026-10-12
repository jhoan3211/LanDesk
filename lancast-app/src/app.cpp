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
#include "lancast/platform/crash.hpp"
#include "lancast/platform/input.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/platform.hpp"
#include "lancast/platform/system.hpp"
#include "resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace lancast::app {
using namespace detail;

namespace detail {

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
            if (LOWORD(wp) == kBannerButton && g_app != nullptr) PostMessageW(GetWindow(hwnd, GW_OWNER), kMsgBannerDisconnect, 0, 0);
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

}  // namespace detail

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
    update_cancel_ = true;
    if (update_thread_.joinable()) update_thread_.join();
    if (service_thread_.joinable()) service_thread_.join();
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
    // ResizeBuffers exige que no quede ninguna referencia al back buffer: la vista de salida del
    // procesador de video también cuenta.
    rtv_.Reset();
    if (renderer_) renderer_->release_target();
    device_.context->OMSetRenderTargets(0, nullptr, nullptr);
    device_.context->ClearState();
    device_.context->Flush();
    ui_rtv_.Reset();
    ui_srv_.Reset();
    ui_tex_.Reset();
    const HRESULT hr = swapchain_->ResizeBuffers(0, width, height, swap_format_,
                                                 tearing_supported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        log::error("ResizeBuffers falló (0x{:08X}) al cambiar a {}x{}", static_cast<std::uint32_t>(hr), width, height);
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bb;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb));
    device_.device->CreateRenderTargetView(bb.Get(), nullptr, &rtv_);
    // Tamaño real del back buffer (si ResizeBuffers falló, sigue siendo el anterior).
    D3D11_TEXTURE2D_DESC desc{};
    bb->GetDesc(&desc);
    backbuffer_size_ = Size{desc.Width, desc.Height};
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
    refresh_service_status();
    host_.start(effective_host_settings());
    report_ = platform::collect_system_report();

    ShowWindow(hwnd_, options_.minimized ? SW_HIDE : SW_SHOW);
    if (!options_.minimized) UpdateWindow(hwnd_);
    if (options_.connect) start_connect(*options_.connect);
    std::thread([] { update::cleanup_previous(); }).detach();  // Borra el .old de una actualización anterior.
    if (settings_.check_updates) check_updates(false);
    if (const auto dump = crash::unseen(sys::app_data_dir() / L"crashes")) {
        toast("LanCast se cerró inesperadamente la última vez. Se guardó un informe: Diagnóstico → Cierres inesperados");
        crash::mark_seen(sys::app_data_dir() / L"crashes");
        log::warn("Volcado de un cierre anterior: {}", detail::narrow(dump->wstring()));
    }

    std::uint64_t last_host_poll = 0;
    while (running_) {
        HANDLE events[2]{};
        DWORD event_count = 0;
        if (session_) {
            events[event_count++] = static_cast<HANDLE>(session_->frame_event(0));
            if (second_) events[event_count++] = static_cast<HANDLE>(session_->frame_event(1));
        }
        const bool visible = IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
        DWORD timeout = 16;
        if (!visible && !second_) timeout = 200;
        MsgWaitForMultipleObjectsEx(event_count, event_count ? events : nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) running_ = false;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!running_) break;

        send_mouse_position();
        process_client();

        // Actualización descargada y verificada: reemplazar el .exe y reiniciar.
        {
            std::filesystem::path ready;
            {
                std::scoped_lock lock(update_mutex_);
                if (update_state_ == UpdateState::Ready) {
                    ready = update_file_;
                    update_state_ = UpdateState::Idle;
                }
            }
            if (!ready.empty()) {
                if (auto r = update::install_and_restart(ready)) {
                    running_ = false;
                    break;
                } else {
                    std::scoped_lock lock(update_mutex_);
                    update_state_ = UpdateState::Failed;
                    update_message_ = r.error().message();
                }
            }
        }

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
            host_.apply_settings(effective_host_settings());
            settings_dirty_ = false;
        }
        poll_service_action();
        if (now - display_checked_us_ > 1'000'000) {
            display_checked_us_ = now;
            follow_display();
        }
        if (!service_busy_ && now - service_checked_us_ > 5'000'000) {
            service_checked_us_ = now;
            refresh_service_status();
        }
        if (visible) render();
        if (second_) second_->render();
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
                           now < toolbar_until_us_ || now < toolbar_tab_until_us_ || !control_enabled();
    if (streaming() && !new_video && !ui_active && now - last_present_us_ < 100'000 && frame_presented_since_stream_) return;
    if (streaming() && !new_video && now - last_present_us_ < 8'000) return;  // Máx. ~120 Hz de interfaz sin video.

    update_hdr_output(false);

    const std::uint64_t t0 = gpu::now_us();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> bb;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb));
    bool drew_video = false;
    bool latency_seen = false;
    if (streaming() && renderer_ && renderer_->has_picture()) {
        const auto stream = session_->stream();
        const Rect view = view_rect({stream->width, stream->height}, backbuffer_size_, settings_.client.stream.scaling);
        drew_video = renderer_->draw(bb.Get(), backbuffer_size_, view,
                                     VideoRenderer::Output{rtv_.Get(), hdr_output_, display_color_.sdr_white_nits,
                                                           display_color_.peak_nits});
        // Medición de latencia: ¿ya se ve en la imagen el color que pintó el host? (no en salida HDR: FP16)
        if (drew_video && !hdr_output_) {
            if (const auto target = session_->latency_target()) {
                latency_seen = sample_matches(bb.Get(), view, *target);
            }
        }
    }
    if (!drew_video) {
        const float bg[4] = {0.075f, 0.082f, 0.098f, 1.0f};
        const float black[4] = {0, 0, 0, 1};
        device_.context->ClearRenderTargetView(rtv_.Get(), streaming() ? black : bg);
    }
    if (hdr_output_ && ensure_ui_target()) {
        // La interfaz se dibuja aparte (8 bits, con gamma) y se compone sobre el video HDR al brillo SDR de Windows.
        const float clear[4] = {0, 0, 0, 0};
        device_.context->ClearRenderTargetView(ui_rtv_.Get(), clear);
        ID3D11RenderTargetView* ui = ui_rtv_.Get();
        device_.context->OMSetRenderTargets(1, &ui, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ui_compositor_->draw(ui_srv_.Get(), rtv_.Get(), backbuffer_size_, display_color_.sdr_white_nits);
    } else {
        ID3D11RenderTargetView* rtv = rtv_.Get();
        device_.context->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    const bool vsync = !streaming() || settings_.client.stream.vsync;
    UINT flags = 0;
    if (!vsync && tearing_supported_) flags |= DXGI_PRESENT_ALLOW_TEARING;
    swapchain_->Present(vsync ? 1 : 0, flags);
    last_present_us_ = gpu::now_us();

    if (latency_seen) session_->latency_observed(last_present_us_);
    if (streaming() && last_present_us_ - last_presentation_check_us_ > 1'000'000) update_presentation_mode();
    if (new_video) {
        if (!frame_presented_since_stream_ && options_.measure_latency) session_->start_latency_test();
        frame_presented_since_stream_ = true;
        session_->report_presented(last_frame.frame.info, last_frame.arrival_us, decode_ms, (last_present_us_ - t0) / 1000.0);
    }
}

bool App::sample_matches(ID3D11Texture2D* backbuffer, const Rect& view, const client::LatencyTarget& target) {
    // Copia 1 píxel del back buffer a una textura de lectura (solo durante la medición: obliga a esperar a la GPU).
    if (!latency_staging_) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = 1;
        d.Height = 1;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_.device->CreateTexture2D(&d, nullptr, &latency_staging_))) return false;
    }
    const UINT px = static_cast<UINT>(std::clamp<float>(view.x + target.x * view.width, 0.f, static_cast<float>(backbuffer_size_.width - 1)));
    const UINT py = static_cast<UINT>(std::clamp<float>(view.y + target.y * view.height, 0.f, static_cast<float>(backbuffer_size_.height - 1)));
    const D3D11_BOX box{px, py, 0, px + 1, py + 1, 1};
    device_.context->CopySubresourceRegion(latency_staging_.Get(), 0, 0, 0, 0, backbuffer, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(device_.context->Map(latency_staging_.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const auto* bgra = static_cast<const std::uint8_t*>(m.pData);
    const int luma = (bgra[0] * 29 + bgra[1] * 150 + bgra[2] * 77) / 256;
    device_.context->Unmap(latency_staging_.Get(), 0);
    return target.white ? luma > 190 : luma < 60;
}

void App::update_hdr_output(bool force) {
    const std::uint64_t now = gpu::now_us();
    const auto stream = streaming() ? session_->stream() : std::nullopt;
    const std::uint16_t epoch = stream ? stream->epoch : 0xFFFF;
    if (!force && epoch == hdr_checked_epoch_ && now - hdr_checked_us_ < 1'000'000) return;
    hdr_checked_us_ = now;
    hdr_checked_epoch_ = epoch;
    display_color_ = gpu::display_color(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
    const bool want = stream && stream->hdr && display_color_.hdr;
    if (want == hdr_output_) return;

    hdr_output_ = want;
    swap_format_ = want ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    resize_swapchain(backbuffer_size_.width, backbuffer_size_.height);
    Microsoft::WRL::ComPtr<IDXGISwapChain3> sc3;
    if (SUCCEEDED(swapchain_.As(&sc3))) {
        sc3->SetColorSpace1(want ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    }
    log::info("Salida {}: blanco SDR {:.0f} nits, pico {:.0f} nits", want ? "HDR (scRGB)" : "SDR", display_color_.sdr_white_nits,
              display_color_.peak_nits);
    if (want) toast("Salida HDR activada");
}

bool App::ensure_ui_target() {
    if (!ui_compositor_) {
        auto c = gpu::UiCompositor::create(device_);
        if (!c) {
            log::error("Composición de la interfaz HDR: {}", c.error().message());
            return false;
        }
        ui_compositor_ = std::move(*c);
    }
    if (ui_tex_) return true;
    auto tex = gpu::create_texture(device_.device.Get(), backbuffer_size_.width, backbuffer_size_.height,
                                   DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!tex) return false;
    ui_tex_ = *tex;
    return SUCCEEDED(device_.device->CreateRenderTargetView(ui_tex_.Get(), nullptr, &ui_rtv_)) &&
           SUCCEEDED(device_.device->CreateShaderResourceView(ui_tex_.Get(), nullptr, &ui_srv_));
}

void App::update_presentation_mode() {
    last_presentation_check_us_ = last_present_us_;
    Microsoft::WRL::ComPtr<IDXGISwapChainMedia> media;
    DXGI_FRAME_STATISTICS_MEDIA st{};
    if (FAILED(swapchain_.As(&media)) || FAILED(media->GetFrameStatisticsMedia(&st))) return;
    // NONE = "independent flip": el frame va directo a la pantalla sin pasar por la composición de DWM.
    presentation_direct_ = st.CompositionMode != DXGI_FRAME_PRESENTATION_MODE_COMPOSED;
    presentation_mode_ = presentation_direct_ ? "directa (sin DWM)" : "compuesta por DWM (+1 frame)";
}

void App::apply_min_latency() {
    StreamSettings& s = settings_.client.stream;
    s.vsync = false;
    s.low_latency = true;
    s.audio_buffer_ms = std::min<std::uint32_t>(s.audio_buffer_ms, 10);
    // FPS = frecuencia del monitor del host: cada frame que Windows dibuja se envía sin esperar turno.
    if (session_) {
        if (const auto info = session_->host_info()) {
            std::uint32_t hz = 0;
            for (const auto& m : info->monitors) {
                if (s.monitor_id == 0 || m.id == s.monitor_id) hz = std::max(hz, m.refresh_hz);
            }
            if (hz >= 30) {
                s.fps = hz;
                s.fps_match_display = false;
            }
        }
    }
    settings_.client.active_profile.clear();
    apply_stream_settings();
    if (!fullscreen_ && streaming()) toggle_fullscreen();  // Pantalla completa = presentación directa.
    toast(std::format("Latencia mínima: {} fps, V-Sync apagado, codificador rápido, pantalla completa", s.fps));
}

void App::save_settings() {
    settings_dirty_ = true;
    settings_dirty_since_ = gpu::now_us();
}

void App::toast(std::string text) {
    toast_ = std::move(text);
    toast_until_us_ = gpu::now_us() + 4'000'000;
}

void App::check_updates(bool manual) {
    {
        std::scoped_lock lock(update_mutex_);
        if (update_state_ == UpdateState::Checking || update_state_ == UpdateState::Downloading) return;
        update_state_ = UpdateState::Checking;
        update_message_.clear();
        if (manual) update_dismissed_ = false;
    }
    if (update_thread_.joinable()) update_thread_.join();
    update_thread_ = std::thread([this, manual] {
        if (!manual) Sleep(3000);  // No competir con el arranque.
        const auto current = parse_version(kAppVersion).value_or(Version{});
        auto result = update::check(current);
        std::scoped_lock lock(update_mutex_);
        if (!result) {
            update_state_ = UpdateState::Failed;
            update_message_ = result.error().message();
            log::info("Búsqueda de actualizaciones: {}", update_message_);
        } else if (!*result) {
            update_state_ = UpdateState::UpToDate;
        } else {
            update_info_ = **result;
            update_state_ = UpdateState::Available;
            log::info("Hay una versión nueva: {}", update_info_->tag);
        }
    });
}

void App::start_update_download() {
    std::optional<update::ReleaseInfo> info;
    {
        std::scoped_lock lock(update_mutex_);
        if (update_state_ != UpdateState::Available || !update_info_) return;
        update_state_ = UpdateState::Downloading;
        update_done_ = 0;
        update_total_ = update_info_->size;
        info = update_info_;
    }
    if (update_thread_.joinable()) update_thread_.join();
    update_cancel_ = false;
    update_thread_ = std::thread([this, release = *info] {
        auto file = update::download(
            release,
            [this](std::uint64_t done, std::uint64_t total) {
                std::scoped_lock lock(update_mutex_);
                update_done_ = done;
                if (total != 0) update_total_ = total;
            },
            &update_cancel_);
        std::scoped_lock lock(update_mutex_);
        if (!file) {
            update_state_ = UpdateState::Failed;
            update_message_ = file.error().message();
            return;
        }
        update_file_ = *file;
        update_state_ = UpdateState::Ready;
    });
}

}  // namespace lancast::app
