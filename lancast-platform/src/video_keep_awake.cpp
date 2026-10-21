#include <objbase.h>

#include <mutex>
#include <vector>

#include "lancast/core/log.hpp"
#include "lancast/platform/mf_codec.hpp"

namespace lancast::mf {

namespace {

// Cuadro clave H.264 diminuto (negro, 256x144). H.264 porque toda GPU con codificador lo soporta, y la unidad de
// video que se quiere mantener despierta es la misma para todos los códecs.
std::vector<std::uint8_t> tiny_keyframe(const gpu::Device& dev) {
    const Size ts{256, 144};
    auto tex = gpu::create_texture(dev.device.Get(), ts.width, ts.height, DXGI_FORMAT_NV12,
                                   D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    if (!tex) return {};
    EncoderConfig cfg;
    cfg.codec = Codec::H264;
    cfg.size = ts;
    cfg.bitrate_kbps = 500;
    auto enc = create_encoder(dev, cfg);
    if (!enc) return {};
    std::mutex m;
    std::vector<std::uint8_t> out;
    (*enc)->set_output_callback([&](EncodedPacket&& p) {
        std::scoped_lock lock(m);
        if (out.empty() && p.keyframe) out = std::move(p.data);
    });
    if (!(*enc)->submit(tex->Get(), 16'666, true)) return {};
    for (int i = 0; i < 100; ++i) {
        Sleep(10);
        std::scoped_lock lock(m);
        if (!out.empty()) break;
    }
    std::scoped_lock lock(m);
    return std::move(out);
}

}  // namespace

VideoEngineKeepAwake::VideoEngineKeepAwake(std::uint64_t adapter_luid, int period_ms)
    : thread_([this, adapter_luid, period_ms](std::stop_token stop) {
          const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
          // Dispositivo propio en la misma GPU: no comparte contexto con el hilo que dibuja.
          auto dev = gpu::create_device(adapter_luid);
          std::vector<std::uint8_t> frame;
          std::unique_ptr<Decoder> dec;
          if (dev) frame = tiny_keyframe(*dev);
          if (!frame.empty()) {
              if (auto d = create_decoder(*dev, Codec::H264, Size{256, 144})) dec = std::move(*d);
          }
          if (!dec) {
              log::info("Unidad de video despierta: no disponible en esta GPU");
          } else {
              active_ = true;
              log::info("Unidad de video despierta: activa (cada {} ms)", period_ms);
              std::uint64_t ts = 0;
              while (!stop.stop_requested()) {
                  if (!dec->decode(frame, ts += 1000)) break;
                  Sleep(static_cast<DWORD>(period_ms));
              }
              active_ = false;
          }
          dec.reset();
          if (SUCCEEDED(co)) CoUninitialize();
      }) {}

VideoEngineKeepAwake::~VideoEngineKeepAwake() = default;

}  // namespace lancast::mf
