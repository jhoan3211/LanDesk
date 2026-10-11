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

void App::draw_stream_toolbar() {
    // Aparece al llevar el mouse al borde superior de la ventana (como en otros escritorios remotos).
    const std::uint64_t now = gpu::now_us();
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MousePos.y >= 0 && io.MousePos.y < 6.0f * scale_ && ImGui::IsMousePosValid()) toolbar_until_us_ = now + 1'500'000;
    if (now > toolbar_until_us_ && !show_menu_) return;
    if (show_menu_) return;  // El menú completo ya está abierto.

    StreamSettings& s = settings_.client.stream;
    const auto stream = session_->stream();
    const auto info = session_->host_info();
    bool changed = false;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Size.x * 0.5f, 0), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##toolbar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoMove);
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
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * scale_);
    if (ImGui::BeginCombo("##tres", std::string{to_string(s.resolution)}.c_str())) {
        for (int i = 0; i < static_cast<int>(ResolutionPreset::Custom); ++i) {
            const auto r = static_cast<ResolutionPreset>(i);
            if (ImGui::Selectable(std::string{to_string(r)}.c_str(), s.resolution == r)) {
                s.resolution = r;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Resolución");

    // FPS
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * scale_);
    if (ImGui::BeginCombo("##tfps", std::format("{} fps", s.fps).c_str())) {
        for (const std::uint32_t f : {30u, 60u, 90u, 120u, 144u, 165u, 240u}) {
            if (ImGui::Selectable(std::format("{} fps", f).c_str(), s.fps == f)) {
                s.fps = f;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    // Monitor
    ImGui::SameLine();
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
    ImGui::SameLine();
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
    ImGui::SameLine();
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
    ImGui::SameLine();
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

    ImGui::SameLine();
    if (primary_button("Latencia mínima")) apply_min_latency();
    ImGui::SetItemTooltip("FPS = Hz del monitor del host, V-Sync apagado, codificador rápido y pantalla completa");
    ImGui::SameLine();
    if (ImGui::Button(show_stats_ ? "Ocultar estadísticas" : "Estadísticas")) {
        show_stats_ = !show_stats_;
        settings_.client.show_stats = show_stats_;
        save_settings();
    }
    ImGui::SameLine();
    if (ImGui::Button("Medir latencia")) {
        session_->start_latency_test();
        show_stats_ = true;
    }
    ImGui::SetItemTooltip("El host muestra un cuadro blanco/negro en la esquina y se mide cuánto tarda en verse aquí");
    ImGui::SameLine();
    if (ImGui::Button("Más opciones")) show_menu_ = true;
    ImGui::SameLine();
    if (ImGui::Button(fullscreen_ ? "Ventana" : "Pantalla completa")) toggle_fullscreen();
    ImGui::SameLine();
    if (danger_button("Desconectar")) PostMessageW(hwnd_, detail::kMsgClientDisconnect, 0, 0);

    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByPopup) ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        toolbar_until_us_ = now + 1'500'000;
    }
    ImGui::End();

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
        if (ImGui::Checkbox("Solo ver", &settings_.client.view_only)) save_settings();
        ImGui::SameLine();
        if (ImGui::Checkbox("Mouse relativo (juegos)", &relative_mouse_)) set_relative_mouse(relative_mouse_);
        ImGui::SameLine();
        if (ImGui::Button("Enviar archivos…")) {
            const auto paths = sys::pick_files(hwnd_);
            if (!paths.empty() && session_->files().connected()) session_->files().send(paths);
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
        }
        ImGui::Separator();
        draw_stream_settings(true);
        ImGui::Separator();
        draw_transfers(session_->files(), "##ovt");
        ImGui::TextColored(kMuted,
                           "Atajos: Ctrl+Alt+Shift + M menú · S estadísticas · F pantalla completa · 1-9 monitor · 0 todos · "
                           "R mouse relativo · V solo ver · Q desconectar");
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
        ImGui::TextColored(kAccent, "%s %ux%u @ %u fps · %u Mbps", std::string{to_string(stream->codec)}.c_str(), stream->width,
                           stream->height, stream->fps, stream->bitrate_kbps / 1000);
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

}  // namespace lancast::app
