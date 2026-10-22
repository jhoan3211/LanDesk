// Estadísticas en vivo en archivos, para leerlas desde fuera de la app (soporte, scripts, Claude):
//   %APPDATA%\LanCast\estadisticas.json  se reescribe cada segundo (cada 5 s sin sesión) con el estado completo
//                                         del cliente y del host, y los últimos 30 s en ventanas de 250 ms.
//   %APPDATA%\LanCast\estadisticas.csv   una línea por segundo mientras hay stream (para ver la evolución).
// Crear el archivo vacío %APPDATA%\LanCast\medir-latencia pide una medición de latencia real (como el botón).

#include <windows.h>

#include <format>
#include <fstream>

#include "app.hpp"
#include "app_internal.hpp"
#include "lancast/core/encoding.hpp"
#include "lancast/core/json_writer.hpp"
#include "lancast/core/log.hpp"
#include "lancast/core/version.hpp"
#include "lancast/platform/system.hpp"

namespace lancast::app {

namespace {

std::string local_time_string() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                       t.wMilliseconds);
}

double round2(double v) { return static_cast<double>(static_cast<std::int64_t>(v * 100.0 + (v >= 0 ? 0.5 : -0.5))) / 100.0; }

void host_stats_json(JsonWriter& j, const host::Status& hs) {
    j.field("escuchando", hs.listening);
    if (!hs.error.empty()) j.field("error", hs.error);
    j.field("sesion_activa", hs.session_active);
    if (hs.session_active) {
        j.field("cliente", hs.client_name).field("ip_cliente", hs.client_address);
        j.field("codificador", hs.encoder_name);
        if (hs.stream) {
            j.field("codec", std::string{to_string(hs.stream->codec)});
            j.field("ancho", hs.stream->width).field("alto", hs.stream->height).field("fps", hs.stream->fps);
            j.field("bitrate_kbps", hs.stream->bitrate_kbps);
        }
        j.field("fps_enviados", round2(hs.stats.fps));
        j.field("mbps_enviados", round2(hs.stats.bitrate_mbps));
        j.field("captura_a_codificado_ms", round2(hs.stats.capture_to_encoded_ms));
        j.field("espera_captura_ms", round2(hs.stats.capture_wait_ms));
        j.field("conversion_y_codificacion_ms", round2(hs.stats.encode_ms));
        j.field("cuadros_saltados_pct", round2(hs.stats.skipped_percent));
        j.key("espectadores").begin_array();
        for (const auto& v : hs.viewers) j.value(v);
        j.end_array();
    }
    if (hs.encode_420_ms > 0) {
        j.field("calibracion", std::format("{}x{}: {:.2f} ms (4:4:4 {:.2f} ms)", hs.calibrated_size.width, hs.calibrated_size.height,
                                           hs.encode_420_ms, hs.encode_444_ms));
    }
}

// Escritura atómica: quien lea nunca ve un archivo a medias.
void write_atomic(const std::filesystem::path& path, const std::string& text) {
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) return;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace

