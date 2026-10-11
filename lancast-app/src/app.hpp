#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "lancast/client/client_session.hpp"
#include "lancast/core/geometry.hpp"
#include "lancast/core/settings.hpp"
#include "lancast/host/host_service.hpp"
#include "lancast/platform/d3d.hpp"
#include "renderer.hpp"

struct ImFont;

namespace lancast::app {

struct LaunchOptions {
    bool minimized = false;
    bool no_host = false;            // Solo cliente (útil para probar dos instancias en el mismo equipo).
    std::optional<std::string> connect;  // Conectar al iniciar.
    std::optional<std::string> pin;      // PIN para --connect (pruebas automatizadas).
};

enum class Page { Connect, Host, Files, Settings, Diagnostics };

class App {
public:
    explicit App(LaunchOptions options);
    ~App();
    int run();

    LRESULT wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    bool keyboard_hook(WPARAM wp, const KBDLLHOOKSTRUCT& k);

private:
    // Ventana y D3D
    bool create_window();
    bool create_swapchain();
    void resize_swapchain(UINT width, UINT height);
    void apply_dpi(UINT dpi);
    void set_style(float scale);
    void toggle_fullscreen();
    void render();
    void present(bool video_frame);

    // Bandeja y aviso de sesión
    void create_tray();
    void remove_tray();
    void tray_menu();
    void notify(const std::wstring& title, const std::wstring& text);
    void update_session_banner();

    // Sesión de cliente
    void start_connect(const std::string& address);
    void disconnect_client();
    [[nodiscard]] bool streaming() const;
    [[nodiscard]] proto::StreamConfig make_stream_config() const;
    void apply_stream_settings();
    void switch_monitor(std::uint32_t id);
    void process_client();
    void set_relative_mouse(bool on);
    void send_mouse_position();
    void handle_mouse_button(UINT msg, WPARAM wp);
    [[nodiscard]] bool control_enabled() const;

    // Interfaz
    void draw_ui();
    void draw_sidebar();
    void draw_connect_page();
    void draw_stream_settings(bool live);
    void draw_host_page();
    void draw_files_page();
    void draw_transfers(files::Manager& manager, const char* id);
    void draw_settings_page();
    void draw_diagnostics_page();
    void draw_stream_overlay();
    void draw_stats_overlay();
    void draw_pin_modal();
    void draw_toast();
    void toast(std::string text);
    void save_settings();

    LaunchOptions options_;
    AppSettings settings_;
    HWND hwnd_ = nullptr;
    HWND banner_ = nullptr;
    UINT dpi_ = 96;
    float scale_ = 1.0f;
    bool running_ = true;
    bool fullscreen_ = false;
    WINDOWPLACEMENT saved_placement_{sizeof(WINDOWPLACEMENT)};
    bool tray_added_ = false;
    bool settings_dirty_ = false;
    std::uint64_t settings_dirty_since_ = 0;

    gpu::Device device_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    bool tearing_supported_ = false;
    Size backbuffer_size_;
    bool resize_pending_ = false;
    UINT pending_w_ = 0;
    UINT pending_h_ = 0;

    // Host
    host::HostService host_;
    bool last_session_active_ = false;
    std::string last_pin_;

    // Cliente
    std::unique_ptr<client::ClientSession> session_;
    std::unique_ptr<VideoRenderer> renderer_;
    std::vector<client::DiscoveredHost> discovered_;
    std::uint64_t last_discovery_us_ = 0;
    std::thread discovery_thread_;
    std::atomic<bool> discovery_running_{false};
    std::mutex discovery_mutex_;
    std::string address_input_;
    std::string pin_input_;
    bool pin_modal_open_ = false;
    bool show_menu_ = false;
    bool show_stats_ = false;
    bool relative_mouse_ = false;
    std::optional<PointF> pending_mouse_;
    HCURSOR remote_cursor_ = nullptr;
    bool remote_cursor_visible_ = true;
    HHOOK keyboard_hook_ = nullptr;
    std::string last_remote_clipboard_;
    std::uint64_t last_present_us_ = 0;
    bool frame_presented_since_stream_ = false;
    client::State last_client_state_ = client::State::Idle;
    std::uint16_t last_stream_epoch_ = 0xFFFF;
    std::string new_profile_name_;

    // UI
    Page page_ = Page::Connect;
    ImFont* font_ = nullptr;
    ImFont* font_big_ = nullptr;
    std::string toast_;
    std::uint64_t toast_until_us_ = 0;
    std::optional<SystemReport> report_;
};

}  // namespace lancast::app
