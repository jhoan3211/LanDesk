#pragma once

// Inyección de entrada en el host (SendInput) y utilidades del cliente.

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <optional>

#include "lancast/core/geometry.hpp"
#include "lancast/core/protocol.hpp"

struct HWND__;
struct HICON__;

namespace lancast::input {

[[nodiscard]] Rect virtual_screen() noexcept;

// Recuerda teclas/botones presionados para soltarlos todos al desconectar (evita teclas "pegadas").
class Injector {
public:
    void mouse_abs(PointF desktop);
    void mouse_rel(std::int32_t dx, std::int32_t dy);
    void mouse_button(proto::MouseButtonId button, bool down);
    void mouse_wheel(std::int32_t delta, bool horizontal);
    void key(std::uint16_t scancode, bool extended, bool down);
    void release_all();

private:
    std::mutex mutex_;
    std::set<std::uint32_t> keys_down_;  // scancode | extended << 16
    std::set<proto::MouseButtonId> buttons_down_;
};

// Portapapeles de texto (UTF-8).
[[nodiscard]] std::optional<std::string> get_clipboard_text(HWND__* owner);
bool set_clipboard_text(HWND__* owner, std::string_view text);

// Convierte la forma de cursor de Desktop Duplication en un HCURSOR (lo destruye quien lo pide con DestroyCursor).
[[nodiscard]] HICON__* create_cursor(const proto::CursorShape& shape);

}  // namespace lancast::input
