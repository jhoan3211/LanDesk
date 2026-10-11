#pragma once

// Colores y controles reutilizables de la interfaz (Dear ImGui).

#include <cstdint>
#include <cstdio>
#include <format>
#include <string>

#include "imgui.h"

namespace lancast::app::ui {

inline const ImVec4 kAccent{0.29f, 0.56f, 0.96f, 1.0f};
inline const ImVec4 kGood{0.30f, 0.78f, 0.47f, 1.0f};
inline const ImVec4 kWarn{0.95f, 0.70f, 0.25f, 1.0f};
inline const ImVec4 kBad{0.93f, 0.36f, 0.36f, 1.0f};
inline const ImVec4 kMuted{0.62f, 0.65f, 0.70f, 1.0f};

// Actualiza `value` en cada tecla (así los botones ven el texto al instante) y devuelve true si cambió.
// Si se pasa `enter`, indica si se pulsó Enter en este campo.
inline bool input_text(const char* label, std::string& value, ImGuiInputTextFlags flags = 0, bool* enter = nullptr) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    const bool changed = ImGui::InputText(label, buf, sizeof(buf), flags & ~ImGuiInputTextFlags_EnterReturnsTrue);
    if (changed) value = buf;
    if (enter != nullptr) {
        *enter = ImGui::IsItemDeactivated() &&
                 (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    }
    return changed;
}

inline void badge(const char* text, const ImVec4& color) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x, color.y, color.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x, color.y, color.z, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::SmallButton(text);
    ImGui::PopStyleColor(4);
}

inline void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

inline void section(const char* title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
}

inline bool primary_button(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.53f, 0.95f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.17f, 0.40f, 0.78f, 1.0f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

inline bool danger_button(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.26f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.16f, 0.16f, 1.0f));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

inline std::string fmt_ms(double v) { return v <= 0 ? "--" : std::format("{:.1f} ms", v); }

inline constexpr std::uint32_t kAudioRates[] = {0, 48000, 44100, 32000, 24000, 16000};

inline std::string audio_rate_label(std::uint32_t rate) {
    return rate == 0 ? std::string{"La del sistema"} : std::format("{} Hz", rate);
}

}  // namespace lancast::app::ui
