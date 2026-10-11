#include "lancast/core/types.hpp"

#include <algorithm>

namespace lancast {

GpuVendor vendor_from_pci_id(std::uint32_t pci_vendor_id) noexcept {
    switch (pci_vendor_id) {
        case 0x10DE:
            return GpuVendor::Nvidia;
        case 0x1002:
        case 0x1022:
            return GpuVendor::Amd;
        case 0x8086:
        case 0x8087:
            return GpuVendor::Intel;
        case 0x1414:
            return GpuVendor::Microsoft;
        case 0x5143:
        case 0x4D4F4351:  // "QCOM" en adaptadores ARM64.
            return GpuVendor::Qualcomm;
        default:
            return GpuVendor::Unknown;
    }
}

std::optional<EncoderBackend> backend_for_vendor(GpuVendor vendor) noexcept {
    switch (vendor) {
        case GpuVendor::Nvidia:
            return EncoderBackend::Nvenc;
        case GpuVendor::Amd:
            return EncoderBackend::Amf;
        case GpuVendor::Intel:
            return EncoderBackend::Qsv;
        default:
            return std::nullopt;
    }
}

std::string_view to_string(Codec codec) noexcept {
    switch (codec) {
        case Codec::H264:
            return "H.264";
        case Codec::Hevc:
            return "HEVC";
        case Codec::Av1:
            return "AV1";
    }
    return "?";
}

std::string_view to_string(GpuVendor vendor) noexcept {
    switch (vendor) {
        case GpuVendor::Nvidia:
            return "NVIDIA";
        case GpuVendor::Amd:
            return "AMD";
        case GpuVendor::Intel:
            return "Intel";
        case GpuVendor::Microsoft:
            return "Microsoft";
        case GpuVendor::Qualcomm:
            return "Qualcomm";
        case GpuVendor::Unknown:
            return "Desconocido";
    }
    return "?";
}

std::string_view to_string(EncoderBackend backend) noexcept {
    switch (backend) {
        case EncoderBackend::Nvenc:
            return "NVENC";
        case EncoderBackend::Amf:
            return "AMF";
        case EncoderBackend::Qsv:
            return "QuickSync";
    }
    return "?";
}

std::string ffmpeg_encoder_name(Codec codec, EncoderBackend backend) {
    std::string name;
    switch (codec) {
        case Codec::H264:
            name = "h264";
            break;
        case Codec::Hevc:
            name = "hevc";
            break;
        case Codec::Av1:
            name = "av1";
            break;
    }
    switch (backend) {
        case EncoderBackend::Nvenc:
            name += "_nvenc";
            break;
        case EncoderBackend::Amf:
            name += "_amf";
            break;
        case EncoderBackend::Qsv:
            name += "_qsv";
            break;
    }
    return name;
}

bool Rect::contains(const Rect& other) const noexcept {
    return other.x >= x && other.y >= y && other.right() <= right() && other.bottom() <= bottom();
}

Rect Rect::united(const Rect& other) const noexcept {
    if (width <= 0 || height <= 0) return other;
    if (other.width <= 0 || other.height <= 0) return *this;
    const std::int32_t left = std::min(x, other.x);
    const std::int32_t top = std::min(y, other.y);
    const std::int32_t r = std::max(right(), other.right());
    const std::int32_t b = std::max(bottom(), other.bottom());
    return Rect{left, top, r - left, b - top};
}

}  // namespace lancast
