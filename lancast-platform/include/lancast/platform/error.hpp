#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace lancast::platform {

struct Error {
    std::string context;
    std::int32_t hresult = 0;  // HRESULT o código Win32 convertido con HRESULT_FROM_WIN32.

    [[nodiscard]] std::string message() const;
};

template <typename T>
using Result = std::expected<T, Error>;

}  // namespace lancast::platform
