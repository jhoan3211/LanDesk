#include <d3d11.h>

#include <algorithm>
#include <array>

#include "lancast/platform/platform.hpp"
#include "win.hpp"

namespace lancast::platform {
namespace {

using detail::ComPtr;

struct KnownProfile {
    GUID guid;
    Codec codec;
    const char* name;
    DXGI_FORMAT format;
    bool ten_bit;
    bool chroma_444;
};

// GUIDs de DXVA (d3d11.h / dxva.h). Los de HEVC 4:4:4 no están en todos los SDK, por eso van literales.
const std::array<KnownProfile, 8>& known_profiles() {
    static const std::array<KnownProfile, 8> kProfiles{{
        {D3D11_DECODER_PROFILE_H264_VLD_NOFGT, Codec::H264, "High (VLD)", DXGI_FORMAT_NV12, false, false},
        {D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, Codec::Hevc, "Main", DXGI_FORMAT_NV12, false, false},
        {D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, Codec::Hevc, "Main10", DXGI_FORMAT_P010, true, false},
        {GUID{0x4008018f, 0xf537, 0x4b36, {0x98, 0xcf, 0x61, 0xaf, 0x8a, 0x2c, 0x1a, 0x33}}, Codec::Hevc,
         "Main 4:4:4", DXGI_FORMAT_AYUV, false, true},
        {GUID{0x0dabeffa, 0x4458, 0x4602, {0xbc, 0x03, 0x07, 0x95, 0x65, 0x9d, 0x61, 0x7c}}, Codec::Hevc,
         "Main10 4:4:4", DXGI_FORMAT_Y410, true, true},
        {D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, Codec::Av1, "Main (P0)", DXGI_FORMAT_NV12, false, false},
        {D3D11_DECODER_PROFILE_AV1_VLD_PROFILE1, Codec::Av1, "High 4:4:4 (P1)", DXGI_FORMAT_AYUV, false, true},
        {D3D11_DECODER_PROFILE_AV1_VLD_PROFILE2, Codec::Av1, "Professional (P2)", DXGI_FORMAT_NV12, false, false},
    }};
    return kProfiles;
}

bool supports_size(ID3D11VideoDevice* video, const GUID& profile, DXGI_FORMAT format, UINT width, UINT height) {
    D3D11_VIDEO_DECODER_DESC desc{};
    desc.Guid = profile;
    desc.SampleWidth = width;
    desc.SampleHeight = height;
    desc.OutputFormat = format;
    UINT configs = 0;
    return SUCCEEDED(video->GetVideoDecoderConfigCount(&desc, &configs)) && configs > 0;
}

}  // namespace

Result<DecoderProbe> enumerate_decoders(const std::vector<GpuInfo>& gpus) {
    ComPtr<IDXGIFactory1> factory;
    if (const HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory)); FAILED(hr)) {
        return detail::hr_error("CreateDXGIFactory1", hr);
    }

    DecoderProbe probe;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 adesc{};
        if (FAILED(adapter->GetDesc1(&adesc))) continue;
        const auto gpu = std::ranges::find(gpus, detail::pack_luid(adesc.AdapterLuid), &GpuInfo::luid);
        if (gpu == gpus.end() || gpu->is_software) continue;

        constexpr std::array kLevels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        ComPtr<ID3D11Device> device;
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT, kLevels.data(),
                                       static_cast<UINT>(kLevels.size()), D3D11_SDK_VERSION, &device, nullptr,
                                       nullptr);
        if (FAILED(hr)) continue;

        ComPtr<ID3D11VideoDevice> video;
        if (FAILED(device.As(&video))) continue;

        const UINT count = video->GetVideoDecoderProfileCount();
        for (UINT i = 0; i < count; ++i) {
            GUID guid{};
            if (FAILED(video->GetVideoDecoderProfile(i, &guid))) continue;

            const auto known = std::ranges::find_if(known_profiles(), [&](const KnownProfile& p) {
                return IsEqualGUID(p.guid, guid) != 0;
            });
            if (known == known_profiles().end()) {
                probe.unknown_profiles.push_back(UnknownDecoderProfile{gpu->index, detail::guid_to_string(guid)});
                continue;
            }
            // El mismo GUID puede aparecer más de una vez en algunos drivers.
            if (std::ranges::any_of(probe.decoders, [&](const DecoderInfo& d) {
                    return d.gpu_index == gpu->index && d.codec == known->codec && d.profile == known->name;
                })) {
                continue;
            }

            BOOL format_ok = FALSE;
            if (FAILED(video->CheckVideoDecoderFormat(&guid, known->format, &format_ok)) || !format_ok) continue;

            DecoderInfo info;
            info.codec = known->codec;
            info.profile = known->name;
            info.gpu_index = gpu->index;
            info.ten_bit = known->ten_bit;
            info.chroma_444 = known->chroma_444;
            for (const auto [w, h] : {std::pair{1920u, 1080u}, std::pair{2560u, 1440u}, std::pair{3840u, 2160u},
                                      std::pair{7680u, 4320u}}) {
                if (!supports_size(video.Get(), guid, known->format, w, h)) break;
                info.max_width = w;
                info.max_height = h;
            }
            if (info.max_width == 0) continue;
            probe.decoders.push_back(std::move(info));
        }
    }
    return probe;
}

}  // namespace lancast::platform
