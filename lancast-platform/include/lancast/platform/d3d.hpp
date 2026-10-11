#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>

#include "lancast/core/types.hpp"
#include "lancast/platform/error.hpp"

namespace lancast::gpu {

using Microsoft::WRL::ComPtr;

struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIAdapter1> adapter;
    std::uint64_t luid = 0;
    GpuVendor vendor = GpuVendor::Unknown;
};

// Crea un dispositivo D3D11 con soporte de video y protección multihilo (lo comparten captura,
// codificador/decodificador de Media Foundation y la presentación). Sin LUID usa la GPU de más rendimiento.
[[nodiscard]] platform::Result<Device> create_device(std::optional<std::uint64_t> adapter_luid);

[[nodiscard]] platform::Result<ComPtr<ID3D11Texture2D>> create_texture(ID3D11Device* device, std::uint32_t width,
                                                                       std::uint32_t height, DXGI_FORMAT format,
                                                                       UINT bind_flags, UINT misc_flags = 0);

// Microsegundos del reloj monotónico (QueryPerformanceCounter).
[[nodiscard]] std::uint64_t now_us() noexcept;

}  // namespace lancast::gpu
