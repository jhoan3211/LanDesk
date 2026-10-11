#include "second_window.hpp"

#include <windowsx.h>

#include <vector>

#include "lancast/core/log.hpp"
#include "resource.h"

namespace lancast::app {
namespace {

LRESULT CALLBACK second_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    if (auto* self = reinterpret_cast<SecondWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); self != nullptr) {
        return self->wnd_proc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct MonitorList {
    std::vector<HMONITOR> monitors;
};

BOOL CALLBACK collect(HMONITOR m, HDC, LPRECT, LPARAM user) {
    reinterpret_cast<MonitorList*>(user)->monitors.push_back(m);
    return TRUE;
}

}  // namespace

SecondWindow::SecondWindow(const gpu::Device& device, client::ClientSession& session, Hooks hooks)
    : device_(device), session_(session), hooks_(std::move(hooks)) {}

SecondWindow::~SecondWindow() {
    renderer_.reset();
    swapchain_.Reset();
    if (hwnd_ != nullptr) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

bool SecondWindow::open(const std::wstring& title, HMONITOR avoid) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = second_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_LANCAST));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"LanCastSecondStream";
    RegisterClassExW(&wc);

    // ¿Hay otro monitor en este equipo? Entonces la ventana va ahí, a pantalla completa.
    MonitorList list;
    EnumDisplayMonitors(nullptr, nullptr, collect, reinterpret_cast<LPARAM>(&list));
    HMONITOR target = nullptr;
    for (HMONITOR m : list.monitors) {
        if (m != avoid) {
            target = m;
            break;
        }
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(target != nullptr ? target : avoid, &mi);
    const RECT& work = mi.rcWork;
    const int w = (work.right - work.left) * 3 / 4;
    const int h = (work.bottom - work.top) * 3 / 4;
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - w) / 2,
                            work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, inst, this);
    if (hwnd_ == nullptr) return false;

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_.adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allow = FALSE;
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow));
        tearing_ = allow != FALSE;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.device.Get(), hwnd_, &desc, nullptr, nullptr, &swapchain_))) return false;
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    renderer_ = std::make_unique<VideoRenderer>(device_);

    ShowWindow(hwnd_, SW_SHOW);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    resize(static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom));
    if (target != nullptr) toggle_fullscreen();
    return true;
}

void SecondWindow::set_title(const std::wstring& title) {
    if (hwnd_ != nullptr) SetWindowTextW(hwnd_, title.c_str());
}

void SecondWindow::resize(UINT width, UINT height) {
    if (!swapchain_ || width == 0 || height == 0) return;
    if (renderer_) renderer_->release_target();
    device_.context->ClearState();
    device_.context->Flush();
    if (FAILED(swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0))) {
        log::warn("Segunda ventana: ResizeBuffers falló");
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bb;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb));
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    backbuffer_size_ = Size{d.Width, d.Height};
}

void SecondWindow::toggle_fullscreen() {
    const LONG style = GetWindowLongW(hwnd_, GWL_STYLE);
    if (!fullscreen_) {
        GetWindowPlacement(hwnd_, &saved_placement_);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd_, &saved_placement_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen_ = !fullscreen_;
}

void SecondWindow::render() {
    if (!swapchain_ || !renderer_ || IsIconic(hwnd_)) return;
    const auto stream = session_.stream(1);
    bool new_video = false;
    if (stream) {
        for (auto& f : session_.take_frames(1)) {
            double ms = 0;
            new_video = renderer_->decode(f, *stream, ms) || new_video;
        }
        if (renderer_->needs_keyframe()) session_.request_keyframe(1);
    }
    // Sin video nuevo, redibujar como mucho 10 veces por segundo (cambios de tamaño, etc.).
    if (!new_video && gpu::now_us() - last_present_us_ < 100'000) return;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> bb;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&bb));
    bool drew = false;
    if (stream && renderer_->has_picture()) {
        drew = renderer_->draw(bb.Get(), backbuffer_size_, view_rect({stream->width, stream->height}, backbuffer_size_, ScalingMode::Fit));
    }
    if (!drew) {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(device_.device->CreateRenderTargetView(bb.Get(), nullptr, &rtv))) {
            const float black[4] = {0, 0, 0, 1};
            device_.context->ClearRenderTargetView(rtv.Get(), black);
        }
    }
    swapchain_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    last_present_us_ = gpu::now_us();
}

void SecondWindow::send_mouse(LPARAM lp) {
    const auto stream = session_.stream(1);
    if (!stream || !hooks_.control_enabled()) return;
    const Rect view = view_rect({stream->width, stream->height}, backbuffer_size_, ScalingMode::Fit);
    if (const auto n = window_to_normalized({static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))}, view)) {
        session_.send_message(proto::encode(proto::MsgType::MouseAbs, proto::MouseAbs{n->x, n->y, 1}));
    }
}

LRESULT SecondWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) resize(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_CLOSE:
            closed_by_user_ = true;
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_LBUTTONDBLCLK:
            toggle_fullscreen();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_F11) toggle_fullscreen();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && hooks_.control_enabled()) {
                const HCURSOR c = session_.cursor(1).visible ? hooks_.remote_cursor() : nullptr;
                SetCursor(c != nullptr ? c : (session_.cursor(1).visible ? LoadCursorW(nullptr, IDC_ARROW) : nullptr));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            send_mouse(lp);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP: {
            if (!hooks_.control_enabled()) return 0;
            send_mouse(lp);
            proto::MouseButton b;
            b.button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)   ? proto::MouseButtonId::Left
                       : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? proto::MouseButtonId::Right
                                                                         : proto::MouseButtonId::Middle;
            b.down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
            if (b.down) {
                SetCapture(hwnd);
            } else {
                ReleaseCapture();
            }
            session_.send_message(proto::encode(proto::MsgType::MouseButton, b));
            return 0;
        }
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (hooks_.control_enabled()) {
                session_.send_message(proto::encode(proto::MsgType::MouseWheel,
                                                    proto::MouseWheel{GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL}));
            }
            return 0;
        case WM_KILLFOCUS:
            session_.send_message(proto::encode(proto::MsgType::ReleaseAll));
            break;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace lancast::app
