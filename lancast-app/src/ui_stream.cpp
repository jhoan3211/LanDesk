// Pantallas de la interfaz (Dear ImGui).

#include <algorithm>
#include <cmath>
#include <format>

#include "app.hpp"
#include "app_internal.hpp"
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

// ------------------------------------------------------------------------------------------------ durante el stream

App::TabResult App::draw_toolbar_tab(bool close_icon, float y) {
    const float k = scale_ * static_cast<float>(settings_.client.toolbar_tab_percent) / 100.0f;
    const ImVec2 size(46.0f * k, 22.0f * k);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - size.x) * 0.5f, vp->Pos.y + y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    ImGui::Begin(close_icon ? "##tab_close" : "##tab_show", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav);
    TabResult r;
    r.clicked = ImGui::InvisibleButton("##tab", size);
    r.hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    const ImU32 bg = r.hovered ? (close_icon ? IM_COL32(205, 60, 60, 245) : IM_COL32(55, 115, 215, 245)) : IM_COL32(28, 30, 36, 235);
    dl->AddRectFilled(p0, p1, bg, size.y * 0.45f, ImDrawFlags_RoundCornersBottom);
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    const float h = size.y * 0.24f;
    const float thick = std::max(1.5f, size.y * 0.09f);
    const ImU32 fg = IM_COL32(236, 236, 242, 255);
    if (close_icon) {
        dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), fg, thick);
        dl->AddLine(ImVec2(c.x - h, c.y + h), ImVec2(c.x + h, c.y - h), fg, thick);
    } else {
        // Flecha hacia abajo: "aquí está la barra".
        const ImVec2 tip(c.x, c.y + h * 0.7f);
        dl->AddLine(ImVec2(c.x - h * 1.4f, c.y - h * 0.6f), tip, fg, thick);
        dl->AddLine(ImVec2(c.x + h * 1.4f, c.y - h * 0.6f), tip, fg, thick);
    }
    ImGui::SetItemTooltip(close_icon ? "Ocultar la barra: ya no aparecerá al pasar el mouse (queda solo este ícono)"
                                     : "Mostrar la barra");
    ImGui::End();
    ImGui::PopStyleVar(3);
    return r;
}

