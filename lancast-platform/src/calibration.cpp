#include "lancast/platform/calibration.hpp"

#include <dxgi1_6.h>
#include <timeapi.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <vector>

#include "lancast/core/bitrate.hpp"
#include "lancast/core/log.hpp"
#include "lancast/platform/color.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/system.hpp"

#pragma comment(lib, "winmm.lib")

namespace lancast::calib {
namespace {

std::filesystem::path file() { return sys::app_data_dir() / L"calibracion.txt"; }

// "Escritorio" sintético con detalle fino (texto) y degradados; `shift` corre el contenido para que haya movimiento.
gpu::ComPtr<ID3D11Texture2D> make_frame(const gpu::Device& dev, Size size, int shift) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(size.width) * size.height);
    std::mt19937 rng(static_cast<std::uint32_t>(7 + shift));
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const std::uint32_t xs = x + static_cast<std::uint32_t>(shift);
            const std::uint32_t r = (xs * 255 / size.width) & 0xFF;
            const std::uint32_t g = (y * 255 / size.height) & 0xFF;
            std::uint32_t b = ((xs / 64 + y / 64) % 2) ? 200 : 60;
            if ((y % 40) < 14 && (xs % 300) < 220) b = (rng() & 1) ? 20 : 235;
            px[static_cast<std::size_t>(y) * size.width + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = size.width;
    d.Height = size.height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const D3D11_SUBRESOURCE_DATA init{px.data(), size.width * 4, 0};
    gpu::ComPtr<ID3D11Texture2D> t;
    dev.device->CreateTexture2D(&d, &init, &t);
    return t;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::ranges::sort(v);
    return v[v.size() / 2];
}

// Un modo (4:2:0 o 4:4:4): cuadros a 60 fps durante ~0,8 s; devuelve la mediana de envío -> bitstream.
platform::Result<double> run_mode(const gpu::Device& dev, const std::array<gpu::ComPtr<ID3D11Texture2D>, 2>& frames, Size native,
                                  Codec codec, bool c444, const std::atomic<bool>& cancel) {
    auto packer = gpu::YuvPacker::create(dev, gpu::PackConfig{native, DXGI_FORMAT_B8G8R8A8_UNORM, native, c444, false});
    if (!packer) return std::unexpected(packer.error());
    const Size coded = (*packer)->coded_size();
    std::array<gpu::ComPtr<ID3D11Texture2D>, 5> pool;
    for (auto& t : pool) {
        auto tex = gpu::create_texture(dev.device.Get(), coded.width, coded.height, DXGI_FORMAT_NV12,
                                       D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        if (!tex) return std::unexpected(tex.error());
        t = *tex;
    }
    const std::uint32_t mbps = std::clamp<std::uint32_t>(auto_bitrate_mbps(coded.width, coded.height, 60, codec), 20, 400);
    mf::EncoderConfig ec{codec, coded, 60, mbps * 1000, RateControl::Cbr, true};
    auto enc = mf::create_encoder(dev, ec);
    if (!enc) return std::unexpected(enc.error());
    std::mutex m;
    std::map<std::uint64_t, std::uint64_t> sent;
    std::vector<double> latency;
    (*enc)->set_output_callback([&](mf::EncodedPacket&& p) {
        const std::uint64_t now = gpu::now_us();
        std::scoped_lock lock(m);
        if (!p.last_slice) return;
        if (const auto it = sent.find(p.timestamp_us); it != sent.end() && p.timestamp_us > 8 * 16'666) {
            latency.push_back(static_cast<double>(now - it->second) / 1000.0);  // Sin los primeros (arranque).
        }
    });
    const std::uint64_t interval = 1'000'000 / 60;
    std::uint64_t next = gpu::now_us();
    for (int i = 0; i < 50 && !cancel; ++i) {
        while (gpu::now_us() < next) Sleep(1);
        next += interval;
        ID3D11Texture2D* target = pool[static_cast<std::size_t>(i) % pool.size()].Get();
        const std::uint64_t ts = static_cast<std::uint64_t>(i + 1) * interval;
        const std::uint64_t t0 = gpu::now_us();
        (*packer)->process(frames[static_cast<std::size_t>(i) % 2].Get(), target);
        {
            std::scoped_lock lock(m);
            sent[ts] = t0;
        }
        if (auto ok = (*enc)->submit(target, ts, i == 0); !ok) return std::unexpected(ok.error());
    }
    Sleep(150);
    std::scoped_lock lock(m);
    if (cancel) return std::unexpected(platform::Error{"Calibración cancelada", 0});
    return median(latency);
}

}  // namespace

std::string gpu_key(std::uint64_t luid) {
    gpu::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
    gpu::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        const std::uint64_t l = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                                desc.AdapterLuid.LowPart;
        if (l != luid) continue;
        LARGE_INTEGER umd{};
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
        const auto v = static_cast<std::uint64_t>(umd.QuadPart);
        return std::format("{:04x}:{:04x}:{}.{}.{}.{}", desc.VendorId, desc.DeviceId, v >> 48, (v >> 32) & 0xFFFF, (v >> 16) & 0xFFFF,
                           v & 0xFFFF);
    }
    return {};
}

