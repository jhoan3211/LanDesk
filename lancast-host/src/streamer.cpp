#include "streamer.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "lancast/core/adapt.hpp"
#include "lancast/core/bitrate.hpp"
#include "lancast/core/geometry.hpp"
#include "lancast/core/log.hpp"
#include "lancast/core/media.hpp"
#include "lancast/platform/capture.hpp"
#include "lancast/platform/mf_codec.hpp"
#include "lancast/platform/platform.hpp"
#include "lancast/platform/system.hpp"
#include "lancast/platform/video_processor.hpp"

namespace lancast::host {
namespace {

// Preferencia en modo automático: mejor compresión primero (menos bits = menos paquetes = menos latencia).
constexpr std::array kAutoOrder{Codec::Hevc, Codec::Av1, Codec::H264};

// Cuadros que pueden esperar en el codificador: más no aumenta el rendimiento, solo la latencia.
// (Media Foundation a veces retiene un cuadro más antes de entregar el primero.)
std::size_t max_queued(const mf::Encoder& e) { return e.backend() == "Media Foundation" ? 3 : 2; }


// Frame entregado al codificador y aún sin bitstream (se empareja por marca de tiempo).
struct InFlight {
    std::uint64_t ts = 0;
    std::uint64_t capture_us = 0;
    std::uint64_t encode_start_us = 0;
};

// Estado compartido con el hilo de Media Foundation que recoge los frames codificados.
struct EncodeContext {
    EncodeContext() : output_event(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~EncodeContext() { CloseHandle(output_event); }
    EncodeContext(const EncodeContext&) = delete;
    EncodeContext& operator=(const EncodeContext&) = delete;

    std::mutex mutex;
    HANDLE output_event;  // Se activa con cada cuadro terminado (contrapresión).
    std::deque<InFlight> in_flight;
    std::uint16_t epoch = 0;
    std::atomic<int> fec{15};
    std::uint32_t next_frame_id = 0;
    bool slices = false;
};

struct Pipeline {
    std::uint64_t luid = 0;
    gpu::Device device;
    std::unique_ptr<capture::DesktopCapture> capture;
    std::unique_ptr<gpu::VideoProcessor> vp;
    // Varias texturas: mientras el codificador lee una, la captura escribe la siguiente (en paralelo).
    std::array<gpu::ComPtr<ID3D11Texture2D>, 5> nv12;
    std::size_t nv12_index = 0;
    std::shared_ptr<EncodeContext> ctx;
    std::unique_ptr<mf::Encoder> encoder;  // Declarado después de ctx: se destruye antes.
    proto::StreamStarted info;
    proto::StreamConfig config;
    Size stream_size;
};

class CoInit {
public:
    CoInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~CoInit() { CoUninitialize(); }
};

}  // namespace

Streamer::Streamer(net::MediaSender& sender, Callbacks callbacks, std::uint8_t stream_index)
    : sender_(sender), callbacks_(std::move(callbacks)), stream_index_(stream_index) {
    thread_ = std::thread([this] { run(); });
    send_thread_ = std::thread([this] { sender_loop(); });
}

Streamer::~Streamer() { stop(); }

void Streamer::stop() {
    running_ = false;
    cv_.notify_all();
    send_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (send_thread_.joinable()) send_thread_.join();
}

void Streamer::configure(const proto::StreamConfig& config, const std::vector<Codec>& client_decoders,
                         std::uint32_t audio_rate, std::uint8_t audio_channels, bool client_slices) {
    std::scoped_lock lock(mutex_);
    pending_ = config;
    client_decoders_ = client_decoders;
    client_slices_ = client_slices;
    audio_rate_ = audio_rate;
    audio_channels_ = audio_channels;
    cv_.notify_all();
}

StreamStats Streamer::stats() const {
    std::scoped_lock lock(mutex_);
    return stats_;
}

std::string Streamer::encoder_name() const {
    std::scoped_lock lock(mutex_);
    return encoder_name_;
}

std::optional<Rect> Streamer::source() const {
    std::scoped_lock lock(mutex_);
    if (!current_) return std::nullopt;
    return current_->source;
}

std::optional<proto::StreamStarted> Streamer::current() const {
    std::scoped_lock lock(mutex_);
    return current_;
}

void Streamer::run() {
    CoInit com;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    sys::RealtimeThread mmcss(L"Capture");
    // Prioridad de GPU alta para el contexto de captura/codificación.
    std::unique_ptr<Pipeline> p;
    std::uint16_t epoch = 0;
    std::uint64_t last_ts = 0;

    bool dirty = false;
    std::uint64_t dirty_since_us = 0;
    std::uint64_t last_encode_us = 0;
    {
        std::scoped_lock lock(mutex_);
        stats_window_us_ = gpu::now_us();
    }
    capture::PointerState last_pointer{};
    std::uint64_t last_pointer_send_us = 0;

    auto fail = [&](const std::string& msg) {
        log::error("Stream: {}", msg);
        if (callbacks_.error) callbacks_.error(msg);
        p.reset();
    };

    auto build = [&](const proto::StreamConfig& cfg, const std::vector<Codec>& decoders, std::uint32_t audio_rate,
                     std::uint8_t audio_channels, bool client_slices) {
        auto gpus = platform::enumerate_gpus();
        if (!gpus) return fail(gpus.error().message());
        auto monitors = platform::enumerate_monitors(*gpus);
        if (!monitors || monitors->empty()) return fail("No se detectaron monitores");

        const auto primary = std::ranges::find_if(*monitors, [](const MonitorInfo& m) { return m.primary; });
        const MonitorInfo* chosen = nullptr;
        if (cfg.monitor_id != 0) {
            const auto it = std::ranges::find(*monitors, cfg.monitor_id, &MonitorInfo::id);
            chosen = it != monitors->end() ? &*it : &*primary;
        }
        const MonitorInfo& anchor = chosen != nullptr ? *chosen : *primary;
        const std::uint32_t gpu_index = anchor.gpu_index.value_or(0);
        if (gpu_index >= gpus->size()) return fail("El monitor no tiene GPU asociada");
        const std::uint64_t luid = (*gpus)[gpu_index].luid;

        std::vector<Rect> rects;
        Rect source;
        std::uint32_t max_refresh = 30;
        std::string notes;
        if (chosen != nullptr) {
            rects.push_back(chosen->desktop);
            source = chosen->desktop;
            max_refresh = static_cast<std::uint32_t>(std::lround(chosen->refresh_rate.value()));
        } else {
            for (const auto& m : *monitors) {
                if (m.gpu_index.value_or(~0u) != gpu_index) {
                    notes += std::format("El monitor {} está en otra GPU y no se incluye. ", m.id);
                    continue;
                }
                rects.push_back(m.desktop);
                source = source.united(m.desktop);
                max_refresh = std::max(max_refresh, static_cast<std::uint32_t>(std::lround(m.refresh_rate.value())));
            }
        }

        auto next = std::make_unique<Pipeline>();
        if (p && p->luid == luid) {
            next->device = p->device;  // Reutilizar el dispositivo D3D11.
        } else {
            auto dev = gpu::create_device(luid);
            if (!dev) return fail(dev.error().message());
            next->device = std::move(*dev);
        }
        next->luid = luid;
        p.reset();  // Liberar la duplicación anterior antes de crear la nueva.

        auto cap = capture::DesktopCapture::create(next->device, rects, source);
        if (!cap) return fail(cap.error().message());
        next->capture = std::move(*cap);

        const Size source_size{static_cast<std::uint32_t>(source.width), static_cast<std::uint32_t>(source.height)};
        Size stream_size = fit_stream_size(source_size, {cfg.max_width, cfg.max_height});
        std::uint32_t fps = std::clamp<std::uint32_t>(cfg.fps, 10, std::max<std::uint32_t>(max_refresh, 30));
        if (fps < cfg.fps) notes += std::format("FPS limitados a {} (frecuencia del monitor). ", fps);
        if (adapt_scale_ < 0.999) {
            stream_size = Size{std::max<std::uint32_t>(static_cast<std::uint32_t>(stream_size.width * adapt_scale_) & ~1u, 320),
                               std::max<std::uint32_t>(static_cast<std::uint32_t>(stream_size.height * adapt_scale_) & ~1u, 240)};
        }
        if (adapt_fps_ != 0) fps = std::min(fps, adapt_fps_);
        std::string reason;
        {
            std::scoped_lock lock(mutex_);
            reason = std::move(adapt_reason_);
            adapt_reason_.clear();
        }
        if (!reason.empty()) {
            notes += std::format("Ajuste automático: {}; ahora {}x{} @ {} fps. ", reason, stream_size.width,
                                 stream_size.height, fps);
        }

        // Códec: el pedido si ambos lo soportan; si no, el mejor común.
        const auto encoders = mf::available_encoders(luid);
        auto both = [&](Codec c) {
            return std::ranges::find(encoders, c) != encoders.end() && std::ranges::find(decoders, c) != decoders.end();
        };
        std::vector<Codec> order;
        if (cfg.codec != CodecChoice::Auto) {
            const Codec wanted = static_cast<Codec>(static_cast<int>(cfg.codec) - 1);
            if (both(wanted)) {
                order.push_back(wanted);
            } else {
                notes += std::format("{} no está disponible en ambos equipos. ", to_string(wanted));
            }
        }
        for (const Codec c : kAutoOrder) {
            if (both(c) && std::ranges::find(order, c) == order.end()) order.push_back(c);
        }
        if (order.empty()) return fail("No hay un códec en común (el host no codifica lo que el cliente decodifica)");
        if (cfg.chroma_444) notes += "4:4:4 no está disponible con este codificador: se usa 4:2:0. ";
        if (cfg.hdr) notes += "HDR de 10 bits no está disponible con este codificador: se usa SDR de 8 bits. ";

        auto vp = gpu::VideoProcessor::create(next->device, next->capture->texture_size(), stream_size,
                                              gpu::ColorConversion::RgbToYuv709);
        if (!vp) return fail(vp.error().message());
        next->vp = std::move(*vp);
        for (auto& t : next->nv12) {
            auto tex = gpu::create_texture(next->device.device.Get(), stream_size.width, stream_size.height, DXGI_FORMAT_NV12,
                                           D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            if (!tex) return fail(tex.error().message());
            t = *tex;
        }

        Codec codec = order.front();
        std::uint32_t bitrate_kbps = 0;
        for (const Codec c : order) {
            bitrate_kbps = cfg.auto_bitrate ? auto_bitrate_mbps(stream_size.width, stream_size.height, fps, c) * 1000
                                            : std::clamp<std::uint32_t>(cfg.bitrate_kbps, kMinBitrateMbps * 1000,
                                                                        kMaxBitrateMbps * 1000);
            mf::EncoderConfig ec{c, stream_size, fps, bitrate_kbps, cfg.rate_control, cfg.low_latency};
            ec.direct = true;
            ec.intra_refresh = cfg.intra_refresh;
            ec.slices = cfg.slices && client_slices;
            auto enc = mf::create_encoder(next->device, ec);
            if (enc) {
                next->encoder = std::move(*enc);
                codec = c;
                break;
            }
            log::warn("No se pudo crear el codificador {}: {}", to_string(c), enc.error().message());
        }
        if (!next->encoder) return fail("No se pudo iniciar ningún codificador por hardware");

        ++epoch;
        next->ctx = std::make_shared<EncodeContext>();
        next->ctx->epoch = epoch;
        next->ctx->fec = static_cast<int>(cfg.fec_percent);
        next->ctx->slices = next->encoder->slices_active();
        // Salida en paralelo: cada frame codificado va directo al hilo de envío.
        next->encoder->set_output_callback([this, ctx = next->ctx](mf::EncodedPacket&& pkt) {
            const std::uint64_t encoded_us = gpu::now_us();
            InFlight f{pkt.timestamp_us, pkt.timestamp_us, encoded_us};
            media::EncodedFrameInfo info;
            {
                std::scoped_lock lock(ctx->mutex);
                while (!ctx->in_flight.empty() && ctx->in_flight.front().ts < pkt.timestamp_us) ctx->in_flight.pop_front();
                if (!ctx->in_flight.empty() && ctx->in_flight.front().ts == pkt.timestamp_us) {
                    f = ctx->in_flight.front();
                    if (pkt.last_slice) ctx->in_flight.pop_front();
                }
                info.frame_id = ++ctx->next_frame_id;
                info.epoch = ctx->epoch;
            }
            info.keyframe = pkt.keyframe;
            info.stream_index = stream_index_;
            info.slice = ctx->slices;
            info.slice_index = pkt.slice_index;
            info.last_slice = pkt.last_slice;
            info.capture_time_us = f.capture_us;
            info.encode_time_us = static_cast<std::uint32_t>(encoded_us - std::min(f.capture_us, encoded_us));
            const std::size_t bytes = pkt.data.size();
            {
                std::scoped_lock lock(send_mutex_);
                send_queue_.push_back(SendJob{std::move(pkt.data), info, ctx->fec.load()});
            }
            send_cv_.notify_one();
            if (pkt.last_slice) SetEvent(ctx->output_event);

            std::scoped_lock lock(mutex_);
            window_bytes_ += bytes;
            if (!pkt.last_slice) return;  // Las estadísticas por cuadro se cuentan con el último slice.
            ++stats_.frames;
            if (info.keyframe) ++stats_.keyframes;
            ++window_frames_;
            window_latency_ms_ += info.encode_time_us / 1000.0;
            window_wait_ms_ += static_cast<double>(f.encode_start_us - std::min(f.capture_us, f.encode_start_us)) / 1000.0;
            window_encode_ms_ += static_cast<double>(encoded_us - std::min(f.encode_start_us, encoded_us)) / 1000.0;
            if (encoded_us - stats_window_us_ >= 1'000'000) {
                const double secs = static_cast<double>(encoded_us - stats_window_us_) / 1e6;
                const double n = static_cast<double>(window_frames_);
                stats_.fps = n / secs;
                stats_.bitrate_mbps = static_cast<double>(window_bytes_) * 8.0 / secs / 1e6;
                stats_.capture_to_encoded_ms = n > 0 ? window_latency_ms_ / n : 0;
                stats_.capture_wait_ms = n > 0 ? window_wait_ms_ / n : 0;
                stats_.encode_ms = n > 0 ? window_encode_ms_ / n : 0;
                const double offered = n + static_cast<double>(window_skipped_);
                stats_.skipped_percent = offered > 0 ? 100.0 * static_cast<double>(window_skipped_) / offered : 0;
                // ¿Sobrecarga? El codificador tarda más que el intervalo entre cuadros, o se saltan >10 % de los cuadros.
                const double interval_ms = static_cast<double>(window_interval_us_) / 1000.0;
                const bool overloaded = adapt::overloaded(stats_.encode_ms, interval_ms, stats_.skipped_percent, window_frames_);
                overload_windows_ = overloaded ? overload_windows_ + 1 : 0;
                if (overload_windows_ >= 2 && encoded_us >= adapt_hold_until_us_) {
                    overload_windows_ = 0;
                    adapt_reason_ = stats_.encode_ms > interval_ms
                                        ? std::format("el codificador tardaba {:.1f} ms por cuadro (hay {:.1f} ms entre cuadros)",
                                                      stats_.encode_ms, interval_ms)
                                        : std::format("el codificador no daba abasto ({:.0f} % de cuadros saltados)",
                                                      stats_.skipped_percent);
                    adapt_requested_ = true;
                }
                window_skipped_ = 0;
                stats_window_us_ = encoded_us;
                window_frames_ = 0;
                window_bytes_ = 0;
                window_latency_ms_ = 0;
                window_wait_ms_ = 0;
                window_encode_ms_ = 0;
            }
        });
        next->config = cfg;
        next->stream_size = stream_size;
        next->info.epoch = epoch;
        next->info.codec = codec;
        next->info.width = stream_size.width;
        next->info.height = stream_size.height;
        next->info.fps = fps;
        next->info.bitrate_kbps = bitrate_kbps;
        next->info.monitor_id = chosen != nullptr ? chosen->id : 0;
        next->info.source = source;
        next->info.audio_rate = cfg.audio ? audio_rate : 0;
        next->info.audio_channels = cfg.audio ? audio_channels : 0;
        next->info.notes = notes;
        next->info.intra_refresh = next->encoder->intra_refresh_active();
        next->info.slices = next->encoder->slices_active();
        next->info.encoder_backend = next->encoder->backend();
        next->info.stream_index = stream_index_;
        p = std::move(next);
        {
            std::scoped_lock lock(mutex_);
            current_ = p->info;
            encoder_name_ = p->encoder->name();
        }
        log::info("Stream {}: {} {}x{} @ {} fps, {} Mbps, origen ({},{}) {}x{}", epoch, to_string(codec), stream_size.width,
                  stream_size.height, fps, bitrate_kbps / 1000, source.x, source.y, source.width, source.height);
        {
            std::scoped_lock lock(mutex_);
            window_interval_us_ = 1'000'000 / fps;
            overload_windows_ = 0;
            adapt_hold_until_us_ = gpu::now_us() + 3'000'000;  // El codificador nuevo necesita calentar.
        }
        if (callbacks_.started) callbacks_.started(p->info);
        keyframe_requested_ = true;
        dirty = true;
        dirty_since_us = gpu::now_us();
    };

    while (running_) {
        // ¿Reconfiguración pendiente?
        std::optional<proto::StreamConfig> cfg;
        std::vector<Codec> decoders;
        std::uint32_t audio_rate = 0;
        std::uint8_t audio_channels = 0;
        bool client_slices = false;
        {
            std::unique_lock lock(mutex_);
            if (!p && !pending_) cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return pending_ || !running_; });
            if (pending_) {
                cfg = pending_;
                pending_.reset();
                decoders = client_decoders_;
                client_slices = client_slices_;
                audio_rate = audio_rate_;
                audio_channels = audio_channels_;
            }
        }
        if (!running_) break;
        if (cfg) {
            // Cambios "ligeros" (bitrate, FEC, audio): se aplican sin recrear captura ni codificador.
            if (p && p->config.monitor_id == cfg->monitor_id && p->config.max_width == cfg->max_width &&
                p->config.max_height == cfg->max_height && p->config.fps == cfg->fps && p->config.codec == cfg->codec &&
                p->config.rate_control == cfg->rate_control && p->config.chroma_444 == cfg->chroma_444 &&
                p->config.hdr == cfg->hdr && p->config.low_latency == cfg->low_latency &&
                p->config.intra_refresh == cfg->intra_refresh && p->config.slices == cfg->slices) {
                const std::uint32_t kbps = cfg->auto_bitrate
                                               ? auto_bitrate_mbps(p->stream_size.width, p->stream_size.height,
                                                                   p->info.fps, p->info.codec) * 1000
                                               : cfg->bitrate_kbps;
                p->encoder->set_bitrate(kbps);
                p->ctx->fec = static_cast<int>(cfg->fec_percent);
                p->config = *cfg;
                p->info.bitrate_kbps = kbps;
                p->info.audio_rate = cfg->audio ? audio_rate : 0;
                p->info.audio_channels = cfg->audio ? audio_channels : 0;
                std::scoped_lock lock(mutex_);
                current_ = p->info;
                if (callbacks_.started) callbacks_.started(p->info);
            } else {
                adapt_scale_ = 1.0;  // Nueva configuración del cliente: se vuelve a intentar sin recortes.
                adapt_fps_ = 0;
                adapt_requested_ = false;
                {
                    std::scoped_lock lock(mutex_);
                    adapt_reason_.clear();
                }
                build(*cfg, decoders, audio_rate, audio_channels, client_slices);
            }
        }
        if (!p) continue;

        // Ajuste automático pedido por las estadísticas.
        if (adapt_requested_.exchange(false)) {
            if (!p->config.adaptive) {
                std::scoped_lock lock(mutex_);
                adapt_reason_.clear();
            } else if (const auto step = adapt::next_step(p->stream_size, p->info.fps, adapt_scale_)) {
                adapt_scale_ = step->scale;
                if (step->fps != 0) adapt_fps_ = step->fps;
                std::vector<Codec> decs;
                bool slices_ok = false;
                std::uint32_t arate = 0;
                std::uint8_t achannels = 0;
                {
                    std::scoped_lock lock(mutex_);
                    decs = client_decoders_;
                    slices_ok = client_slices_;
                    arate = audio_rate_;
                    achannels = audio_channels_;
                    log::warn("Ajuste automático: {}", adapt_reason_);
                }
                const proto::StreamConfig same = p->config;
                build(same, decs, arate, achannels, slices_ok);
                if (!p) continue;
            } else {
                std::scoped_lock lock(mutex_);
                log::warn("Ajuste automático: ya en el mínimo (720p, 30 fps); {}", adapt_reason_);
                adapt_reason_.clear();
            }
        }

        // Esperar un frame nuevo, como mucho hasta el próximo instante de envío.
        const std::uint64_t interval_us = 1'000'000 / p->info.fps;
        std::uint64_t now = gpu::now_us();
        std::uint32_t timeout_ms = 16;
        if (dirty) {
            const std::uint64_t due = last_encode_us + interval_us;
            timeout_ms = due > now ? static_cast<std::uint32_t>((due - now) / 1000) : 0;
        }
        const capture::Status st = p->capture->acquire(timeout_ms);
        now = gpu::now_us();
        if (st == capture::Status::NewFrame) {
            if (!dirty) dirty_since_us = now;
            dirty = true;
        } else if (st == capture::Status::Recreated) {
            keyframe_requested_ = true;
            dirty = true;
            dirty_since_us = now;
        }

        // Cursor: posición (hasta ~120 Hz) y forma.
        const capture::PointerState& ptr = p->capture->pointer();
        if ((ptr.visible != last_pointer.visible || ptr.x != last_pointer.x || ptr.y != last_pointer.y) &&
            now - last_pointer_send_us > 8000) {
            last_pointer = ptr;
            last_pointer_send_us = now;
            const Rect& src = p->info.source;
            if (callbacks_.cursor_pos) {
                callbacks_.cursor_pos(proto::CursorPos{
                    ptr.visible && ptr.x >= src.x && ptr.y >= src.y && ptr.x < src.right() && ptr.y < src.bottom(),
                    static_cast<float>(ptr.x - src.x) / static_cast<float>(src.width),
                    static_cast<float>(ptr.y - src.y) / static_cast<float>(src.height), stream_index_});
            }
        }
        if (auto shape = p->capture->take_pointer_shape(); shape && callbacks_.cursor_shape) callbacks_.cursor_shape(*shape);

        // Fotograma de mantenimiento si la pantalla está quieta (ayuda a recuperarse tras pérdidas).
        if (!dirty && now - last_encode_us > 1'000'000) {
            dirty = true;
            dirty_since_us = now;
        }
        if (refresh_requested_.exchange(false)) {
            if (p->encoder->intra_refresh_active()) {
                p->encoder->request_intra_refresh();  // NVENC: ola bajo demanda; AMF: refresco continuo.
            } else {
                keyframe_requested_ = true;
            }
        }
        const bool idr = keyframe_requested_.exchange(false);
        if (!(dirty || idr) || (!idr && now - last_encode_us + 1000 < interval_us)) {
            if (idr) keyframe_requested_ = true;
            continue;
        }
        if (!sender_.endpoint()) {
            // Aún no conocemos el endpoint UDP del cliente.
            if (idr) keyframe_requested_ = true;
            Sleep(5);
            continue;
        }

        // Contrapresión: si el codificador ya tiene cuadros esperando, no se agrega otro a la cola (solo sumaría
        // latencia). El cuadro queda pendiente y se envía la imagen más reciente cuando haya lugar.
        if (!idr) {
            std::size_t queued = 0;
            {
                std::scoped_lock lock(p->ctx->mutex);
                // Entradas viejas = cuadros que el codificador descartó: no deben bloquear.
                while (!p->ctx->in_flight.empty() && now > p->ctx->in_flight.front().encode_start_us + 250'000) {
                    p->ctx->in_flight.pop_front();
                }
                queued = p->ctx->in_flight.size();
            }
            if (queued >= max_queued(*p->encoder)) {
                if (st == capture::Status::NewFrame) {
                    std::scoped_lock lock(mutex_);
                    ++window_skipped_;
                }
                WaitForSingleObject(p->ctx->output_event, 2);
                continue;
            }
        }

        // Momento real en que Windows presentó la imagen que vamos a codificar (si es reciente).
        const std::uint64_t presented = p->capture->last_present_us();
        // (Los frames de mantenimiento o IDR sobre una imagen vieja cuentan desde ahora.)
        const std::uint64_t base = dirty ? dirty_since_us : now;
        const std::uint64_t capture_us = (presented != 0 && presented <= now && presented + 50'000 >= base) ? presented : base;
        const std::uint64_t encode_start_us = gpu::now_us();
        ID3D11Texture2D* target = p->nv12[p->nv12_index].Get();
        p->nv12_index = (p->nv12_index + 1) % p->nv12.size();
        if (!p->vp->process(p->capture->texture(), 0, target, nullptr, nullptr, p->capture->rotation())) {
            fail("Falló la conversión de color en la GPU");
            continue;
        }
        // Entregar al codificador y volver a capturar: el bitstream llega por el callback (en paralelo).
        const std::uint64_t ts = std::max(now, last_ts + 1);
        last_ts = ts;
        {
            std::scoped_lock lock(p->ctx->mutex);
            p->ctx->in_flight.push_back(InFlight{ts, capture_us, encode_start_us});
            if (p->ctx->in_flight.size() > 16) p->ctx->in_flight.pop_front();
        }
        auto submitted = p->encoder->submit(target, ts, idr);
        last_encode_us = now;
        dirty = false;
        if (!submitted) fail(submitted.error().message());
    }
    p.reset();
}

void Streamer::sender_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    sys::RealtimeThread mmcss(L"Games");
    auto cipher = sender_.make_cipher();
    while (running_) {
        SendJob job;
        {
            std::unique_lock lock(send_mutex_);
            send_cv_.wait(lock, [&] { return !send_queue_.empty() || !running_; });
            if (!running_) return;
            job = std::move(send_queue_.front());
            send_queue_.pop_front();
        }
        // Todos los fragmentos de video miden lo mismo: se envían por lotes (UDP Send Offload).
        net::MediaSender::Batch batch(sender_);
        media::packetize_frame(job.data, job.info, job.fec_percent, [&](std::span<const std::uint8_t> payload) {
            batch.add(*cipher, media::PacketType::Video, payload);
        });
    }
}

}  // namespace lancast::host
