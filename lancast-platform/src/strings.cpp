#include <format>

#include "win.hpp"

namespace lancast::platform::detail {

std::string to_utf8(std::wstring_view wide) {
    // Cortar en el primer NUL: muchos campos de Win32 son arrays de tamaño fijo.
    if (const auto nul = wide.find(L'\0'); nul != std::wstring_view::npos) wide = wide.substr(0, nul);
    if (wide.empty()) return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

std::string guid_to_string(const GUID& g) {
    return std::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}", g.Data1,
                       g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
                       g.Data4[6], g.Data4[7]);
}

}  // namespace lancast::platform::detail
