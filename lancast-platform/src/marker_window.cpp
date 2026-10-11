#include "lancast/platform/marker_window.hpp"

#include "win.hpp"

#include <dwmapi.h>

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "lancast/platform/d3d.hpp"

#pragma comment(lib, "dwmapi.lib")

namespace lancast::sys {
namespace {

constexpr UINT kMsgFlash = WM_APP + 10;

LRESULT CALLBACK marker_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        const bool white = GetPropW(hwnd, L"white") != nullptr;
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(white ? WHITE_BRUSH : BLACK_BRUSH)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;  // No intercepta el mouse.
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

struct MarkerWindow::Impl {
    std::thread thread;
    HWND hwnd = nullptr;
    DWORD thread_id = 0;
    std::promise<void> ready;
};

MarkerWindow::MarkerWindow(const Rect& rect) : impl_(std::make_unique<Impl>()) {
    auto ready = impl_->ready.get_future();
    impl_->thread = std::thread([this, rect] {
        impl_->thread_id = GetCurrentThreadId();
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = marker_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"LanCastLatencyMarker";
        RegisterClassExW(&wc);
        impl_->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                      wc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, rect.x, rect.y, rect.width, rect.height,
                                      nullptr, nullptr, wc.hInstance, nullptr);
        impl_->ready.set_value();
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            if (m.message == kMsgFlash) {
                // Propiedad compartida: si quien pidió el destello dejó de esperar, la promesa sigue viva.
                std::unique_ptr<std::shared_ptr<std::promise<std::uint64_t>>> result{
                    reinterpret_cast<std::shared_ptr<std::promise<std::uint64_t>>*>(m.lParam)};
                if (m.wParam != 0) {
                    SetPropW(impl_->hwnd, L"white", reinterpret_cast<HANDLE>(1));
                } else {
                    RemovePropW(impl_->hwnd, L"white");
                }
                SetWindowPos(impl_->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                InvalidateRect(impl_->hwnd, nullptr, FALSE);
                UpdateWindow(impl_->hwnd);
                DwmFlush();  // Espera a la composición: a partir de aquí el cambio está en pantalla.
                (*result)->set_value(gpu::now_us());
                continue;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (impl_->hwnd != nullptr) DestroyWindow(impl_->hwnd);
    });
    ready.wait();
}

MarkerWindow::~MarkerWindow() {
    if (impl_->thread_id != 0) PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    if (impl_->thread.joinable()) impl_->thread.join();
}

std::uint64_t MarkerWindow::flash(bool white) {
    if (impl_->hwnd == nullptr) return 0;
    auto result = std::make_shared<std::promise<std::uint64_t>>();
    auto future = result->get_future();
    auto* message = new std::shared_ptr<std::promise<std::uint64_t>>(result);
    if (!PostThreadMessageW(impl_->thread_id, kMsgFlash, white ? 1 : 0, reinterpret_cast<LPARAM>(message))) {
        delete message;
        return 0;
    }
    if (future.wait_for(std::chrono::seconds(1)) != std::future_status::ready) return 0;
    return future.get();
}

}  // namespace lancast::sys
