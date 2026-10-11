#include <dxgi1_6.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <unordered_map>

#include "lancast/platform/platform.hpp"
#include "win.hpp"

namespace lancast::platform {
namespace {

using detail::ComPtr;

// Datos que solo da la API DisplayConfig (nombre comercial, Hz exactos, modo nativo, HDR).
struct DisplayConfigEntry {
    std::string friendly_name;
    Rational refresh_rate;
    std::uint32_t native_width = 0;
    std::uint32_t native_height = 0;
    std::uint32_t rotation_degrees = 0;
    std::uint32_t bits_per_color = 0;
    bool hdr_supported = false;
    bool hdr_enabled = false;
};

std::uint32_t rotation_to_degrees(DISPLAYCONFIG_ROTATION rotation) noexcept {
    switch (rotation) {
        case DISPLAYCONFIG_ROTATION_ROTATE90:
            return 90;
        case DISPLAYCONFIG_ROTATION_ROTATE180:
            return 180;
        case DISPLAYCONFIG_ROTATION_ROTATE270:
            return 270;
        default:
            return 0;
    }
}

// Clave: nombre GDI del origen ("\\.\DISPLAY1").
std::unordered_map<std::wstring, DisplayConfigEntry> query_display_config() {
    std::unordered_map<std::wstring, DisplayConfigEntry> result;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;

    LONG rc = ERROR_SUCCESS;
    do {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (rc != ERROR_SUCCESS) return result;
        paths.resize(path_count);
        modes.resize(mode_count);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
                                nullptr);
        paths.resize(path_count);
    } while (rc == ERROR_INSUFFICIENT_BUFFER);
    if (rc != ERROR_SUCCESS) return result;

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;

        const std::wstring gdi_name{source.viewGdiDeviceName};
        if (result.contains(gdi_name)) continue;  // Monitores clonados: nos quedamos con el primero.

        DisplayConfigEntry entry;
        entry.refresh_rate = Rational{path.targetInfo.refreshRate.Numerator, path.targetInfo.refreshRate.Denominator};
        entry.rotation_degrees = rotation_to_degrees(path.targetInfo.rotation);

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
            entry.friendly_name = detail::to_utf8(target.monitorFriendlyDeviceName);
        }

        DISPLAYCONFIG_TARGET_PREFERRED_MODE preferred{};
        preferred.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
        preferred.header.size = sizeof(preferred);
        preferred.header.adapterId = path.targetInfo.adapterId;
        preferred.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&preferred.header) == ERROR_SUCCESS) {
            entry.native_width = preferred.width;
            entry.native_height = preferred.height;
        }

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
        color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        color.header.size = sizeof(color);
        color.header.adapterId = path.targetInfo.adapterId;
        color.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) {
            entry.hdr_supported = color.advancedColorSupported != 0;
            entry.hdr_enabled = color.advancedColorEnabled != 0;
            entry.bits_per_color = color.bitsPerColorChannel;
        }

        result.emplace(gdi_name, std::move(entry));
    }
    return result;
}

// Datos por salida DXGI: a qué GPU está conectado el monitor y su espacio de color actual.
struct DxgiOutputEntry {
    std::uint32_t gpu_index = 0;
    std::uint32_t bits_per_color = 0;
    bool hdr10_active = false;
};

std::unordered_map<HMONITOR, DxgiOutputEntry> query_dxgi_outputs(const std::vector<GpuInfo>& gpus) {
    std::unordered_map<HMONITOR, DxgiOutputEntry> result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        DXGI_ADAPTER_DESC1 adesc{};
        if (FAILED(adapter->GetDesc1(&adesc))) continue;
        const std::uint64_t luid = detail::pack_luid(adesc.AdapterLuid);
        const auto gpu = std::ranges::find(gpus, luid, &GpuInfo::luid);
        if (gpu == gpus.end()) continue;

        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DxgiOutputEntry entry;
            entry.gpu_index = gpu->index;
            HMONITOR monitor = nullptr;

            ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 desc1{};
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&desc1))) {
                monitor = desc1.Monitor;
                entry.bits_per_color = desc1.BitsPerColor;
                entry.hdr10_active = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            } else if (SUCCEEDED(output->GetDesc(&desc))) {
                monitor = desc.Monitor;
            }
            if (monitor != nullptr) result.emplace(monitor, entry);
            output.Reset();
        }
        adapter.Reset();
    }
    return result;
}

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC /*hdc*/, LPRECT /*rect*/, LPARAM user) {
    reinterpret_cast<std::vector<HMONITOR>*>(user)->push_back(monitor);
    return TRUE;
}

}  // namespace

Result<std::vector<MonitorInfo>> enumerate_monitors(const std::vector<GpuInfo>& gpus) {
    std::vector<HMONITOR> handles;
    if (!EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&handles))) {
        return detail::hr_error("EnumDisplayMonitors", HRESULT_FROM_WIN32(GetLastError()));
    }

    const auto display_config = query_display_config();
    const auto dxgi_outputs = query_dxgi_outputs(gpus);

    std::vector<MonitorInfo> monitors;
    monitors.reserve(handles.size());
    for (HMONITOR handle : handles) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(handle, &mi)) continue;

        MonitorInfo m;
        m.gdi_name = detail::to_utf8(mi.szDevice);
        // Con Per-Monitor DPI Aware v2 estas coordenadas son píxeles físicos del escritorio virtual.
        m.desktop = Rect{mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top};
        m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

        UINT dpi_x = 96;
        UINT dpi_y = 96;
        if (SUCCEEDED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) m.dpi = dpi_x;
        m.scale_percent = (m.dpi * 100 + 48) / 96;

        if (const auto it = display_config.find(mi.szDevice); it != display_config.end()) {
            const DisplayConfigEntry& dc = it->second;
            m.friendly_name = dc.friendly_name;
            m.refresh_rate = dc.refresh_rate;
            m.native_width = dc.native_width;
            m.native_height = dc.native_height;
            m.rotation_degrees = dc.rotation_degrees;
            m.hdr_supported = dc.hdr_supported;
            m.hdr_enabled = dc.hdr_enabled;
            if (dc.bits_per_color != 0) m.bits_per_color = dc.bits_per_color;
        } else {
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
                m.refresh_rate = Rational{mode.dmDisplayFrequency, 1};
            }
        }

        if (const auto it = dxgi_outputs.find(handle); it != dxgi_outputs.end()) {
            m.gpu_index = it->second.gpu_index;
            if (it->second.bits_per_color != 0) m.bits_per_color = it->second.bits_per_color;
            // DXGI es la fuente fiable de "HDR10 activo"; advancedColorEnabled también se activa con ACM en SDR.
            m.hdr_enabled = it->second.hdr10_active;
            m.hdr_supported = m.hdr_supported || m.hdr_enabled;
        }

        if (m.native_width == 0 || m.native_height == 0) {
            m.native_width = static_cast<std::uint32_t>(m.desktop.width);
            m.native_height = static_cast<std::uint32_t>(m.desktop.height);
        }
        monitors.push_back(std::move(m));
    }

    // Numeración estable de izquierda a derecha y de arriba abajo (la usan los atajos Ctrl+Alt+Shift+N).
    std::ranges::sort(monitors, [](const MonitorInfo& a, const MonitorInfo& b) {
        return a.desktop.x != b.desktop.x ? a.desktop.x < b.desktop.x : a.desktop.y < b.desktop.y;
    });
    for (std::size_t i = 0; i < monitors.size(); ++i) monitors[i].id = static_cast<std::uint32_t>(i + 1);
    return monitors;
}

}  // namespace lancast::platform
