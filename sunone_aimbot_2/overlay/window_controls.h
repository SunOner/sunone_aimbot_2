#pragma once

#include "imgui/imgui.h"
#include "window_geometry.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace OverlayWindowControls
{
constexpr float ButtonSize = 32;
constexpr float Spacing = 4;
constexpr float Width = ButtonSize * 3 + Spacing * 2;
constexpr float ReservedWidth = Width + Spacing;
constexpr float HeaderHeight = ButtonSize + 5;
enum class Action { None, Hide, Maximize, Fullscreen };

// Settings scroll below the fixed header. Graphs already have a toolbar on
// this row and reserve only its right-hand end for the window controls.
inline void reserveHeader(bool graph)
{
    if (!graph) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + HeaderHeight);
}

struct Strip
{
    std::array<ImVec4, 3> bounds{};

    bool contains(float x, float y) const
    {
        for (const auto& rect : bounds)
            if (x >= rect.x && x < rect.z && y >= rect.y && y < rect.w) return true;
        return false;
    }

    Action draw(OverlayWindowGeometry::Mode mode)
    {
        const auto window = ImGui::GetWindowPos();
        const auto maximum = ImGui::GetWindowContentRegionMax();
        const auto minimum = ImGui::GetWindowContentRegionMin();
        ImGui::SetCursorScreenPos(ImVec2(window.x + maximum.x - Width, window.y + minimum.y));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Spacing, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##overlay_window_controls", ImVec2(Width, ButtonSize),
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings);
        Action action = Action::None;
        for (int index = 0; index < 3; ++index)
        {
            if (index) ImGui::SameLine();
            const char* ids[] = {"hide", "maximize", "fullscreen"};
            const auto position = ImGui::GetCursorScreenPos();
            const ImVec2 center(position.x + ButtonSize / 2, position.y + ButtonSize / 2);
            bounds[index] = ImVec4(position.x, position.y, position.x + ButtonSize, position.y + ButtonSize);
            const bool clicked = ImGui::InvisibleButton(ids[index], ImVec2(ButtonSize, ButtonSize));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PushID(ids[index]);
            const auto fadeId = ImGui::GetID("hover");
            const float previous = ImGui::GetStateStorage()->GetFloat(fadeId);
            const float fade = previous + ((hovered ? 1.0f : 0.0f) - previous) *
                (1 - std::exp(-18 * std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f)));
            ImGui::GetStateStorage()->SetFloat(fadeId, fade);
            ImGui::PopID();
            const bool active = (index == 1 && mode == OverlayWindowGeometry::Mode::Maximized) ||
                (index == 2 && mode == OverlayWindowGeometry::Mode::Fullscreen);
            auto* draw = ImGui::GetWindowDrawList();
            const ImVec2 end(position.x + ButtonSize, position.y + ButtonSize);
            draw->AddRectFilled(position, end, active ? IM_COL32(38, 108, 145, 215) :
                IM_COL32(39 + static_cast<int>(23 * fade), 42 + static_cast<int>(25 * fade), 48 + static_cast<int>(28 * fade), 255), 6);
            if (active) draw->AddRect(position, end, IM_COL32(96, 205, 255, 210), 6);
            const ImU32 color = IM_COL32(222, 231, 241, 255);
            const auto point = [&](float x, float y) { return ImVec2(center.x + x, center.y + y); };
            const auto line = [&](float x, float y, float u, float v) { draw->AddLine(point(x, y), point(u, v), color, 1.7f); };
            if (index == 0) line(-7, 5, 7, 5);
            else if (index == 1)
            {
                if (active)
                {
                    draw->AddRect(point(-7, -3), point(4, 8), color, 1, 0, 1.6f);
                    line(-3, -3, -3, -7); line(-3, -7, 8, -7); line(8, -7, 8, 4); line(8, 4, 4, 4);
                }
                else { draw->AddRect(point(-8, -7), point(8, 7), color, 1, 0, 1.6f); line(-8, -3, 8, -3); }
            }
            else for (float x : {-1.0f, 1.0f}) for (float y : {-1.0f, 1.0f})
            {
                const float outer = active ? 3.0f : 8.0f, inner = active ? 8.0f : 3.0f;
                line(outer * x, outer * y, inner * x, outer * y); line(outer * x, outer * y, outer * x, inner * y);
            }
            if (hovered)
            {
                const char* tooltip = index == 0 ? "Hide overlay\nUse the overlay hotkey to show it again." :
                    index == 1 ? (active ? "Restore window size" : "Maximize window (keep taskbar visible)") :
                    (active ? "Exit fullscreen (F11)" : "Fullscreen (F11)");
                ImGui::SetTooltip("%s", tooltip);
            }
            if (clicked) action = index == 0 ? Action::Hide : index == 1 ? Action::Maximize : Action::Fullscreen;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return action;
    }
};
}