void App::draw_stream_toolbar() {
    // Aparece al llevar el mouse al borde superior de la ventana (como en otros escritorios remotos).
    const std::uint64_t now = gpu::now_us();
    const ImGuiIO& io = ImGui::GetIO();
    const bool at_top = ImGui::IsMousePosValid() && io.MousePos.y >= 0 && io.MousePos.y < 6.0f * scale_;
    if (settings_.client.toolbar_collapsed) {
        // Barra oculta con la ✕: arriba solo aparece un ícono pequeño en el centro; al pulsarlo vuelve la barra.
        if (show_menu_) return;
        if (at_top) toolbar_tab_until_us_ = now + 1'500'000;
        if (now > toolbar_tab_until_us_) return;
        const TabResult tab = draw_toolbar_tab(false, 0.0f);
        if (tab.hovered) toolbar_tab_until_us_ = now + 1'500'000;
        if (tab.clicked) {
            settings_.client.toolbar_collapsed = false;
            save_settings();
            toolbar_until_us_ = now + 2'500'000;
            toolbar_tab_until_us_ = 0;
        }
        return;
    }
    if (at_top) toolbar_until_us_ = now + 1'500'000;
    if (now > toolbar_until_us_ && !show_menu_) return;
    if (show_menu_) return;  // El menú completo ya está abierto.

    StreamSettings& s = settings_.client.stream;
    const auto stream = session_->stream();
    const auto info = session_->host_info();
    bool changed = false;

    // La barra se acomoda al ancho de la ventana: en una fila si cabe y, si no (p. ej. a 1280x720 o en una ventana
    // angosta), en las filas que hagan falta, sin cortar ni esconder ningún control.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float max_w = std::max(240.0f * scale_, ImGui::GetMainViewport()->Size.x - 2 * style.WindowPadding.x - 24.0f * scale_);
    float row_w = -1.0f;
    auto place = [&](float w) {
        if (row_w >= 0 && row_w + style.ItemSpacing.x + w <= max_w) {
            ImGui::SameLine();
            row_w += style.ItemSpacing.x + w;
        } else {
            row_w = w;  // Primer control o fila nueva.
        }
    };
    auto button_w = [&](const std::string& label) { return ImGui::CalcTextSize(label.c_str(), nullptr, true).x + style.FramePadding.x * 2; };

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Size.x * 0.5f, 0), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(max_w + 2 * style.WindowPadding.x, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##toolbar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove);
    place(ImGui::CalcTextSize(session_->host_name().c_str()).x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kAccent, "%s", session_->host_name().c_str());
    if (stream) {
        ImGui::SetItemTooltip("%s %ux%u · %u fps · %u Mbps · audio %s", std::string{to_string(stream->codec)}.c_str(),
                              stream->width, stream->height, stream->fps, stream->bitrate_kbps / 1000,
                              stream->audio_rate ? std::format("{} Hz {}", stream->audio_rate,
                                                               stream->audio_channels == 1 ? "mono" : "estéreo").c_str()
                                                 : "apagado");
    }

    // Resolución
    place(200 * scale_);
    const Size here = display_size();
    auto res_label = [&](ResolutionPreset r) {
        return r == ResolutionPreset::ClientNative ? std::format("Igual a esta pantalla ({}x{})", here.width, here.height)
                                                   : std::string{to_string(r)};
    };
    ImGui::SetNextItemWidth(200 * scale_);
    if (ImGui::BeginCombo("##tres", res_label(s.resolution).c_str())) {
        for (int i = 0; i < static_cast<int>(ResolutionPreset::Custom); ++i) {
            const auto r = static_cast<ResolutionPreset>(i);
            if (ImGui::Selectable(res_label(r).c_str(), s.resolution == r)) {
                s.resolution = r;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Resolución");

    // FPS
    place(110 * scale_);
    const std::uint32_t here_hz = display_hz();
    ImGui::SetNextItemWidth(110 * scale_);
    const std::string fps_label = s.fps_match_display ? std::format("{} fps (pantalla)", std::max(here_hz, 30u)) : std::format("{} fps", s.fps);
    if (ImGui::BeginCombo("##tfps", fps_label.c_str())) {
        if (ImGui::Selectable(std::format("Igual a esta pantalla ({} Hz)", here_hz).c_str(), s.fps_match_display)) {
            s.fps_match_display = true;
            changed = true;
        }
        for (const std::uint32_t f : {30u, 60u, 90u, 120u, 144u, 165u, 240u}) {
            if (ImGui::Selectable(std::format("{} fps", f).c_str(), !s.fps_match_display && s.fps == f)) {
                s.fps = f;
                s.fps_match_display = false;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Más FPS que los Hz de esta pantalla no se ven, pero cargan al codificador del host");

    // Monitor
    place(170 * scale_);
    ImGui::SetNextItemWidth(170 * scale_);
    const std::string mon = s.monitor_id == 0 ? std::string{"Todos los monitores"} : std::format("Monitor {}", s.monitor_id);
    if (ImGui::BeginCombo("##tmon", mon.c_str())) {
        if (info) {
            for (const auto& m : info->monitors) {
                if (ImGui::Selectable(std::format("{}: {} ({}x{})", m.id, m.name, m.rect.width, m.rect.height).c_str(),
                                      s.monitor_id == m.id)) {
                    s.monitor_id = m.id;
                    changed = true;
                }
            }
        }
        if (ImGui::Selectable("Todos los monitores", s.monitor_id == 0)) {
            s.monitor_id = 0;
            changed = true;
        }
        ImGui::EndCombo();
    }

    // Bitrate
    place(110 * scale_);
    ImGui::SetNextItemWidth(110 * scale_);
    const std::string br = s.auto_bitrate ? std::string{"Bitrate auto"} : std::format("{} Mbps", s.bitrate_mbps);
    if (ImGui::BeginCombo("##tbr", br.c_str())) {
        if (ImGui::Selectable("Automático", s.auto_bitrate)) {
            s.auto_bitrate = true;
            changed = true;
        }
        for (const std::uint32_t m : {10u, 20u, 30u, 50u, 80u, 100u, 150u, 200u, 300u, 500u}) {
            if (ImGui::Selectable(std::format("{} Mbps", m).c_str(), !s.auto_bitrate && s.bitrate_mbps == m)) {
                s.auto_bitrate = false;
                s.bitrate_mbps = m;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    // Códec
    place(110 * scale_);
    ImGui::SetNextItemWidth(110 * scale_);
    if (ImGui::BeginCombo("##tcodec", std::string{to_string(s.codec)}.c_str())) {
        for (int i = 0; i <= 3; ++i) {
            const auto c = static_cast<CodecChoice>(i);
            if (ImGui::Selectable(std::string{to_string(c)}.c_str(), s.codec == c)) {
                s.codec = c;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    // Audio
    place(140 * scale_);
    ImGui::SetNextItemWidth(140 * scale_);
    const std::string audio_label = !s.audio ? std::string{"Audio apagado"}
                                             : std::format("Audio {}", s.audio_sample_rate == 0 ? std::string{"sistema"}
                                                                                                : std::format("{:.1f} kHz", s.audio_sample_rate / 1000.0));
    if (ImGui::BeginCombo("##taudio", audio_label.c_str())) {
        if (ImGui::Selectable("Apagado", !s.audio)) {
            s.audio = false;
            changed = true;
        }
        for (const std::uint32_t r : kAudioRates) {
            if (ImGui::Selectable(audio_rate_label(r).c_str(), s.audio && s.audio_sample_rate == r)) {
                s.audio = true;
                s.audio_sample_rate = r;
                changed = true;
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable("Estéreo", s.audio_stereo)) {
            s.audio_stereo = true;
            changed = true;
        }
        if (ImGui::Selectable("Mono", !s.audio_stereo)) {
            s.audio_stereo = false;
            changed = true;
        }
        ImGui::Separator();
        int vol = static_cast<int>(s.audio_volume);
        ImGui::SetNextItemWidth(160 * scale_);
        if (ImGui::SliderInt("Volumen", &vol, 0, 100, "%d %%")) s.audio_volume = static_cast<std::uint32_t>(vol);
        if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
        ImGui::EndCombo();
    }

    // Micrófono de este equipo -> host (solo si el host tiene un cable de audio virtual para recibirlo).
    if (info && info->mic_available) {
        const bool on = settings_.client.microphone;
        const char* mic_label = on ? "Micrófono encendido" : "Micrófono apagado";
        place(button_w(mic_label));
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(mic_label)) {
            settings_.client.microphone = !on;
            session_->set_microphone(settings_.client.microphone);
            save_settings();
        }
        if (on) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Tu micrófono llega al host como un micrófono más (para llamadas o juegos allá).");
    }

    // Segunda ventana: un botón, una lista o nada (si el host tiene un solo monitor).
    if (second_) {
        place(button_w(std::format("Cerrar ventana del monitor {}", second_monitor_id_)));
    } else if (info && info->monitors.size() >= 2 && !(stream && stream->monitor_id == 0)) {
        place(190 * scale_);
    }
    draw_second_window_controls();
    place(button_w("Latencia mínima"));
    if (primary_button("Latencia mínima")) apply_min_latency();
    ImGui::SetItemTooltip("FPS = Hz del monitor del host, V-Sync apagado, codificador rápido y pantalla completa");
    place(button_w("Calidad máxima"));
    if (primary_button("Calidad máxima")) apply_max_quality();
    ImGui::SetItemTooltip("Resolución nativa del host (sin escalar) y 50 %% más bitrate, con el codificador rápido: la imagen "
                          "mejora y la latencia sube muy poco (~2 ms en una GPU integrada)");
    place(button_w(show_stats_ ? "Ocultar estadísticas" : "Estadísticas"));
    if (ImGui::Button(show_stats_ ? "Ocultar estadísticas" : "Estadísticas")) {
        show_stats_ = !show_stats_;
        settings_.client.show_stats = show_stats_;
        save_settings();
    }
    place(button_w(settings_.client.show_graphs ? "Ocultar gráficos" : "Gráficos"));
    if (ImGui::Button(settings_.client.show_graphs ? "Ocultar gráficos" : "Gráficos")) toggle_graphs();
    ImGui::SetItemTooltip("Gráficos en tiempo real: latencia por cuadro, bitrate, pérdidas, FPS y RTT (Ctrl+Alt+Shift+G)");
    place(button_w("Medir latencia"));
    if (ImGui::Button("Medir latencia")) {
        session_->start_latency_test();
        show_stats_ = true;
    }
    ImGui::SetItemTooltip("El host muestra un cuadro blanco/negro en la esquina y se mide cuánto tarda en verse aquí");
    place(button_w("Más opciones"));
    if (ImGui::Button("Más opciones")) show_menu_ = true;
    place(button_w(fullscreen_ ? "Ventana" : "Pantalla completa"));
    if (ImGui::Button(fullscreen_ ? "Ventana" : "Pantalla completa")) toggle_fullscreen();
    place(button_w("Desconectar"));
    if (danger_button("Desconectar")) PostMessageW(hwnd_, detail::kMsgClientDisconnect, 0, 0);

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup) ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        toolbar_until_us_ = now + 1'500'000;
    }
    const float bar_bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - ImGui::GetMainViewport()->Pos.y;
    ImGui::End();

    // ✕ que sobresale en el centro, debajo de la barra: la esconde hasta que se pulse el ícono de arriba.
    const TabResult close = draw_toolbar_tab(true, bar_bottom);
    if (close.hovered) toolbar_until_us_ = now + 1'500'000;
    if (close.clicked) {
        settings_.client.toolbar_collapsed = true;
        save_settings();
        toolbar_until_us_ = 0;
        toolbar_tab_until_us_ = now + 1'500'000;
        toast("Barra oculta. Lleva el mouse arriba y pulsa la flecha del centro para volver a mostrarla");
    }

    if (changed) {
        settings_.client.active_profile.clear();
        apply_stream_settings();
        toast("Aplicando cambios…");
    }
}

void App::draw_stream_overlay() {
    const auto stream = session_->stream();

    draw_stream_toolbar();

    // Cursor remoto en modo "solo ver" (el del sistema no se mueve con el del host).
    if (!control_enabled() && stream) {
        const proto::CursorPos c = session_->cursor();
        if (c.visible) {
            const Rect view = view_rect({stream->width, stream->height}, backbuffer_size_, settings_.client.stream.scaling);
            const ImVec2 p(view.x + c.x * view.width, view.y + c.y * view.height);
            const float s = 18 * scale_;
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImVec2 pts[] = {p, ImVec2(p.x, p.y + s), ImVec2(p.x + s * 0.28f, p.y + s * 0.75f),
                                  ImVec2(p.x + s * 0.7f, p.y + s * 0.7f)};
            dl->AddConvexPolyFilled(pts, 4, IM_COL32(255, 255, 255, 255));
            dl->AddPolyline(pts, 4, IM_COL32(0, 0, 0, 255), 1.5f, ImDrawFlags_Closed);
        }
    }

    if (show_stats_) draw_stats_overlay();
    if (settings_.client.show_graphs) draw_graphs();
    if (!frame_presented_since_stream_) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), 0, ImVec2(0.5f, 0.5f));
        ImGui::Begin("##waiting", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::Text("Esperando video de %s…", session_->host_name().c_str());
        if (!renderer_->error().empty()) ImGui::TextColored(kBad, "%s", renderer_->error().c_str());
        const std::string msg = session_->message();
        if (!msg.empty()) ImGui::TextColored(kWarn, "%s", msg.c_str());
        if (ImGui::Button("Desconectar")) PostMessageW(hwnd_, detail::kMsgClientDisconnect, 0, 0);
        ImGui::End();
    }
    if (!show_menu_) return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Size.x * 0.5f, 20 * scale_), ImGuiCond_Appearing, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(900 * scale_, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (ImGui::Begin(std::format("LanCast · {}###menu", session_->host_name()).c_str(), &show_menu_,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (danger_button("Desconectar")) PostMessageW(hwnd_, detail::kMsgClientDisconnect, 0, 0);
        ImGui::SameLine();
        if (ImGui::Button(fullscreen_ ? "Salir de pantalla completa" : "Pantalla completa")) toggle_fullscreen();
        ImGui::SameLine();
        if (ImGui::Button(show_stats_ ? "Ocultar estadísticas" : "Estadísticas")) show_stats_ = !show_stats_;
        ImGui::SameLine();
        if (ImGui::Button(settings_.client.show_graphs ? "Ocultar gráficos" : "Gráficos")) toggle_graphs();
        ImGui::SameLine();
        if (ImGui::Checkbox("Solo ver", &settings_.client.view_only)) save_settings();
        ImGui::SameLine();
        if (ImGui::Checkbox("Mouse relativo (juegos)", &relative_mouse_)) set_relative_mouse(relative_mouse_);
        ImGui::SameLine();
        if (ImGui::Button("Enviar archivos…")) {
            const auto paths = sys::pick_files(hwnd_);
            if (!paths.empty() && session_->files().connected()) session_->files().send(paths);
        }
        if (const auto info = session_->host_info(); info && info->secure_desktop) {
            ImGui::SameLine();
            if (ImGui::Button("Ctrl+Alt+Supr")) send_secure_attention();
            ImGui::SetItemTooltip("También con Ctrl+Alt+Fin");
        }
        if (const auto info = session_->host_info()) {
            ImGui::TextUnformatted("Monitor:");
            for (const auto& m : info->monitors) {
                ImGui::SameLine();
                const bool sel = stream && stream->monitor_id == m.id;
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
                if (ImGui::Button(std::format("{}: {}", m.id, m.name).c_str())) switch_monitor(m.id);
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::SameLine();
            if (ImGui::Button("Todos")) switch_monitor(0);
            ImGui::SameLine();
            draw_second_window_controls();
        }
        ImGui::Separator();
        draw_stream_settings(true);
        ImGui::Separator();
        draw_transfers(session_->files(), "##ovt");
        ImGui::TextColored(kMuted,
                           "Atajos: Ctrl+Alt+Shift + M menú · S estadísticas · G gráficos · F pantalla completa · 1-9 monitor · 0 todos · "
                           "R mouse relativo · V solo ver · Q desconectar · Ctrl+Alt+Fin = Ctrl+Alt+Supr");
    }
    ImGui::End();
}

void App::draw_stats_overlay() {
    const client::Stats s = session_->stats();
    const auto stream = session_->stream();
    ImGui::SetNextWindowPos(ImVec2(12 * scale_, 12 * scale_));
    ImGui::SetNextWindowBgAlpha(0.72f);
    ImGui::Begin("##stats", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    if (stream) {
        ImGui::TextColored(kAccent, "%s %ux%u @ %u fps · %u Mbps%s%s", std::string{to_string(stream->codec)}.c_str(),
                           stream->width, stream->height, stream->fps, stream->bitrate_kbps / 1000,
                           stream->chroma_444 ? " · 4:4:4" : "", stream->hdr ? (hdr_output_ ? " · HDR" : " · HDR→SDR") : "");
    }
    ImGui::Text("FPS: %.0f   Bitrate: %.1f Mbps", s.fps, s.bitrate_mbps);
    ImGui::Separator();
    ImGui::Text("Captura+codificación  %s", fmt_ms(s.encode_ms).c_str());
    ImGui::Text("Red                   %s", fmt_ms(s.network_ms).c_str());
    ImGui::Text("Decodificación        %s", fmt_ms(s.decode_ms).c_str());
    ImGui::Text("Presentación          %s", fmt_ms(s.present_ms).c_str());
    if (!presentation_mode_.empty()) {
        ImGui::TextColored(presentation_direct_ ? kGood : kWarn, "Presentación %s", presentation_mode_.c_str());
        if (!presentation_direct_) ImGui::TextColored(kMuted, "  Pantalla completa (Ctrl+Alt+Shift+F) quita ese frame extra");
    }
    if (s.latency_test_running) {
        ImGui::TextColored(kAccent, "Midiendo latencia real… %d/10", s.latency_samples);
    } else if (s.real_latency_ms > 0) {
        const ImVec4 c = s.real_latency_ms < 15 ? kGood : s.real_latency_ms < 30 ? kWarn : kBad;
        ImGui::TextColored(c, "Latencia real medida: %.1f ms (%.1f–%.1f)", s.real_latency_ms, s.real_latency_min_ms,
                           s.real_latency_max_ms);
    }
    const ImVec4 total_color = s.total_ms < 10 ? kGood : s.total_ms < 25 ? kWarn : kBad;
    ImGui::TextColored(total_color, "Total extremo a extremo %s", fmt_ms(s.total_ms).c_str());
    ImGui::Separator();
    ImGui::Text("RTT: %s   Pérdida: %.2f %%", fmt_ms(s.rtt_ms).c_str(), s.packet_loss_pct);
    ImGui::Text("Recuperados por FEC: %llu   Perdidos: %llu   IDR pedidos: %llu",
                static_cast<unsigned long long>(s.frames_recovered), static_cast<unsigned long long>(s.frames_lost),
                static_cast<unsigned long long>(s.keyframe_requests));
    if (stream && stream->audio_rate > 0) ImGui::Text("Audio: %u Hz · buffer %.0f ms", stream->audio_rate, s.audio_buffer_ms);
    if (stream && !stream->encoder_backend.empty()) {
        ImGui::TextColored(kMuted, "Codificador: %s%s%s", stream->encoder_backend.c_str(),
                           stream->intra_refresh ? " · intra-refresh" : "", stream->slices ? " · slices" : "");
    }
    ImGui::TextColored(kMuted, "Decodificador: %s", renderer_ ? renderer_->decoder_name().c_str() : "-");
    ImGui::End();
}

void App::draw_pin_modal() {
    if (pin_modal_open_ && !ImGui::IsPopupOpen("Emparejar con PIN")) ImGui::OpenPopup("Emparejar con PIN");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Emparejar con PIN", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!pin_modal_open_ || !session_) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextWrapped("Es la primera vez que te conectas a %s.", session_->host_name().c_str());
            ImGui::TextUnformatted("Escribe el PIN de 6 dígitos que se muestra en ese equipo:");
            ImGui::PushFont(font_big_, ImGui::GetStyle().FontSizeBase * 1.8f);
            ImGui::SetNextItemWidth(220 * scale_);
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            bool enter = false;
            input_text("##pin", pin_input_, ImGuiInputTextFlags_CharsDecimal, &enter);
            ImGui::PopFont();
            if (pin_input_.size() > 6) pin_input_.resize(6);
            ImGui::BeginDisabled(pin_input_.size() != 6);
            if (primary_button("Emparejar") || (enter && pin_input_.size() == 6)) {
                session_->submit_pin(pin_input_);
                pin_modal_open_ = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancelar")) {
                pin_modal_open_ = false;
                ImGui::CloseCurrentPopup();
                disconnect_client();
            }
        }
        ImGui::EndPopup();
    }
}

void App::draw_toast() {
    if (toast_.empty() || gpu::now_us() > toast_until_us_) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y - 30 * scale_), 0, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("##toast", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(toast_.c_str());
    ImGui::End();
}

void App::draw_reconnect_overlay() {
    if (!reconnecting_ || streaming()) return;
    const double left = reconnect_deadline_us_ > gpu::now_us() ? (reconnect_deadline_us_ - gpu::now_us()) / 1e6 : 0.0;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::Begin("##reconnect", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    const char* spinner = "|/-\\";
    ImGui::TextColored(kWarn, "Se perdió la conexión con %s", reconnect_host_name_.c_str());
    ImGui::Text("%c  Reconectando… intento %d (quedan %.0f s)", spinner[static_cast<int>(ImGui::GetTime() * 8) % 4],
                reconnect_attempts_, left);
    if (ImGui::Button("Cancelar")) disconnect_client();
    ImGui::End();
}

void App::draw_second_window_controls() {
    if (second_) {
        if (ImGui::Button(std::format("Cerrar ventana del monitor {}", second_monitor_id_).c_str())) close_second_window();
        return;
    }
    const auto info = session_->host_info();
    const auto stream = session_->stream();
    if (!info || info->monitors.size() < 2 || (stream && stream->monitor_id == 0)) return;
    ImGui::SetNextItemWidth(190 * scale_);
    if (ImGui::BeginCombo("##second", "Abrir otro monitor…")) {
        for (const auto& m : info->monitors) {
            if (stream && stream->monitor_id == m.id) continue;
            if (ImGui::Selectable(std::format("{}: {} ({}x{})", m.id, m.name, m.rect.width, m.rect.height).c_str())) {
                open_second_window(m.id);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Muestra otro monitor del host en una segunda ventana (o en tu segundo monitor)");
}

// ------------------------------------------------------------------------------------------------ gráficos

namespace {

constexpr float kGraphSeconds = 10.0f;

struct Chart {
    ImDrawList* dl = nullptr;
    ImVec2 p0;
    ImVec2 p1;
    std::uint64_t now = 0;

    [[nodiscard]] float x_of(std::uint64_t t) const {
        const float age = static_cast<float>(now - std::min(t, now)) / 1e6f;
        return p1.x - (age / kGraphSeconds) * (p1.x - p0.x);
    }
    [[nodiscard]] float y_of(float v, float max) const { return p1.y - std::clamp(v / max, 0.0f, 1.0f) * (p1.y - p0.y); }
};

// Escala "redonda" para el eje Y (5, 10, 20, 25, 50, 100...).
float nice_max(float v, float floor_value) {
    v = std::max(v, floor_value);
    for (const float step : {5.f, 10.f, 15.f, 20.f, 25.f, 30.f, 40.f, 50.f, 75.f, 100.f, 150.f, 200.f, 300.f, 500.f, 1000.f}) {
        if (v <= step) return step;
    }
    return std::ceil(v / 500.f) * 500.f;
}

Chart begin_chart(float height, float y_max, const char* unit, float scale) {
    Chart c;
    c.dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float left = 34.0f * scale;  // Etiquetas del eje Y.
    c.p0 = ImVec2(pos.x + left, pos.y);
    c.p1 = ImVec2(pos.x + w, pos.y + height);
    c.dl->AddRectFilled(c.p0, c.p1, IM_COL32(18, 20, 26, 230), 4.0f);
    // Líneas de referencia en 0, 50 % y 100 % de la escala.
    for (const float f : {0.0f, 0.5f, 1.0f}) {
        const float y = c.p1.y - f * (c.p1.y - c.p0.y);
        c.dl->AddLine(ImVec2(c.p0.x, y), ImVec2(c.p1.x, y), IM_COL32(255, 255, 255, f == 0.0f ? 60 : 25));
        char label[32];
        std::snprintf(label, sizeof(label), "%g%s", y_max * f, f == 1.0f ? unit : "");
        c.dl->AddText(ImVec2(pos.x, y - ImGui::GetFontSize() * 0.5f), IM_COL32(150, 155, 165, 255), label);
    }
    ImGui::Dummy(ImVec2(w, height));
    return c;
}

void legend(ImU32 color, const char* text) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float s = ImGui::GetFontSize() * 0.6f;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + s * 0.4f), ImVec2(p.x + s, p.y + s * 1.4f), color, 2.0f);
    ImGui::Dummy(ImVec2(s + 4, s));
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(text);
}

float percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0;
    std::ranges::sort(v);
    return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<float>(v.size() - 1)))];
}

}  // namespace

void App::toggle_graphs() {
    settings_.client.show_graphs = !settings_.client.show_graphs;
    save_settings();
}

void App::draw_graphs() {
    const auto frames = session_->frame_history();
    const auto net = session_->net_history();
    const std::uint64_t now = gpu::now_us();
    const float k = scale_;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = std::min(470.0f * k, vp->Size.x * 0.45f);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - width - 12 * k, vp->Pos.y + 12 * k), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.80f);
    ImGui::Begin("##graphs", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

    const ImU32 c_encode = IM_COL32(74, 144, 226, 255);
    const ImU32 c_network = IM_COL32(76, 199, 119, 255);
    const ImU32 c_client = IM_COL32(240, 196, 70, 255);
    const ImU32 c_wait = IM_COL32(140, 145, 155, 200);
    const ImU32 c_lost = IM_COL32(235, 70, 70, 255);
    const ImU32 c_fec = IM_COL32(245, 150, 50, 255);
    const ImU32 c_idr = IM_COL32(180, 110, 240, 255);

    // --- Latencia por cuadro (apilada) ---
    std::vector<float> totals;
    totals.reserve(frames.size());
    for (const auto& f : frames) {
        if (now - f.time_us < static_cast<std::uint64_t>(kGraphSeconds * 1e6f)) totals.push_back(f.total_ms);
    }
    const float last_total = frames.empty() ? 0.0f : frames.back().total_ms;
    ImGui::TextColored(kAccent, "Latencia por cuadro");
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "ahora %.1f ms · p95 %.1f · máx %.1f", last_total, percentile(totals, 0.95f),
                       totals.empty() ? 0.0f : *std::ranges::max_element(totals));
    {
        const float y_max = nice_max(percentile(totals, 0.98f) * 1.15f, 10.0f);
        Chart c = begin_chart(110 * k, y_max, " ms", k);
        c.now = now;
        // Por columna de píxeles se dibuja el cuadro con mayor latencia (así se ven los picos).
        const int columns = std::max(1, static_cast<int>(c.p1.x - c.p0.x));
        std::vector<const lancast::client::FrameSample*> worst(static_cast<std::size_t>(columns), nullptr);
        for (const auto& f : frames) {
            const int col = static_cast<int>(c.x_of(f.time_us) - c.p0.x);
            if (col < 0 || col >= columns) continue;
            auto& w = worst[static_cast<std::size_t>(col)];
            if (w == nullptr || f.total_ms > w->total_ms) w = &f;
        }
        for (int col = 0; col < columns; ++col) {
            const auto* f = worst[static_cast<std::size_t>(col)];
            if (f == nullptr) continue;
            const float x = c.p0.x + static_cast<float>(col);
            float base = 0;
            auto seg = [&](float v, ImU32 color) {
                if (v <= 0) return;
                c.dl->AddLine(ImVec2(x, c.y_of(base, y_max)), ImVec2(x, c.y_of(base + v, y_max)), color, 1.0f);
                base += v;
            };
            seg(f->encode_ms, c_encode);
            seg(f->network_ms, c_network);
            seg(f->decode_ms + f->present_ms, c_client);
            seg(std::max(0.0f, f->total_ms - base), c_wait);  // Resto: espera hasta la presentación (V-Sync, cola).
            if (f->keyframe) c.dl->AddCircleFilled(ImVec2(x, c.p1.y - 3 * k), 2.0f * k, c_idr);
        }
        // Eventos de pérdida (líneas verticales).
        for (const auto& n : net) {
            const float x = c.x_of(n.time_us);
            if (x < c.p0.x) continue;
            if (n.frames_lost > 0) {
                c.dl->AddLine(ImVec2(x, c.p0.y), ImVec2(x, c.p1.y), c_lost, 1.5f);
            } else if (n.fec_recovered > 0) {
                c.dl->AddLine(ImVec2(x, c.p0.y), ImVec2(x, c.p1.y), c_fec, 1.0f);
            }
            if (n.keyframe_requests > 0) c.dl->AddTriangleFilled(ImVec2(x - 4 * k, c.p0.y), ImVec2(x + 4 * k, c.p0.y),
                                                                  ImVec2(x, c.p0.y + 6 * k), c_idr);
        }
    }
    legend(c_encode, "Host (captura+codif.)");
    ImGui::SameLine();
    legend(c_network, "Red");
    ImGui::SameLine();
    legend(c_client, "Decod.+dibujo");
    ImGui::SameLine();
    legend(c_wait, "Espera");
    legend(c_lost, "Cuadro perdido");
    ImGui::SameLine();
    legend(c_fec, "Reparado por FEC");
    ImGui::SameLine();
    legend(c_idr, "Keyframe / pedido");

    // --- Bitrate y pérdida de paquetes ---
    ImGui::Spacing();
    float max_mbps = 0;
    float max_loss = 0;
    for (const auto& n : net) {
        max_mbps = std::max(max_mbps, n.mbps);
        max_loss = std::max(max_loss, n.loss_pct);
    }
    const float last_mbps = net.empty() ? 0.0f : net.back().mbps;
    const float last_loss = net.empty() ? 0.0f : net.back().loss_pct;
    ImGui::TextColored(kAccent, "Bitrate");
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "ahora %.1f Mbps · máx %.1f · pérdida %.2f %%", last_mbps, max_mbps, last_loss);
    {
        const float y_max = nice_max(max_mbps * 1.1f, 5.0f);
        const float loss_max = std::max(5.0f, max_loss);
        Chart c = begin_chart(70 * k, y_max, "", k);
        c.now = now;
        const float bar = std::max(2.0f, (c.p1.x - c.p0.x) / (kGraphSeconds * 4.0f) - 1.0f);
        ImVec2 prev{-1, -1};
        for (const auto& n : net) {
            const float x = c.x_of(n.time_us);
            if (x < c.p0.x) continue;
            if (n.loss_pct > 0.001f) {
                const float h = std::clamp(n.loss_pct / loss_max, 0.02f, 1.0f) * (c.p1.y - c.p0.y);
                c.dl->AddRectFilled(ImVec2(x - bar, c.p1.y - h), ImVec2(x, c.p1.y), IM_COL32(235, 70, 70, 170));
            }
            const ImVec2 p(x, c.y_of(n.mbps, y_max));
            if (prev.x >= 0) c.dl->AddLine(prev, p, c_encode, 2.0f);
            prev = p;
        }
        c.dl->AddText(ImVec2(c.p1.x - 70 * k, c.p0.y + 2), IM_COL32(150, 155, 165, 255), "Mbps");
    }

    // --- FPS y RTT ---
    ImGui::Spacing();
    float max_fps = 0;
    float max_rtt = 0;
    for (const auto& n : net) {
        max_fps = std::max(max_fps, n.fps);
        max_rtt = std::max(max_rtt, n.rtt_ms);
    }
    const float last_fps = net.empty() ? 0.0f : net.back().fps;
    const float last_rtt = net.empty() ? 0.0f : net.back().rtt_ms;
    ImGui::TextColored(kAccent, "FPS y RTT");
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%.0f fps · RTT %.1f ms (máx %.1f)", last_fps, last_rtt, max_rtt);
    {
        const float fps_max = nice_max(max_fps * 1.1f, 30.0f);
        const float rtt_max = nice_max(max_rtt * 1.2f, 5.0f);
        Chart c = begin_chart(60 * k, fps_max, " fps", k);
        c.now = now;
        ImVec2 pf{-1, -1};
        ImVec2 pr{-1, -1};
        for (const auto& n : net) {
            const float x = c.x_of(n.time_us);
            if (x < c.p0.x) continue;
            const ImVec2 f(x, c.y_of(n.fps, fps_max));
            const ImVec2 r(x, c.y_of(n.rtt_ms, rtt_max));
            if (pf.x >= 0) {
                c.dl->AddLine(pf, f, c_network, 2.0f);
                c.dl->AddLine(pr, r, IM_COL32(90, 200, 230, 255), 1.5f);
            }
            pf = f;
            pr = r;
        }
        char rtt_label[32];
        std::snprintf(rtt_label, sizeof(rtt_label), "RTT 0-%g ms", rtt_max);
        c.dl->AddText(ImVec2(c.p1.x - 110 * k, c.p0.y + 2), IM_COL32(90, 200, 230, 255), rtt_label);
    }
    legend(c_network, "FPS");
    ImGui::SameLine();
    legend(IM_COL32(90, 200, 230, 255), "RTT (escala propia)");
    ImGui::TextColored(kMuted, "Últimos %.0f s · Ctrl+Alt+Shift+G oculta", kGraphSeconds);
    ImGui::End();
}

}  // namespace lancast::app