void App::write_live_stats() {
    const std::uint64_t now = gpu::now_us();
    const bool active = session_ && streaming();
    if (now - live_stats_us_ < (active ? 1'000'000u : 5'000'000u)) return;
    live_stats_us_ = now;
    const auto dir = sys::app_data_dir();

    // Pedido de medición de latencia desde fuera de la app.
    std::error_code ec;
    const auto request = dir / L"medir-latencia";
    if (std::filesystem::exists(request, ec)) {
        std::filesystem::remove(request, ec);
        if (active) {
            session_->start_latency_test();
            log::info("Medición de latencia pedida por archivo");
        }
    }

    JsonWriter j;
    j.begin_object();
    j.field("hora", local_time_string());
    j.field("version", std::string{kAppVersion});
    j.field("equipo", settings_.device_name);

    // ---- Este equipo como cliente
    j.key("cliente").begin_object();
    if (!session_) {
        j.field("estado", "sin sesion");
    } else {
        j.field("estado", std::string{client::to_string(session_->state())});
        j.field("host", session_->host_name());
        j.field("mensaje", session_->message());
        if (const auto hi = session_->host_info()) {
            j.field("espectador", hi->spectator);
            j.field("control_permitido", hi->input_allowed);
        }
        if (const auto st = session_->stream()) {
            j.key("stream").begin_object();
            j.field("codec", std::string{to_string(st->codec)});
            j.field("ancho", st->width).field("alto", st->height);
            j.field("fps", st->fps);
            j.field("bitrate_kbps", st->bitrate_kbps);
            j.field("chroma_444", st->chroma_444).field("hdr", st->hdr);
            j.field("codificador_host", st->encoder_backend);
            j.field("decodificador", renderer_ ? renderer_->decoder_name() : std::string{});
            j.end_object();
        }
        const client::Stats s = session_->stats();
        j.key("ultimo_segundo").begin_object();
        j.field("fps", round2(s.fps));
        j.field("mbps", round2(s.bitrate_mbps));
        j.field("host_captura_codificacion_ms", round2(s.encode_ms));
        j.field("red_ms", round2(s.network_ms));
        j.field("decodificacion_ms", round2(s.decode_ms));
        j.field("presentacion_ms", round2(s.present_ms));
        j.field("total_ms", round2(s.total_ms));
        j.field("rtt_ms", round2(s.rtt_ms));
        j.field("perdida_paquetes_pct", round2(s.packet_loss_pct));
        j.field("fec_pct", session_->current_fec());
        j.field("cuadros_reparados", s.frames_recovered);
        j.field("cuadros_perdidos", s.frames_lost);
        j.field("keyframes_pedidos", s.keyframe_requests);
        j.field("audio_bufer_ms", round2(s.audio_buffer_ms));
        j.end_object();
        j.key("latencia_real").begin_object();
        j.field("midiendo", s.latency_test_running);
        j.field("muestras", s.latency_samples);
        j.field("promedio_ms", round2(s.real_latency_ms));
        j.field("min_ms", round2(s.real_latency_min_ms));
        j.field("max_ms", round2(s.real_latency_max_ms));
        j.end_object();
        j.field("unidad_video_despierta", keep_awake_ ? keep_awake_->active() : false);
        j.field("microfono_enviando", session_->microphone_active());
        // Últimos 30 s en ventanas de 250 ms.
        j.key("historial_250ms").begin_array();
        const auto hist = session_->net_history();
        const std::size_t from = hist.size() > 120 ? hist.size() - 120 : 0;
        for (std::size_t i = from; i < hist.size(); ++i) {
            const auto& h = hist[i];
            j.begin_object();
            j.field("hace_ms", static_cast<std::int64_t>((now - h.time_us) / 1000));
            j.field("mbps", round2(h.mbps));
            j.field("perdida_pct", round2(h.loss_pct));
            j.field("reparados", h.fec_recovered);
            j.field("perdidos", h.frames_lost);
            j.field("rtt_ms", round2(h.rtt_ms));
            j.end_object();
        }
        j.end_array();

        // CSV: una línea por segundo mientras hay stream.
        if (active) {
            const auto csv = dir / L"estadisticas.csv";
            if (std::filesystem::exists(csv, ec) && std::filesystem::file_size(csv, ec) > 20u * 1024 * 1024) {
                std::filesystem::rename(csv, dir / L"estadisticas-anterior.csv", ec);
            }
            const bool fresh = !std::filesystem::exists(csv, ec);
            std::ofstream out(csv, std::ios::app | std::ios::binary);
            if (fresh) {
                out << "hora,fps,mbps,host_ms,red_ms,decodificacion_ms,presentacion_ms,total_ms,rtt_ms,perdida_pct,fec_pct,"
                       "reparados,perdidos,latencia_real_ms\r\n";
            }
            out << std::format("{},{:.1f},{:.1f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{},{},{},{:.1f}\r\n",
                               local_time_string(), s.fps, s.bitrate_mbps, s.encode_ms, s.network_ms, s.decode_ms, s.present_ms,
                               s.total_ms, s.rtt_ms, s.packet_loss_pct, session_->current_fec(), s.frames_recovered,
                               s.frames_lost, s.real_latency_ms);
        }
    }
    j.end_object();

    // ---- Este equipo como host (con el servicio instalado, lo escribe el servicio: ver service_stats_dir()).
    j.key("host").begin_object();
    if (service_host_) {
        j.field("en_servicio", true);
        j.field("ver", detail::narrow((service_stats_dir() / L"estadisticas-host.json").wstring()));
    } else {
        host_stats_json(j, host_.status());
    }
    j.end_object();
    j.end_object();

    write_atomic(dir / L"estadisticas.json", j.str());
}

// Estado del host en el servicio (agente): su carpeta de datos está protegida, así que escribe en una aparte que
// cualquier usuario del equipo puede leer (solo estadísticas, nada secreto).
std::filesystem::path service_stats_dir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    return std::filesystem::path{n > 0 ? buf : L"C:\\ProgramData"} / L"LanCast-estadisticas";
}

void write_service_host_stats(const host::Status& hs) {
    static std::uint64_t last = 0;
    const std::uint64_t now = gpu::now_us();
    if (now - last < (hs.session_active ? 1'000'000u : 5'000'000u)) return;
    last = now;
    const auto dir = service_stats_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    JsonWriter j;
    j.begin_object();
    j.field("hora", local_time_string());
    j.field("version", std::string{kAppVersion});
    j.key("host").begin_object();
    host_stats_json(j, hs);
    j.end_object();
    j.end_object();
    write_atomic(dir / L"estadisticas-host.json", j.str());
}

}  // namespace lancast::app
