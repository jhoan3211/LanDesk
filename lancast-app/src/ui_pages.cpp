// Pantallas de la interfaz (Dear ImGui).

#include <algorithm>
#include <cmath>
#include <format>

#include "app.hpp"
#include "app_internal.hpp"
#include "lancast/core/version.hpp"
#include "ui_common.hpp"
#include "imgui.h"
#include "lancast/core/bitrate.hpp"
#include "lancast/core/encoding.hpp"
#include "lancast/core/geometry.hpp"
#include "lancast/core/log.hpp"
#include "lancast/core/report.hpp"
#include "lancast/platform/d3d.hpp"
#include "lancast/platform/platform.hpp"
#include "lancast/platform/system.hpp"

namespace lancast::app {
using namespace ui;

// ------------------------------------------------------------------------------------------------ conectar

void App::draw_connect_page() {
    ImGui::PushFont(font_big_, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("Conectar a otro equipo");
    ImGui::PopFont();
    ImGui::TextColored(kMuted, "Ve y controla la pantalla de otro PC de tu red local con latencia mínima.");

    // Estado de una conexión en curso.
    if (session_) {
        const client::State st = session_->state();
        ImGui::Spacing();
        ImGui::BeginChild("##status", ImVec2(0, 70 * scale_), ImGuiChildFlags_Borders);
        const char* spinner = "|/-\\";
        const int idx = static_cast<int>(ImGui::GetTime() * 8) % 4;
        ImGui::Text("%c  %s %s", spinner[idx], std::string{client::to_string(st)}.c_str(),
                    session_->host_endpoint().to_string().c_str());
        const std::string msg = session_->message();
        if (!msg.empty()) ImGui::TextColored(kMuted, "%s", msg.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 120 * scale_);
        if (ImGui::Button("Cancelar")) disconnect_client();
        ImGui::EndChild();
    }

    section("Conexión manual por IP");
    ImGui::SetNextItemWidth(320 * scale_);
    bool enter = false;
    input_text("##addr", address_input_, 0, &enter);
    ImGui::SameLine();
    if ((primary_button("Conectar") || enter) && !address_input_.empty()) start_connect(address_input_);
    help("Escribe la IP del otro equipo (por ejemplo 192.168.40.7) o IP:puerto si cambiaste el puerto (por defecto 47984).");
    if (!settings_.client.recent_addresses.empty()) {
        ImGui::TextColored(kMuted, "Recientes:");
        for (const std::string& a : settings_.client.recent_addresses) {
            ImGui::SameLine();
            if (ImGui::SmallButton(a.c_str())) {
                address_input_ = a;
                start_connect(a);
                break;
            }
        }
    }

    section("Equipos encontrados en la red");
    // Descubrimiento automático cada 4 s mientras esta página está visible.
    const std::uint64_t now = gpu::now_us();
    if (!discovery_running_ && now - last_discovery_us_ > 4'000'000) {
        if (discovery_thread_.joinable()) discovery_thread_.join();
        discovery_running_ = true;
        last_discovery_us_ = now;
        discovery_thread_ = std::thread([this] {
            auto found = client::discover_hosts(800);
            std::scoped_lock lock(discovery_mutex_);
            discovered_ = std::move(found);
            discovery_running_ = false;
        });
    }
    std::vector<client::DiscoveredHost> hosts;
    {
        std::scoped_lock lock(discovery_mutex_);
        hosts = discovered_;
    }
    // El propio equipo aparece por loopback: se oculta.
    std::erase_if(hosts, [&](const client::DiscoveredHost& h) { return h.id_hex == settings_.device_id; });
    if (hosts.empty()) {
        ImGui::TextColored(kMuted, discovery_running_ ? "Buscando…" : "No se encontraron equipos con LanCast abierto.");
        ImGui::TextColored(kMuted, "Abre LanCast en el otro PC o usa la conexión manual por IP.");
    }
    for (const auto& h : hosts) {
        ImGui::PushID(h.id_hex.c_str());
        ImGui::BeginChild("##host", ImVec2(0, 64 * scale_), ImGuiChildFlags_Borders);
        ImGui::PushFont(font_big_, 0.0f);
        ImGui::TextUnformatted(h.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (h.paired) badge("emparejado", kGood);
        if (h.busy) {
            ImGui::SameLine();
            badge("ocupado", kWarn);
        }
        ImGui::TextColored(kMuted, "%s", h.endpoint.to_string().c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 130 * scale_);
        ImGui::SetCursorPosY(14 * scale_);
        if (primary_button("Conectar", ImVec2(110 * scale_, 0))) {
            address_input_ = h.endpoint.port == kDefaultControlPort ? h.endpoint.ip_string() : h.endpoint.to_string();
            start_connect(address_input_);
        }
        ImGui::EndChild();
        ImGui::PopID();
    }

    section("Configuración del stream");
    draw_stream_settings(false);
}

void App::draw_stream_settings(bool live) {
    StreamSettings& s = settings_.client.stream;
    bool changed = false;
    bool profile_changed = false;

    // Perfiles
    auto& profiles = settings_.client.profiles;
    ImGui::SetNextItemWidth(260 * scale_);
    const char* current = settings_.client.active_profile.empty() ? "(sin perfil)" : settings_.client.active_profile.c_str();
    if (ImGui::BeginCombo("Perfil", current)) {
        for (const Profile& p : profiles) {
            if (ImGui::Selectable(p.name.c_str(), p.name == settings_.client.active_profile)) {
                const std::uint32_t monitor = s.monitor_id;
                s = p.stream;
                if (live) s.monitor_id = monitor;
                settings_.client.active_profile = p.name;
                profile_changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Guardar como…")) {
        new_profile_name_ = settings_.client.active_profile;
        ImGui::OpenPopup("##saveprofile");
    }
    if (!settings_.client.active_profile.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Borrar perfil")) {
            std::erase_if(profiles, [&](const Profile& p) { return p.name == settings_.client.active_profile; });
            settings_.client.active_profile.clear();
            save_settings();
        }
    }
    if (ImGui::BeginPopup("##saveprofile")) {
        ImGui::TextUnformatted("Nombre del perfil");
        ImGui::SetNextItemWidth(260 * scale_);
        bool enter = false;
        input_text("##pname", new_profile_name_, 0, &enter);
        if ((ImGui::Button("Guardar") || enter) && !new_profile_name_.empty()) {
            std::erase_if(profiles, [&](const Profile& p) { return p.name == new_profile_name_; });
            profiles.push_back(Profile{new_profile_name_, s});
            settings_.client.active_profile = new_profile_name_;
            save_settings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    const float label_w = 170 * scale_;
    auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(std::min(360 * scale_, ImGui::GetContentRegionAvail().x));
    };

    if (ImGui::BeginTable("##streamcfg", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();

        // Monitor
        row("Monitor del host");
        std::string mon_label = s.monitor_id == 0 ? "Todos (escritorio completo)" : std::format("Monitor {}", s.monitor_id);
        const auto info = session_ ? session_->host_info() : std::nullopt;
        if (info) {
            for (const auto& m : info->monitors) {
                if (m.id == s.monitor_id) mon_label = std::format("{}: {} ({}x{} {} Hz)", m.id, m.name, m.rect.width, m.rect.height, m.refresh_hz);
            }
        }
        if (ImGui::BeginCombo("##monitor", mon_label.c_str())) {
            if (info) {
                for (const auto& m : info->monitors) {
                    const std::string label = std::format("{}: {} — {}x{} @ {} Hz, pos ({}, {}){}", m.id, m.name, m.rect.width,
                                                          m.rect.height, m.refresh_hz, m.rect.x, m.rect.y, m.primary ? " (principal)" : "");
                    if (ImGui::Selectable(label.c_str(), s.monitor_id == m.id)) {
                        s.monitor_id = m.id;
                        changed = true;
                    }
                }
            } else {
                for (std::uint32_t i = 1; i <= 4; ++i) {
                    if (ImGui::Selectable(std::format("Monitor {}", i).c_str(), s.monitor_id == i)) {
                        s.monitor_id = i;
                        changed = true;
                    }
                }
            }
            if (ImGui::Selectable("Todos (escritorio virtual completo)", s.monitor_id == 0)) {
                s.monitor_id = 0;
                changed = true;
            }
            ImGui::EndCombo();
        }

        // Resolución
        row("Resolución");
        if (ImGui::BeginCombo("##res", std::string{to_string(s.resolution)}.c_str())) {
            for (int i = 0; i <= static_cast<int>(ResolutionPreset::Custom); ++i) {
                const auto r = static_cast<ResolutionPreset>(i);
                if (ImGui::Selectable(std::string{to_string(r)}.c_str(), s.resolution == r)) {
                    s.resolution = r;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (s.resolution == ResolutionPreset::Custom) {
            int wh[2] = {static_cast<int>(s.custom_width), static_cast<int>(s.custom_height)};
            row("  Ancho × alto");
            if (ImGui::InputInt2("##custom", wh)) {
                s.custom_width = static_cast<std::uint32_t>(std::clamp(wh[0], 320, 7680)) & ~1u;
                s.custom_height = static_cast<std::uint32_t>(std::clamp(wh[1], 240, 4320)) & ~1u;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        }

        // FPS
        row("FPS");
        static const int kFps[] = {30, 60, 90, 120, 144, 165, 240};
        const std::string fps_label =
            s.fps_match_display ? std::format("Igual a esta pantalla ({} Hz)", display_hz()) : std::to_string(s.fps);
        if (ImGui::BeginCombo("##fps", fps_label.c_str())) {
            if (ImGui::Selectable(std::format("Igual a esta pantalla ({} Hz)", display_hz()).c_str(), s.fps_match_display)) {
                s.fps_match_display = true;
                changed = true;
            }
            for (const int f : kFps) {
                if (ImGui::Selectable(std::to_string(f).c_str(), !s.fps_match_display && static_cast<int>(s.fps) == f)) {
                    s.fps = static_cast<std::uint32_t>(f);
                    s.fps_match_display = false;
                    changed = true;
                }
            }
            int custom = static_cast<int>(s.fps);
            ImGui::SetNextItemWidth(120 * scale_);
            if (ImGui::InputInt("Personalizado", &custom, 1, 10)) {
                s.fps = static_cast<std::uint32_t>(std::clamp(custom, 10, 360));
                s.fps_match_display = false;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
            ImGui::EndCombo();
        }
        help("Limitado por la frecuencia del monitor del host.");

        // Códec
        row("Códec");
        if (ImGui::BeginCombo("##codec", std::string{to_string(s.codec)}.c_str())) {
            for (int i = 0; i <= 3; ++i) {
                const auto c = static_cast<CodecChoice>(i);
                if (ImGui::Selectable(std::string{to_string(c)}.c_str(), s.codec == c)) {
                    s.codec = c;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        help("Automático elige el mejor que soporten las dos GPU (HEVC > AV1 > H.264).");

        // Bitrate
        row("Bitrate");
        if (ImGui::Checkbox("Automático##br", &s.auto_bitrate)) changed = true;
        if (s.auto_bitrate) {
            const Size req = requested_size(s, backbuffer_size_.width ? Size{2560, 1440} : Size{1920, 1080});
            const Codec c = s.codec == CodecChoice::H264 ? Codec::H264 : Codec::Hevc;
            ImGui::SameLine();
            ImGui::TextColored(kMuted, "≈ %u Mbps", auto_bitrate_mbps(req.width, req.height, requested_fps(s), c));
        } else {
            int mbps = static_cast<int>(s.bitrate_mbps);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("");
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(std::min(360 * scale_, ImGui::GetContentRegionAvail().x));
            if (ImGui::SliderInt("##mbps", &mbps, 5, 1000, "%d Mbps", ImGuiSliderFlags_Logarithmic)) s.bitrate_mbps = static_cast<std::uint32_t>(mbps);
            if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        }

        ImGui::TableNextColumn();

        row("Control de tasa");
        int rc = static_cast<int>(s.rate_control);
        if (ImGui::RadioButton("CBR", &rc, 0)) changed = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("VBR", &rc, 1)) changed = true;
        s.rate_control = static_cast<RateControl>(rc);

        row("Chroma");
        int chroma = s.chroma_444 ? 1 : 0;
        if (ImGui::RadioButton("4:2:0", &chroma, 0)) changed = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("4:4:4", &chroma, 1)) changed = true;
        s.chroma_444 = chroma == 1;

        row("Color");
        int hdr = s.hdr ? 1 : 0;
        if (ImGui::RadioButton("8 bits SDR", &hdr, 0)) changed = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("10 bits HDR", &hdr, 1)) changed = true;
        s.hdr = hdr == 1;

        row("Escalado");
        int sc = static_cast<int>(s.scaling);
        bool scaling_changed = false;
        scaling_changed |= ImGui::RadioButton("Ajustar", &sc, 0);
        ImGui::SameLine();
        scaling_changed |= ImGui::RadioButton("Estirar", &sc, 1);
        ImGui::SameLine();
        scaling_changed |= ImGui::RadioButton("1:1", &sc, 2);
        s.scaling = static_cast<ScalingMode>(sc);
        if (scaling_changed) save_settings();  // Es local: no hace falta renegociar.

        row("V-Sync");
        if (ImGui::Checkbox("##vsync", &s.vsync)) save_settings();
        help("Desactivado = menor latencia (permite tearing).");

        row("Codificador");
        if (ImGui::Checkbox("Latencia mínima##ll", &s.low_latency)) changed = true;
        help("Preset más rápido del codificador y búfer de 1 frame. Desactívalo para más calidad con poco bitrate.");
        row("Pérdidas");
        if (ImGui::Checkbox("Intra-refresh (experimental)##ir", &s.intra_refresh)) changed = true;
        help("Ante un paquete perdido la imagen se repara en pocos cuadros en vez de pedir un keyframe completo. "
             "Con los decodificadores de Windows suele recuperar más lento que un keyframe: déjalo apagado salvo "
             "que veas picos al recuperarse.");
        row("Envío");
        if (ImGui::Checkbox("Por slices (sub-frame)##sl", &s.slices)) changed = true;
        help("El cuadro se envía en partes a medida que el codificador las termina: la red trabaja en paralelo "
             "con la codificación. Solo si la GPU del host lo soporta.");
        row("Ajuste automático");
        if (ImGui::Checkbox("##adaptive", &s.adaptive)) changed = true;
        help("Si el codificador del host tarda más que el tiempo entre cuadros, baja la resolución (hasta 720p) "
             "y luego los FPS, para que la latencia no crezca. Se avisa con un mensaje.");
        if (live) {
            row("");
            if (primary_button("Aplicar latencia mínima")) apply_min_latency();
        }

        row("Audio");
        if (ImGui::Checkbox("Transmitir##audio", &s.audio)) changed = true;
        ImGui::BeginDisabled(!s.audio);
        row("  Frecuencia");
        if (ImGui::BeginCombo("##arate", audio_rate_label(s.audio_sample_rate).c_str())) {
            for (const std::uint32_t r : kAudioRates) {
                if (ImGui::Selectable(audio_rate_label(r).c_str(), s.audio_sample_rate == r)) {
                    s.audio_sample_rate = r;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        row("  Canales");
        int ch = s.audio_stereo ? 0 : 1;
        if (ImGui::RadioButton("Estéreo", &ch, 0)) changed = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Mono", &ch, 1)) changed = true;
        s.audio_stereo = ch == 0;
        row("  Volumen");
        int vol = static_cast<int>(s.audio_volume);
        if (ImGui::SliderInt("##vol", &vol, 0, 100, "%d %%")) s.audio_volume = static_cast<std::uint32_t>(vol);
        if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();  // Local: no hace falta renegociar.
        row("  Búfer");
        int buf = static_cast<int>(s.audio_buffer_ms);
        if (ImGui::SliderInt("##abuf", &buf, 5, 200, "%d ms")) s.audio_buffer_ms = static_cast<std::uint32_t>(buf);
        if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
        help("Menos búfer = menos latencia de audio. Súbelo si escuchas cortes.");
        ImGui::EndDisabled();

        row("FEC");
        int fec = static_cast<int>(s.fec_percent);
        if (ImGui::SliderInt("##fec", &fec, 0, 50, "%d %%")) s.fec_percent = static_cast<std::uint32_t>(fec);
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        help("Paquetes de paridad Reed-Solomon: recuperan pérdidas sin esperar retransmisiones. 10–20% es lo recomendado.");
        ImGui::EndTable();
    }

    if (changed || profile_changed) {
        if (changed && !profile_changed) {
            // Modificar a mano deja de coincidir con el perfil.
            const auto it = std::ranges::find(profiles, settings_.client.active_profile, &Profile::name);
            if (it != profiles.end() && !(it->stream == s)) settings_.client.active_profile.clear();
        }
        apply_stream_settings();
    }
}

// ------------------------------------------------------------------------------------------------ host

void App::draw_host_page() {
    const host::Status st = host_.status();
    ImGui::PushFont(font_big_, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("Este equipo como host");
    ImGui::PopFont();
    ImGui::TextColored(kMuted, "Permite que otros equipos de tu red vean y controlen esta pantalla.");

    if (service_host_) {
        draw_service_section();
        return;
    }

    bool enabled = settings_.host.enabled;
    if (ImGui::Checkbox("Permitir conexiones a este equipo", &enabled)) {
        settings_.host.enabled = enabled;
        sys::save_settings(settings_);
        host_.apply_settings(effective_host_settings());
    }

    if (st.pin) {
        ImGui::Spacing();
        ImGui::BeginChild("##pin", ImVec2(0, 150 * scale_), ImGuiChildFlags_Borders);
        ImGui::TextColored(kWarn, "Solicitud de emparejamiento de %s", st.pin_requester.c_str());
        ImGui::TextUnformatted("Escribe este PIN en el otro equipo:");
        ImGui::PushFont(font_big_, ImGui::GetStyle().FontSizeBase * 3.0f);
        const std::string spaced = st.pin->substr(0, 3) + " " + st.pin->substr(3);
        ImGui::TextColored(kAccent, "%s", spaced.c_str());
        ImGui::PopFont();
        if (ImGui::Button("Rechazar")) host_.cancel_pairing();
        ImGui::EndChild();
    }

    section("Estado");
    if (!st.error.empty()) {
        ImGui::TextColored(kBad, "%s", st.error.c_str());
        ImGui::TextWrapped("Otra aplicación (¿otra copia de LanCast?) puede estar usando los puertos. Cámbialos en Ajustes.");
    } else if (st.listening) {
        ImGui::TextColored(kGood, "Esperando conexiones en:");
        for (const auto& a : st.addresses) {
            ImGui::BulletText("%s  (puerto %u)", a.c_str(), settings_.host.control_port);
        }
    } else {
        ImGui::TextColored(kMuted, "El host está desactivado.");
    }

    if (st.session_active) {
        section("Sesión activa");
        ImGui::BeginChild("##session", ImVec2(0, 170 * scale_), ImGuiChildFlags_Borders);
        ImGui::TextColored(kWarn, "%s (%s) está conectado", st.client_name.c_str(), st.client_address.c_str());
        const double mins = (gpu::now_us() - st.session_started_us) / 60e6;
        ImGui::TextColored(kMuted, "Desde hace %.0f min", mins);
        if (st.stream) {
            ImGui::Text("%s %ux%u @ %u fps · %u Mbps · %s", std::string{to_string(st.stream->codec)}.c_str(), st.stream->width,
                        st.stream->height, st.stream->fps, st.stream->bitrate_kbps / 1000, st.encoder_name.c_str());
            ImGui::Text("Enviando %.0f fps · %.1f Mbps · captura+codificación %.1f ms", st.stats.fps, st.stats.bitrate_mbps,
                        st.stats.capture_to_encoded_ms);
            if (st.stats.skipped_percent >= 1.0) {
                ImGui::TextColored(kWarn, "El codificador no da abasto: %.0f %% de cuadros saltados", st.stats.skipped_percent);
            }
        }
        if (danger_button("Desconectar")) host_.disconnect();
        ImGui::SameLine();
        bool view_only = st.view_only;
        if (ImGui::Checkbox("Solo ver (bloquear el control)", &view_only)) host_.set_view_only(view_only);
        ImGui::SameLine();
        if (ImGui::Button("Enviar archivos al cliente…")) {
            const auto paths = sys::pick_files(hwnd_);
            if (!paths.empty()) {
                host_.files().send(paths);
                page_ = Page::Files;
            }
        }
        ImGui::EndChild();
    }

    section("Permisos");
    bool changed = false;
    changed |= ImGui::Checkbox("Permitir control de mouse y teclado", &settings_.host.allow_input);
    changed |= ImGui::Checkbox("Sincronizar portapapeles de texto", &settings_.host.allow_clipboard);
    changed |= ImGui::Checkbox("Permitir transferencia de archivos", &settings_.host.allow_files);
    changed |= ImGui::Checkbox("Transmitir el audio del equipo", &settings_.host.allow_audio);
    if (changed) save_settings();

    section("Equipos emparejados");
    const auto paired = host_.paired_clients();
    if (paired.empty()) ImGui::TextColored(kMuted, "Ninguno. La primera conexión de cada equipo pide un PIN.");
    for (const auto& d : paired) {
        ImGui::PushID(d.id_hex.c_str());
        ImGui::BulletText("%s", d.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "%s", d.last_address.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Olvidar")) host_.revoke(d.id_hex);
        ImGui::PopID();
    }

    draw_service_section();
}

// ------------------------------------------------------------------------------------------------ archivos

void App::draw_transfers(files::Manager& manager, const char* id) {
    const auto items = manager.items();
    if (items.empty()) {
        ImGui::TextColored(kMuted, "Sin transferencias.");
        return;
    }
    if (ImGui::BeginTable(id, 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Archivo", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Progreso", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Velocidad", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Estado", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80 * scale_);
        ImGui::TableHeadersRow();
        for (const auto& it : items) {
            ImGui::PushID(static_cast<int>(it.uid));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s %s", it.outgoing ? "↑" : "↓", it.name.c_str());
            ImGui::TableNextColumn();
            const float frac = it.size == 0 ? 1.0f : static_cast<float>(static_cast<double>(it.done) / static_cast<double>(it.size));
            ImGui::ProgressBar(frac, ImVec2(-1, 0), std::format("{} / {}", human_bytes(it.done), human_bytes(it.size)).c_str());
            ImGui::TableNextColumn();
            if (it.state == files::State::Active && it.speed_bps > 0) {
                const double eta = static_cast<double>(it.size - it.done) / it.speed_bps;
                ImGui::Text("%s/s", human_bytes(static_cast<std::uint64_t>(it.speed_bps)).c_str());
                ImGui::TextColored(kMuted, "quedan %s", human_duration(eta).c_str());
            } else {
                ImGui::TextColored(kMuted, "--");
            }
            ImGui::TableNextColumn();
            const ImVec4 color = it.state == files::State::Done     ? kGood
                                 : it.state == files::State::Failed ? kBad
                                 : it.state == files::State::Cancelled ? kMuted
                                                                       : kAccent;
            ImGui::TextColored(color, "%s", std::string{files::to_string(it.state)}.c_str());
            if (!it.message.empty()) {
                ImGui::SameLine();
                help(it.message.c_str());
            }
            ImGui::TableNextColumn();
            if (it.state == files::State::Queued || it.state == files::State::Active || it.state == files::State::Waiting) {
                if (ImGui::SmallButton("Cancelar")) manager.cancel(it.uid);
            } else if (it.state == files::State::Done && !it.outgoing) {
                if (ImGui::SmallButton("Abrir")) sys::open_in_explorer(it.local_path.parent_path());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void App::draw_files_page() {
    ImGui::PushFont(font_big_, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("Transferencia de archivos");
    ImGui::PopFont();
    ImGui::TextColored(kMuted, "Arrastra archivos o carpetas a la ventana de LanCast para enviarlos. Se reanudan si se corta la conexión.");

    const bool client_files = session_ && session_->files().connected();
    const bool host_files = host_.status().session_active && host_.files().connected();
    section("Enviar");
    ImGui::BeginDisabled(!client_files);
    if (primary_button(client_files ? std::format("Enviar archivos a {}…", session_->host_name()).c_str() : "Enviar archivos al host…")) {
        const auto paths = sys::pick_files(hwnd_);
        if (!paths.empty()) session_->files().send(paths);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!host_files);
    if (ImGui::Button("Enviar archivos al cliente conectado…")) {
        const auto paths = sys::pick_files(hwnd_);
        if (!paths.empty()) host_.files().send(paths);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Abrir carpeta de recibidos")) {
        sys::open_in_explorer(settings_.host.receive_folder.empty()
                                  ? sys::default_receive_dir()
                                  : std::filesystem::path{std::u8string{settings_.host.receive_folder.begin(),
                                                                        settings_.host.receive_folder.end()}});
    }
    if (!client_files && !host_files) ImGui::TextColored(kMuted, "Conéctate a otro equipo (o espera que alguien se conecte) para transferir.");

    section("Como cliente (con el equipo al que me conecté)");
    if (session_) {
        draw_transfers(session_->files(), "##cft");
        if (ImGui::SmallButton("Limpiar terminadas##c")) session_->files().clear_finished();
    } else {
        ImGui::TextColored(kMuted, "Sin conexión.");
    }
    section("Como host (con quien se conectó a este equipo)");
    draw_transfers(host_.files(), "##hft");
    if (ImGui::SmallButton("Limpiar terminadas##h")) host_.files().clear_finished();
}

// ------------------------------------------------------------------------------------------------ ajustes

void App::draw_settings_page() {
    ImGui::PushFont(font_big_, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("Ajustes");
    ImGui::PopFont();

    bool changed = false;
    section("General");
    ImGui::SetNextItemWidth(320 * scale_);
    changed |= input_text("Nombre de este equipo", settings_.device_name);
    if (ImGui::Checkbox("Iniciar con Windows (minimizado en la bandeja)", &settings_.start_with_windows)) {
        if (!sys::set_autostart(settings_.start_with_windows)) toast("No se pudo cambiar el inicio automático");
        changed = true;
    }
    changed |= ImGui::Checkbox("Al cerrar la ventana, seguir en la bandeja (para recibir conexiones)", &settings_.minimize_to_tray);

    section("Actualizaciones");
    changed |= ImGui::Checkbox("Buscar versiones nuevas al iniciar", &settings_.check_updates);
    ImGui::SameLine();
    if (ImGui::Button("Buscar ahora")) check_updates(true);
    {
        std::scoped_lock lock(update_mutex_);
        switch (update_state_) {
            case UpdateState::Checking:
                ImGui::TextColored(kMuted, "Buscando…");
                break;
            case UpdateState::UpToDate:
                ImGui::TextColored(kGood, "Tienes la última versión (%s)", std::string{kAppVersion}.c_str());
                break;
            case UpdateState::Available:
            case UpdateState::Downloading:
                ImGui::TextColored(kAccent, "Disponible: %s (mira el aviso de arriba)", update_info_ ? update_info_->tag.c_str() : "");
                break;
            case UpdateState::Failed:
                ImGui::TextColored(kWarn, "%s", update_message_.c_str());
                break;
            default:
                ImGui::TextColored(kMuted, "Versión actual: %s", std::string{kAppVersion}.c_str());
                break;
        }
    }

    section("Carpetas de recepción");
    auto folder_row = [&](const char* label, std::string& value) {
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(420 * scale_);
        std::string shown = value.empty() ? "(Descargas\\LanCast)" : value;
        ImGui::BeginDisabled();
        input_text("##f", shown);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Elegir…")) {
            if (auto p = sys::pick_folder(hwnd_)) {
                const auto u8 = p->u8string();
                value.assign(u8.begin(), u8.end());
                changed = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Predeterminada")) {
            value.clear();
            changed = true;
        }
        ImGui::PopID();
    };
    folder_row("Archivos recibidos como host", settings_.host.receive_folder);
    folder_row("Archivos recibidos como cliente", settings_.client.receive_folder);
    changed |= ImGui::Checkbox("Sobrescribir archivos existentes (si no, se guardan como \"nombre (1)\")",
                               &settings_.host.overwrite_existing);

    section("Cliente");
    changed |= ImGui::Checkbox("Modo solo ver (no enviar mouse ni teclado)", &settings_.client.view_only);
    changed |= ImGui::Checkbox("Barra del stream oculta (arriba solo aparece una flecha pequeña)",
                               &settings_.client.toolbar_collapsed);
    int tab = static_cast<int>(settings_.client.toolbar_tab_percent);
    ImGui::SetNextItemWidth(220 * scale_);
    if (ImGui::SliderInt("Tamaño de la ✕ y la flecha de la barra", &tab, 50, 300, "%d %%")) {
        settings_.client.toolbar_tab_percent = static_cast<std::uint32_t>(tab);
        changed = true;
    }
    changed |= ImGui::Checkbox("Sincronizar portapapeles de texto", &settings_.client.clipboard_sync);
    int loss = static_cast<int>(settings_.client.simulate_loss_percent);
    ImGui::SetNextItemWidth(220 * scale_);
    if (ImGui::SliderInt("Simular pérdida de paquetes (pruebas)", &loss, 0, 20, "%d %%")) {
        settings_.client.simulate_loss_percent = static_cast<std::uint32_t>(loss);
        changed = true;
    }

    section("Red y seguridad (host)");
    int ports[2] = {settings_.host.control_port, settings_.host.media_port};
    ImGui::SetNextItemWidth(260 * scale_);
    if (ImGui::InputInt2("Puertos: control TCP / video UDP", ports)) {
        settings_.host.control_port = static_cast<std::uint16_t>(std::clamp(ports[0], 1024, 65534));
        settings_.host.media_port = static_cast<std::uint16_t>(std::clamp(ports[1], 1024, 65535));
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    help("El canal de archivos usa el puerto de control + 1.");
    changed |= ImGui::Checkbox("Solo aceptar conexiones de redes privadas (192.168.x.x, 10.x.x.x, 172.16-31.x.x)",
                               &settings_.host.private_networks_only);
    std::string whitelist;
    for (const auto& ip : settings_.host.ip_whitelist) whitelist += (whitelist.empty() ? "" : ", ") + ip;
    ImGui::SetNextItemWidth(420 * scale_);
    if (input_text("IPs permitidas (vacío = todas)", whitelist)) {
        settings_.host.ip_whitelist.clear();
        std::size_t start = 0;
        while (start < whitelist.size()) {
            const std::size_t end = whitelist.find_first_of(", ;", start);
            std::string ip = whitelist.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!ip.empty()) settings_.host.ip_whitelist.push_back(ip);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        changed = true;
    }
    if (ImGui::Button("Crear reglas del firewall (perfil Privado)")) {
        toast(sys::add_firewall_rules() ? "Reglas del firewall creadas" : "No se crearon las reglas (¿cancelaste el aviso de Windows?)");
    }
    help("Permite que otros equipos de la red privada se conecten a LanCast. Windows pedirá permiso de administrador.");

    if (changed) save_settings();

    section("Registro");
    ImGui::BeginChild("##log", ImVec2(0, 200 * scale_), ImGuiChildFlags_Borders);
    for (const auto& e : log::recent(200)) {
        const ImVec4 color = e.level == log::Level::Error ? kBad : e.level == log::Level::Warn ? kWarn : kMuted;
        ImGui::TextColored(color, "[%s] %s", std::string{log::level_name(e.level)}.c_str(), e.message.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    if (ImGui::SmallButton("Abrir carpeta de configuración")) sys::open_in_explorer(sys::app_data_dir());
}

void App::draw_diagnostics_page() {
    ImGui::PushFont(font_big_, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted("Diagnóstico del equipo");
    ImGui::PopFont();
    if (ImGui::Button("Actualizar")) report_ = platform::collect_system_report();
    ImGui::SameLine();
    if (ImGui::Button("Copiar al portapapeles") && report_) {
        ImGui::SetClipboardText(format_report_text(*report_, true).c_str());
        toast("Copiado");
    }
    if (!report_) return;
    ImGui::BeginChild("##report", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextUnformatted(format_report_text(*report_, false).c_str());
    ImGui::EndChild();
}

}  // namespace lancast::app
