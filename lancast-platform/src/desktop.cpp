#include "lancast/platform/desktop.hpp"

#include "win.hpp"

#include "lancast/core/log.hpp"

namespace lancast::desk {
namespace {

std::wstring desktop_name(HDESK desk) {
    wchar_t name[256]{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), &needed)) return {};
    return name;
}

}  // namespace

std::wstring input_desktop_name() {
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (input == nullptr) return {};
    std::wstring name = desktop_name(input);
    CloseDesktop(input);
    return name;
}

bool attach_to_input_desktop() {
    HDESK input = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (input == nullptr) return false;
    const std::wstring name = desktop_name(input);
    if (HDESK current = GetThreadDesktop(GetCurrentThreadId()); current != nullptr && desktop_name(current) == name) {
        CloseDesktop(input);
        return false;
    }
    if (!SetThreadDesktop(input)) {
        log::debug("SetThreadDesktop({}) falló: {}", platform::detail::to_utf8(name), GetLastError());
        CloseDesktop(input);
        return false;
    }
    // El handle del escritorio anterior abierto por este hilo ya no se usa.
    thread_local HDESK owned = nullptr;
    if (owned != nullptr) CloseDesktop(owned);
    owned = input;
    log::info("Hilo {} cambiado al escritorio {}", GetCurrentThreadId(), platform::detail::to_utf8(name));
    return true;
}

}  // namespace lancast::desk
