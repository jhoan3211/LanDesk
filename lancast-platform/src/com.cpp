#include <format>

#include "win.hpp"

namespace lancast::platform {

std::string Error::message() const {
    if (hresult == 0) return context;

    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(hresult), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (len > 0 && buffer != nullptr) {
        text = detail::to_utf8(std::wstring_view{buffer, len});
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    }
    if (buffer != nullptr) LocalFree(buffer);

    const auto code = static_cast<std::uint32_t>(hresult);
    return text.empty() ? std::format("{} (0x{:08X})", context, code)
                        : std::format("{}: {} (0x{:08X})", context, text, code);
}

namespace detail {

ComScope::ComScope() noexcept : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}

ComScope::~ComScope() {
    // RPC_E_CHANGED_MODE significa que otro componente ya inicializó COM: no nos toca liberarlo.
    if (SUCCEEDED(hr_)) CoUninitialize();
}

}  // namespace detail
}  // namespace lancast::platform