platform::Result<Result> measure(std::uint64_t luid, Size native, const std::atomic<bool>& cancel) {
    native = Size{native.width & ~1u, native.height & ~1u};
    auto dev = gpu::create_device(luid);
    if (!dev) return std::unexpected(dev.error());
    const auto codecs = mf::available_encoders(dev->luid);
    if (codecs.empty()) return std::unexpected(platform::Error{"Sin codificador por hardware", 0});
    const Codec codec = std::ranges::find(codecs, Codec::Hevc) != codecs.end() ? Codec::Hevc : codecs.front();
    timeBeginPeriod(1);
    const std::array<gpu::ComPtr<ID3D11Texture2D>, 2> frames{make_frame(*dev, native, 0), make_frame(*dev, native, 37)};
    Result r;
    r.gpu_key = gpu_key(luid);
    r.codec = codec;
    r.native = native;
    auto a = run_mode(*dev, frames, native, codec, false, cancel);
    auto b = a ? run_mode(*dev, frames, native, codec, true, cancel) : platform::Result<double>{std::unexpected(a.error())};
    timeEndPeriod(1);
    if (!a) return std::unexpected(a.error());
    r.ms_420 = *a;
    r.ms_444 = b ? *b : 0;  // Sin 4:4:4 (p. ej. tamaño no admitido): solo se ofrece 4:2:0.
    if (cancel) return std::unexpected(platform::Error{"Calibración cancelada", 0});
    return r;
}

std::optional<Result> load(const std::string& key, Size native) {
    std::ifstream f(file());
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        Result r;
        int codec = 0;
        if (std::getline(in, r.gpu_key, '|') && in >> codec && in.ignore() && in >> r.native.width && in.ignore() &&
            in >> r.native.height && in.ignore() && in >> r.ms_420 && in.ignore() && in >> r.ms_444) {
            r.codec = static_cast<Codec>(codec);
            if (r.gpu_key == key && r.native == native) return r;
        }
    }
    return std::nullopt;
}

void save(const Result& r) {
    std::vector<std::string> lines;
    {
        std::ifstream f(file());
        std::string line;
        const std::string prefix = r.gpu_key + "|";
        const std::string size = std::format("|{}|{}|", r.native.width, r.native.height);
        while (std::getline(f, line)) {
            if (!(line.starts_with(prefix) && line.find(size) != std::string::npos)) lines.push_back(line);
        }
    }
    lines.push_back(std::format("{}|{}|{}|{}|{:.2f}|{:.2f}", r.gpu_key, static_cast<int>(r.codec), r.native.width, r.native.height,
                                r.ms_420, r.ms_444));
    std::ofstream out(file(), std::ios::trunc);
    for (const auto& l : lines) out << l << '\n';
}

}  // namespace lancast::calib
