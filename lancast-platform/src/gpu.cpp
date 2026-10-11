#include <dxgi1_6.h>

#include "lancast/platform/platform.hpp"
#include "win.hpp"

namespace lancast::platform {

using detail::ComPtr;

Result<std::vector<GpuInfo>> enumerate_gpus() {
    ComPtr<IDXGIFactory1> factory;
    if (const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); FAILED(hr)) {
        return detail::hr_error("CreateDXGIFactory1", hr);
    }

    // IDXGIFactory6 permite ordenar por preferencia de rendimiento (dGPU antes que iGPU en portátiles).
    ComPtr<IDXGIFactory6> factory6;
    (void)factory.As(&factory6);

    std::vector<GpuInfo> gpus;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory6
                               ? factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                      IID_PPV_ARGS(&adapter))
                               : factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return detail::hr_error("EnumAdapters", hr);

        DXGI_ADAPTER_DESC1 desc{};
        if (const HRESULT dhr = adapter->GetDesc1(&desc); FAILED(dhr)) {
            return detail::hr_error("IDXGIAdapter1::GetDesc1", dhr);
        }

        GpuInfo gpu;
        gpu.index = static_cast<std::uint32_t>(gpus.size());
        gpu.name = detail::to_utf8(desc.Description);
        gpu.vendor_id = desc.VendorId;
        gpu.device_id = desc.DeviceId;
        gpu.vendor = vendor_from_pci_id(desc.VendorId);
        gpu.luid = detail::pack_luid(desc.AdapterLuid);
        gpu.dedicated_video_memory = desc.DedicatedVideoMemory;
        gpu.shared_system_memory = desc.SharedSystemMemory;
        gpu.is_software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        gpus.push_back(std::move(gpu));
    }
    return gpus;
}

}  // namespace lancast::platform
