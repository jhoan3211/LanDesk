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

void App::set_style(float scale) {
    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.ChildRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.TabRounding = 6.0f;
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.WindowPadding = ImVec2(16, 14);
    style.ScrollbarSize = 12;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.082f, 0.098f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.105f, 0.114f, 0.137f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.145f, 0.98f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.23f, 0.27f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.27f, 0.32f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.19f, 0.21f, 0.25f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.28f, 0.34f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.29f, 0.33f, 0.40f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.30f, 0.48f, 0.7f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.36f, 0.58f, 0.8f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.40f, 0.64f, 1.0f);
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.65f, 1.0f, 1.0f);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.24f, 0.28f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    ImGui::GetStyle() = style;
}

// ------------------------------------------------------------------------------------------------ raíz

void App::draw_ui() {
    if (streaming()) {
        draw_stream_overlay();
    } else {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        draw_sidebar();
        ImGui::SameLine();
        ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        switch (page_) {
            case Page::Connect:
                draw_connect_page();
                break;
            case Page::Host:
                draw_host_page();
                break;
            case Page::Files:
                draw_files_page();
                break;
            case Page::Settings:
                draw_settings_page();
                break;
            case Page::Diagnostics:
                draw_diagnostics_page();
                break;
        }
        ImGui::EndChild();
        ImGui::End();
    }
    draw_reconnect_overlay();
    draw_pin_modal();
    draw_toast();
}

void App::draw_sidebar() {
    const float w = 210.0f * scale_;
    ImGui::BeginChild("##sidebar", ImVec2(w, 0), ImGuiChildFlags_None);
    ImGui::PushFont(font_big_, ImGui::GetStyle().FontSizeBase * 1.6f);
    ImGui::TextColored(kAccent, "LanCast");
    ImGui::PopFont();
    ImGui::TextColored(kMuted, "%s · v1.2.0", settings_.device_name.c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const host::Status st = host_.status();
    auto item = [&](Page p, const char* label, const char* extra = nullptr) {
        const bool selected = page_ == p;
        if (ImGui::Selectable(label, selected, 0, ImVec2(0, 34 * scale_))) page_ = p;
        if (extra != nullptr) {
            ImGui::SameLine(w - 70 * scale_);
            badge(extra, kWarn);
        }
    };
    item(Page::Connect, "  Conectar a un equipo");
    item(Page::Host, "  Este equipo (host)", st.pin ? "PIN" : (st.session_active ? "activo" : nullptr));
    item(Page::Files, "  Archivos");
    item(Page::Settings, "  Ajustes");
    item(Page::Diagnostics, "  Diagnóstico");

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 80 * scale_);
    ImGui::Separator();
    if (st.listening) {
        ImGui::TextColored(kGood, "● Aceptando conexiones");
        if (!st.addresses.empty()) ImGui::TextColored(kMuted, "%s", st.addresses.front().c_str());
    } else if (!settings_.host.enabled) {
        ImGui::TextColored(kMuted, "○ Host desactivado");
    } else {
        ImGui::TextColored(kBad, "● Host con error");
    }
    ImGui::EndChild();
}

}  // namespace lancast::app
