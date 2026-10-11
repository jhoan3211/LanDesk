#pragma once

#include <vector>

#include "lancast/core/types.hpp"
#include "lancast/platform/error.hpp"

namespace lancast::platform {

// Activa Per-Monitor DPI Aware v2 (también va en el manifiesto; esto es la red de seguridad).
void enable_per_monitor_dpi_awareness() noexcept;

// GPUs ordenadas por preferencia de rendimiento (índice 0 = la más potente).
[[nodiscard]] Result<std::vector<GpuInfo>> enumerate_gpus();

// Monitores activos, numerados de izquierda a derecha, con coordenadas negativas respetadas.
[[nodiscard]] Result<std::vector<MonitorInfo>> enumerate_monitors(const std::vector<GpuInfo>& gpus);

// Codificadores de hardware usables por FFmpeg (NVENC/AMF/QSV), detectados vía MFT de hardware del driver.
[[nodiscard]] Result<std::vector<EncoderInfo>> enumerate_encoders(const std::vector<GpuInfo>& gpus);

// Perfiles de decodificación D3D11VA por GPU.
struct DecoderProbe {
    std::vector<DecoderInfo> decoders;
    std::vector<UnknownDecoderProfile> unknown_profiles;
};
[[nodiscard]] Result<DecoderProbe> enumerate_decoders(const std::vector<GpuInfo>& gpus);

// Ejecuta todo lo anterior; los fallos parciales quedan como avisos en el reporte.
[[nodiscard]] SystemReport collect_system_report();

}  // namespace lancast::platform
