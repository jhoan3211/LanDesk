#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <array>
#include <cstdlib>

#include "lancast/platform/platform.hpp"
#include "win.hpp"

namespace lancast::platform {
namespace {

using detail::ComPtr;

// Subtipos de vídeo de Media Foundation construidos desde su FOURCC (evita depender de la versión del SDK
// para MFVideoFormat_AV1).
constexpr GUID media_subtype(std::uint32_t fourcc) noexcept {
    return GUID{fourcc, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
}

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}

GUID subtype_for(Codec codec) noexcept {
    switch (codec) {
        case Codec::H264:
            return media_subtype(fourcc('H', '2', '6', '4'));
        case Codec::Hevc:
            return media_subtype(fourcc('H', 'E', 'V', 'C'));
        case Codec::Av1:
            return media_subtype(fourcc('A', 'V', '0', '1'));
    }
    return GUID_NULL;
}

// DLL de runtime que carga FFmpeg para cada backend. Basta con comprobar que existe en System32:
// el driver la instala ahí. (QSV usa el dispatcher oneVPL integrado en FFmpeg y localiza el runtime en el
// DriverStore, por eso puede no aparecer en System32 aunque funcione; se valida al abrir el codificador en la fase 2.)
struct RuntimeCheck {
    std::string label;
    bool present = false;
};

RuntimeCheck check_runtime(EncoderBackend backend) {
    std::array<const wchar_t*, 3> candidates{};
    switch (backend) {
        case EncoderBackend::Nvenc:
            candidates = {L"nvEncodeAPI64.dll", nullptr, nullptr};
            break;
        case EncoderBackend::Amf:
            candidates = {L"amfrt64.dll", nullptr, nullptr};
            break;
        case EncoderBackend::Qsv:
            candidates = {L"libmfx64-gen.dll", L"libmfxhw64.dll", L"libvpl.dll"};
            break;
    }

    wchar_t system_dir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(system_dir, MAX_PATH);
    RuntimeCheck check;
    for (const wchar_t* name : candidates) {
        if (name == nullptr) continue;
        if (!check.label.empty()) check.label += " | ";
        check.label += detail::to_utf8(name);
        if (len == 0) continue;
        const std::wstring path = std::wstring{system_dir, len} + L"\\" + name;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) check.present = true;
    }
    return check;
}

std::string read_string(IMFActivate* activate, const GUID& key) {
    wchar_t* value = nullptr;
    UINT32 length = 0;
    if (FAILED(activate->GetAllocatedString(key, &value, &length)) || value == nullptr) return {};
    std::string out = detail::to_utf8(std::wstring_view{value, length});
    CoTaskMemFree(value);
    return out;
}

// Vendor del MFT ("VEN_10DE") convertido a id PCI.
std::uint32_t read_vendor_id(IMFActivate* activate) {
    const std::string ven = read_string(activate, MFT_ENUM_HARDWARE_VENDOR_ID_Attribute);
    if (ven.size() < 8 || !ven.starts_with("VEN_")) return 0;
    return static_cast<std::uint32_t>(std::strtoul(ven.c_str() + 4, nullptr, 16));
}

struct MftMatch {
    std::string friendly_name;
    std::uint32_t vendor_id = 0;
};

// Enumera MFTs codificadores de hardware para un subtipo. Con `adapter` usa MFTEnum2 (filtra por GPU).
Result<std::vector<MftMatch>> find_hardware_mfts(const GUID& subtype, IMFAttributes* adapter) {
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, subtype};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    constexpr UINT32 kFlags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;

    const HRESULT hr = adapter != nullptr
                           ? MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, kFlags, nullptr, &output, adapter, &activates, &count)
                           : MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, kFlags, nullptr, &output, &activates, &count);
    if (FAILED(hr)) return detail::hr_error(adapter != nullptr ? "MFTEnum2" : "MFTEnumEx", hr);

    std::vector<MftMatch> matches;
    for (UINT32 i = 0; i < count; ++i) {
        matches.push_back(MftMatch{read_string(activates[i], MFT_FRIENDLY_NAME_Attribute), read_vendor_id(activates[i])});
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
    return matches;
}

class MediaFoundationScope {
public:
    MediaFoundationScope() noexcept : hr_(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)) {}
    ~MediaFoundationScope() {
        if (SUCCEEDED(hr_)) MFShutdown();
    }
    MediaFoundationScope(const MediaFoundationScope&) = delete;
    MediaFoundationScope& operator=(const MediaFoundationScope&) = delete;
    [[nodiscard]] HRESULT result() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

}  // namespace

Result<std::vector<EncoderInfo>> enumerate_encoders(const std::vector<GpuInfo>& gpus) {
    detail::ComScope com;
    MediaFoundationScope mf;
    if (FAILED(mf.result())) return detail::hr_error("MFStartup", mf.result());

    std::vector<EncoderInfo> encoders;
    for (const GpuInfo& gpu : gpus) {
        if (gpu.is_software) continue;
        const auto backend = backend_for_vendor(gpu.vendor);
        if (!backend) continue;
        const RuntimeCheck runtime = check_runtime(*backend);

        ComPtr<IMFAttributes> adapter_attrs;
        if (SUCCEEDED(MFCreateAttributes(&adapter_attrs, 1))) {
            LUID luid{};
            luid.LowPart = static_cast<DWORD>(gpu.luid & 0xFFFFFFFFu);
            luid.HighPart = static_cast<LONG>(gpu.luid >> 32);
            if (FAILED(adapter_attrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&luid),
                                              sizeof(luid)))) {
                adapter_attrs.Reset();
            }
        }

        for (const Codec codec : kAllCodecs) {
            auto mfts = find_hardware_mfts(subtype_for(codec), adapter_attrs.Get());
            if (!mfts) mfts = find_hardware_mfts(subtype_for(codec), nullptr);  // Windows sin MFTEnum2.
            if (!mfts) return std::unexpected(mfts.error());

            const auto match = std::ranges::find_if(*mfts, [&](const MftMatch& m) {
                return m.vendor_id == 0 || vendor_from_pci_id(m.vendor_id) == gpu.vendor;
            });
            if (match == mfts->end()) continue;

            EncoderInfo info;
            info.codec = codec;
            info.backend = *backend;
            info.ffmpeg_name = ffmpeg_encoder_name(codec, *backend);
            info.gpu_index = gpu.index;
            info.mft_name = match->friendly_name;
            info.runtime_dll = runtime.label;
            info.runtime_present = runtime.present;
            encoders.push_back(std::move(info));
        }
    }
    return encoders;
}

}  // namespace lancast::platform
