#pragma once

// Segunda ventana de stream: muestra otro monitor del host en paralelo con la ventana principal
// (su propia swapchain y su propio decodificador; el teclado sigue yendo por el hook global).

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "lancast/client/client_session.hpp"
#include "lancast/core/geometry.hpp"
#include "lancast/platform/d3d.hpp"
#include "renderer.hpp"

namespace lancast::app {

class SecondWindow {
public:
    struct Hooks {
        std::function<bool()> control_enabled;  // ¿Se puede enviar mouse/teclado?
        std::function<HCURSOR()> remote_cursor;  // Forma del cursor remoto (o nullptr).
    };

    SecondWindow(const gpu::Device& device, client::ClientSession& session, Hooks hooks);
    ~SecondWindow();
    SecondWindow(const SecondWindow&) = delete;
    SecondWindow& operator=(const SecondWindow&) = delete;

    // Abre la ventana; si el equipo tiene otro monitor distinto de `avoid`, la pone ahí a pantalla completa.
    bool open(const std::wstring& title, HMONITOR avoid);
    void set_title(const std::wstring& title);
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] bool closed_by_user() const noexcept { return closed_by_user_; }

    // Decodifica los cuadros pendientes del stream 2 y presenta el último.
    void render();

    LRESULT wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    void resize(UINT width, UINT height);
    void toggle_fullscreen();
    void send_mouse(LPARAM lp);

    const gpu::Device& device_;
    client::ClientSession& session_;
    Hooks hooks_;
    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    bool tearing_ = false;
    Size backbuffer_size_;
    std::unique_ptr<VideoRenderer> renderer_;
    bool fullscreen_ = false;
    WINDOWPLACEMENT saved_placement_{sizeof(WINDOWPLACEMENT)};
    bool closed_by_user_ = false;
    std::uint64_t last_present_us_ = 0;
};

}  // namespace lancast::app
