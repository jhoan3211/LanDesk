#pragma once

// Utilidades internas de Windows (no exportadas).

#include <windows.h>
#include <wrl/client.h>

#include <expected>
#include <string>
#include <string_view>

#include "lancast/platform/error.hpp"

namespace lancast::platform::detail {

using Microsoft::WRL::ComPtr;

[[nodiscard]] std::string to_utf8(std::wstring_view wide);
[[nodiscard]] std::wstring to_wide(std::string_view utf8);
[[nodiscard]] std::string guid_to_string(const GUID& guid);

[[nodiscard]] inline std::unexpected<Error> hr_error(std::string context, HRESULT hr) {
    return std::unexpected(Error{std::move(context), static_cast<std::int32_t>(hr)});
}

[[nodiscard]] inline std::uint64_t pack_luid(const LUID& luid) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
}

// Inicializa COM (MTA) en el hilo actual mientras viva el objeto.
class ComScope {
public:
    ComScope() noexcept;
    ~ComScope();
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    [[nodiscard]] HRESULT result() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

}  // namespace lancast::platform::detail
