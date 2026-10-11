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
#include "lancast/platform/system.hpp"
#include "lancast/platform/updater.hpp"
#include "renderer.hpp"
#include "second_window.hpp"

struct ImFont;

namespace lancast::app {

struct LaunchOptions {
    bool minimized = false;
    bool no_host = false;            // Solo cliente (útil para probar dos instancias en el mismo equipo).
    std::optional<std::string> connect;  // Conectar al iniciar.
    std::optional<std::string> pin;      // PIN para --connect (pruebas automatizadas).
    bool measure_latency = false;        // Medir la latencia real al conectar (pruebas).
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
    void disconnect_client(bool keep_view = false);
    void draw_reconnect_overlay();
    void open_second_window(std::uint32_t monitor_id);
    void close_second_window();
    void draw_second_window_controls();
    void check_updates(bool manual);
    void start_update_download();
    void draw_update_banner();
    [[nodiscard]] bool streaming() const;
    [[nodiscard]] proto::StreamConfig make_stream_config() const;
    void apply_stream_settings();
    void apply_min_latency();
    void update_presentation_mode();
    bool sample_matches(ID3D11Texture2D* backbuffer, const Rect& view, const client::LatencyTarget& target);
    void switch_monitor(std::uint32_t id);
    void process_client();
    void set_relative_mouse(bool on);
    void send_mouse_position();
    void handle_mouse_button(UINT msg, WPARAM wp);
    [[nodiscard]] bool control_enabled() const;
    [[nodiscard]] bool ui_captures_mouse() const;

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
    void draw_stream_toolbar();
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
    std::uint64_t toolbar_until_us_ = 0;  // La barra superior se muestra hasta este instante.
    bool relative_mouse_ = false;
    std::optional<PointF> pending_mouse_;
    std::uint64_t last_mouse_send_us_ = 0;
    std::unique_ptr<sys::RealtimeThread> render_boost_;  // MMCSS del hilo de presentación durante el stream.
    std::string presentation_mode_;                      // "directa" / "compuesta por DWM"
    bool presentation_direct_ = false;
    std::uint64_t last_presentation_check_us_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> latency_staging_;
    HCURSOR remote_cursor_ = nullptr;
    bool remote_cursor_visible_ = true;
    HHOOK keyboard_hook_ = nullptr;
    bool mod_ctrl_[2] = {false, false};  // [izquierdo, derecho]
    bool mod_alt_[2] = {false, false};
    bool mod_shift_[2] = {false, false};
    std::string last_remote_clipboard_;
    std::uint64_t last_present_us_ = 0;
    bool frame_presented_since_stream_ = false;
    client::State last_client_state_ = client::State::Idle;
    std::uint16_t last_stream_epoch_ = 0xFFFF;
    std::string new_profile_name_;

    // Actualización automática (hilo propio; estado protegido por update_mutex_).
    enum class UpdateState { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };
    std::mutex update_mutex_;
    UpdateState update_state_ = UpdateState::Idle;
    std::optional<update::ReleaseInfo> update_info_;
    std::filesystem::path update_file_;
    std::string update_message_;
    std::uint64_t update_done_ = 0;
    std::uint64_t update_total_ = 0;
    bool update_dismissed_ = false;
    std::atomic<bool> update_cancel_{false};
    std::thread update_thread_;

    // Segunda ventana (otro monitor del host en paralelo).
    std::unique_ptr<SecondWindow> second_;
    std::uint32_t second_monitor_id_ = 0;

    // Reconexión automática tras un corte de red.
    bool was_connected_ = false;
    bool reconnecting_ = false;
    std::string reconnect_address_;
    std::string last_connect_address_;
    std::string reconnect_host_name_;
    std::uint64_t reconnect_deadline_us_ = 0;
    std::uint64_t next_reconnect_us_ = 0;
    int reconnect_attempts_ = 0;

    // UI
    Page page_ = Page::Connect;
    ImFont* font_ = nullptr;
    ImFont* font_big_ = nullptr;
    std::string toast_;
    std::uint64_t toast_until_us_ = 0;
    std::optional<SystemReport> report_;
};

}  // namespace lancast::app
