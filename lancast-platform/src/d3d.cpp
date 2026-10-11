#include "lancast/platform/d3d.hpp"

#include <array>

#include "win.hpp"

namespace lancast::gpu {

platform::Result<Device> create_device(std::optional<std::uint64_t> adapter_luid) {
    ComPtr<IDXGIFactory1> factory;
    if (const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); FAILED(hr)) {
        return platform::detail::hr_error("CreateDXGIFactory1", hr);
    }
    ComPtr<IDXGIFactory6> factory6;
    (void)factory.As(&factory6);

    Device dev;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory6 ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                           IID_PPV_ARGS(&adapter))
                                    : factory->EnumAdapters1(i, &adapter);
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        const std::uint64_t luid = platform::detail::pack_luid(desc.AdapterLuid);
        if (adapter_luid && *adapter_luid != luid) continue;
        dev.adapter = adapter;
        dev.luid = luid;
        dev.vendor = vendor_from_pci_id(desc.VendorId);
        break;
    }
    if (!dev.adapter) return std::unexpected(platform::Error{"No se encontró una GPU compatible", 0});

    constexpr std::array kLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(dev.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, kLevels.data(),
                                   static_cast<UINT>(kLevels.size()), D3D11_SDK_VERSION, &dev.device, nullptr, &dev.context);
    if (FAILED(hr)) return platform::detail::hr_error("D3D11CreateDevice", hr);

    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(dev.device.As(&mt))) mt->SetMultithreadProtected(TRUE);
    return dev;
}

platform::Result<ComPtr<ID3D11Texture2D>> create_texture(ID3D11Device* device, std::uint32_t width, std::uint32_t height,
                                                         DXGI_FORMAT format, UINT bind_flags, UINT misc_flags) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;
    desc.MiscFlags = misc_flags;
    ComPtr<ID3D11Texture2D> tex;
    if (const HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex); FAILED(hr)) {
        return platform::detail::hr_error("CreateTexture2D", hr);
    }
    return tex;
}

std::uint64_t now_us() noexcept {
    static const std::int64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<std::uint64_t>(c.QuadPart / freq * 1'000'000 + (c.QuadPart % freq) * 1'000'000 / freq);
}

}  // namespace lancast::gpu
