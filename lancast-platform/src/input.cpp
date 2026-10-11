#include "lancast/platform/input.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "win.hpp"

namespace lancast::input {
namespace {

void send(INPUT& in) { SendInput(1, &in, sizeof(INPUT)); }

DWORD button_flag(proto::MouseButtonId b, bool down, DWORD& data) {
    data = 0;
    switch (b) {
        case proto::MouseButtonId::Left:
            return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        case proto::MouseButtonId::Right:
            return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        case proto::MouseButtonId::Middle:
            return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        case proto::MouseButtonId::X1:
            data = XBUTTON1;
            return down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        case proto::MouseButtonId::X2:
            data = XBUTTON2;
            return down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    }
    return 0;
}

}  // namespace

Rect virtual_screen() noexcept {
    return Rect{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

void Injector::mouse_abs(PointF desktop) {
    const AbsolutePoint a = desktop_to_absolute(desktop, virtual_screen());
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = a.x;
    in.mi.dy = a.y;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    send(in);
}

void Injector::mouse_rel(std::int32_t dx, std::int32_t dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    send(in);
}

void Injector::mouse_button(proto::MouseButtonId button, bool down) {
    {
        std::scoped_lock lock(mutex_);
        if (down) {
            buttons_down_.insert(button);
        } else {
            buttons_down_.erase(button);
        }
    }
    INPUT in{};
    in.type = INPUT_MOUSE;
    DWORD data = 0;
    in.mi.dwFlags = button_flag(button, down, data);
    in.mi.mouseData = data;
    send(in);
}

void Injector::mouse_wheel(std::int32_t delta, bool horizontal) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(delta);
    send(in);
}

void Injector::key(std::uint16_t scancode, bool extended, bool down) {
    {
        std::scoped_lock lock(mutex_);
        const std::uint32_t id = scancode | (extended ? 0x10000u : 0u);
        if (down) {
            keys_down_.insert(id);
        } else {
            keys_down_.erase(id);
        }
    }
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scancode;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
    send(in);
}

void Injector::release_all() {
    std::set<std::uint32_t> keys;
    std::set<proto::MouseButtonId> buttons;
    {
        std::scoped_lock lock(mutex_);
        keys.swap(keys_down_);
        buttons.swap(buttons_down_);
    }
    for (const std::uint32_t k : keys) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = static_cast<WORD>(k & 0xFFFF);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | ((k & 0x10000) ? KEYEVENTF_EXTENDEDKEY : 0);
        send(in);
    }
    for (const auto b : buttons) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        DWORD data = 0;
        in.mi.dwFlags = button_flag(b, false, data);
        in.mi.mouseData = data;
        send(in);
    }
}

std::optional<std::string> get_clipboard_text(HWND__* owner) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return std::nullopt;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(owner)) break;
        if (attempt == 4) return std::nullopt;
        Sleep(10);
    }
    std::optional<std::string> result;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT); h != nullptr) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(h)); text != nullptr) {
            result = platform::detail::to_utf8(text);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return result;
}

bool set_clipboard_text(HWND__* owner, std::string_view text) {
    const std::wstring wide = platform::detail::to_wide(text);
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(owner)) break;
        if (attempt == 4) return false;
        Sleep(10);
    }
    EmptyClipboard();
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem != nullptr) {
        if (void* dst = GlobalLock(mem); dst != nullptr) {
            std::memcpy(dst, wide.c_str(), bytes);
            GlobalUnlock(mem);
            ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

HICON__* create_cursor(const proto::CursorShape& shape) {
    const int w = static_cast<int>(shape.width);
    if (w <= 0 || shape.height == 0) return nullptr;

    if (shape.type == proto::CursorType::Monochrome) {
        // AND arriba, XOR abajo, 1 bpp. CreateCursor espera filas alineadas a WORD.
        const int h = static_cast<int>(shape.height / 2);
        const int row = ((w + 15) / 16) * 2;
        std::vector<std::uint8_t> and_bits(static_cast<std::size_t>(row * h), 0xFF);
        std::vector<std::uint8_t> xor_bits(static_cast<std::size_t>(row * h), 0x00);
        const int copy = std::min(row, static_cast<int>(shape.pitch));
        for (int y = 0; y < h; ++y) {
            std::memcpy(&and_bits[static_cast<std::size_t>(y * row)], &shape.data[static_cast<std::size_t>(y) * shape.pitch],
                        static_cast<std::size_t>(copy));
            std::memcpy(&xor_bits[static_cast<std::size_t>(y * row)],
                        &shape.data[static_cast<std::size_t>(y + h) * shape.pitch], static_cast<std::size_t>(copy));
        }
        return CreateCursor(GetModuleHandleW(nullptr), shape.hot_x, shape.hot_y, w, h, and_bits.data(), xor_bits.data());
    }

    // Color / color enmascarado: DIB de 32 bits con alfa.
    const int h = static_cast<int>(shape.height);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (color == nullptr || bits == nullptr) return nullptr;

    auto* dst = static_cast<std::uint32_t*>(bits);
    for (int y = 0; y < h; ++y) {
        const auto* src = reinterpret_cast<const std::uint32_t*>(&shape.data[static_cast<std::size_t>(y) * shape.pitch]);
        for (int x = 0; x < w; ++x) {
            std::uint32_t px = src[x];
            if (shape.type == proto::CursorType::MaskedColor) {
                // Alfa 0xFF = XOR con la pantalla (aproximación: negro XOR = transparente, resto opaco).
                const bool xor_pixel = (px >> 24) == 0xFF;
                px = xor_pixel ? ((px & 0x00FFFFFF) == 0 ? 0u : (px | 0xFF000000u)) : (px | 0xFF000000u);
            }
            dst[y * w + x] = px;
        }
    }
    HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = static_cast<DWORD>(shape.hot_x);
    info.yHotspot = static_cast<DWORD>(shape.hot_y);
    info.hbmMask = mask;
    info.hbmColor = color;
    HICON cursor = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return cursor;
}

}  // namespace lancast::input
