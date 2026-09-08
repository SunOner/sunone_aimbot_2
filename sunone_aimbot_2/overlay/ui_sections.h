#ifndef OVERLAY_UI_SECTIONS_H
#define OVERLAY_UI_SECTIONS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "imgui/imgui.h"
#include "medius_ui.h"

namespace OverlayUI
{
struct SettingRow
{
    ImVec2 min;
    ImVec2 max;
    float controlWidth;
};

inline bool& IsMediusInputSection() noexcept
{
    static thread_local bool value = false;
    return value;
}

inline float AdaptiveItemWidth(float ratio = 0.64f) noexcept
{
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail <= 1.0f)
        return 1.0f;

    const float maxW = avail * 0.92f;
    const float minW = avail * ((avail < 280.0f) ? 0.62f : 0.42f);
    const float boundedMin = (minW < maxW) ? minW : maxW;
    const float target = avail * ratio;
    return std::clamp(target, boundedMin, maxW);
}

inline float AnimateFloat(const char* id, float target, float speed = 16.0f, float initial = 0.0f) noexcept
{
    ImGuiStorage* storage = ImGui::GetStateStorage();
    float* value = storage->GetFloatRef(ImGui::GetID(id), initial);
    const float dt = std::min(ImGui::GetIO().DeltaTime, 1.0f / 15.0f);
    const float step = 1.0f - std::exp(-speed * dt);
    *value += (target - *value) * step;
    if (std::abs(*value - target) < 0.001f)
        *value = target;
    return *value;
}

inline ImU32 LerpColor(ImU32 a, ImU32 b, float t) noexcept
{
    t = std::clamp(t, 0.0f, 1.0f);
    const ImVec4 av = ImGui::ColorConvertU32ToFloat4(a);
    const ImVec4 bv = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        av.x + (bv.x - av.x) * t,
        av.y + (bv.y - av.y) * t,
        av.z + (bv.z - av.z) * t,
        av.w + (bv.w - av.w) * t));
}

inline SettingRow BeginSettingRow(const char* label, float height = 58.0f, float controlRatio = 0.44f) noexcept
{
    ImGui::PushID(label);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const ImVec2 rowMax(rowMin.x + avail, rowMin.y + height);
    const bool hoverable = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const bool hovered = hoverable && ImGui::IsMouseHoveringRect(rowMin, rowMax);
    const float hoverAnim = AnimateFloat("##row_hover_anim", hovered ? 1.0f : 0.0f, 16.0f, 0.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(rowMin, rowMax, LerpColor(IM_COL32(37, 39, 44, 232), IM_COL32(45, 47, 52, 238), hoverAnim), 3.0f);
    draw->AddRect(rowMin, rowMax, IM_COL32(255, 255, 255, static_cast<int>(6.0f + 8.0f * hoverAnim)), 3.0f, 0, 1.0f);

    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    const float labelY = rowMin.y + (height - labelSize.y) * 0.5f;
    draw->AddText(ImVec2(rowMin.x + 15.0f, labelY), IM_COL32(232, 232, 236, 245), label);

    const float maxControlW = std::min(320.0f, std::max(180.0f, avail - 205.0f));
    const float minControlW = std::min(210.0f, maxControlW);
    const float controlW = std::clamp(avail * controlRatio, minControlW, maxControlW);
    const float controlY = rowMin.y + (height - ImGui::GetFrameHeight()) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(rowMax.x - controlW - 15.0f, controlY));
    ImGui::SetNextItemWidth(controlW);

    IM_UNUSED(style);
    return { rowMin, rowMax, controlW };
}

inline void EndSettingRow(const SettingRow& row) noexcept
{
    ImGui::SetCursorScreenPos(row.min);
    ImGui::Dummy(ImVec2(row.max.x - row.min.x, row.max.y - row.min.y));
    ImGui::PopID();
}

inline bool CheckboxRow(const char* label, bool* value, const char* id = "##value") noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::Checkbox(id, value);
    EndSettingRow(row);
    return changed;
}

inline bool SliderIntRow(const char* label, int* value, int minValue, int maxValue, const char* format = "%d", const char* id = "##value") noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::SliderInt(id, value, minValue, maxValue, format);
    EndSettingRow(row);
    return changed;
}

inline bool SliderFloatRow(const char* label, float* value, float minValue, float maxValue, const char* format = "%.3f", const char* id = "##value") noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::SliderFloat(id, value, minValue, maxValue, format);
    EndSettingRow(row);
    return changed;
}

inline bool InputTextRow(const char* label, char* buffer, size_t bufferSize, ImGuiInputTextFlags flags = 0, const char* id = "##value") noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::InputText(id, buffer, bufferSize, flags);
    EndSettingRow(row);
    return changed;
}

inline bool InputIntRow(const char* label, int* value, int step = 1, int stepFast = 100, ImGuiInputTextFlags flags = 0, const char* id = "##value") noexcept
{
    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::InputInt(id, value, step, stepFast, flags);
    EndSettingRow(row);
    return changed;
}

inline bool ButtonRow(const char* label, const char* buttonText, const char* id = nullptr) noexcept
{
    ImGui::PushID(id ? id : label);
    const SettingRow row = BeginSettingRow(label);
    const bool clicked = ImGui::Button(buttonText, ImVec2(row.controlWidth, 0.0f));
    EndSettingRow(row);
    ImGui::PopID();
    return clicked;
}

inline bool ComboRow(const char* label, int* currentItem, const char* const items[], int itemsCount, const char* id = "##value") noexcept
{
    // draw_mouse.cpp owns the existing backend list. Inject MEDIUS here without changing its
    // indexing contract: selecting MEDIUS writes the method directly and returns false, while
    // selecting any existing backend returns the legacy index to the caller.
    if (label && std::strcmp(label, "Mouse Input Method") == 0)
    {
        const SettingRow row = BeginSettingRow(label);
        const bool mediusActive = MediusUI::IsActive();
        const char* preview = "";
        if (mediusActive)
            preview = "MEDIUS";
        else if (*currentItem >= 0 && *currentItem < itemsCount)
            preview = items[*currentItem];

        bool existingChanged = false;
        int newExistingIndex = *currentItem;
        if (ImGui::BeginCombo(id, preview))
        {
            for (int i = 0; i < itemsCount; ++i)
            {
                const bool selected = !mediusActive && i == *currentItem;
                if (ImGui::Selectable(items[i], selected))
                {
                    newExistingIndex = i;
                    existingChanged = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }

            if (ImGui::Selectable("MEDIUS", mediusActive))
                MediusUI::Select();
            if (mediusActive)
                ImGui::SetItemDefaultFocus();
            ImGui::EndCombo();
        }
        EndSettingRow(row);

        if (existingChanged)
        {
            *currentItem = newExistingIndex;
            return true;
        }
        return false;
    }

    const SettingRow row = BeginSettingRow(label);
    const bool changed = ImGui::Combo(id, currentItem, items, itemsCount);
    EndSettingRow(row);
    return changed;
}

inline void TextRow(const char* text, ImU32 color = IM_COL32(255, 236, 86, 255), float height = 32.0f) noexcept
{
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const ImVec2 rowMax(rowMin.x + avail, rowMin.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(rowMin, rowMax, IM_COL32(37, 39, 44, 145), 3.0f);
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    draw->AddText(ImVec2(rowMin.x + 14.0f, rowMin.y + (height - textSize.y) * 0.5f), color, text);
    ImGui::Dummy(ImVec2(avail, height + 6.0f));
}

inline void DrawBodyFrame(const ImVec2& min, const ImVec2& max, bool subsection = false) noexcept
{
    IM_UNUSED(min);
    IM_UNUSED(max);
    IM_UNUSED(subsection);
}

inline void DrawSectionHeader(const char* label, bool subsection = false) noexcept
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float height = subsection ? 26.0f : 29.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 max(pos.x + avail, pos.y + height);

    ImGui::InvisibleButton(subsection ? "##subsection_header" : "##section_header", ImVec2(avail, height));
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 bg = hovered ? IM_COL32(46, 47, 52, 232) : IM_COL32(38, 39, 44, 220);
    const ImU32 border = IM_COL32(255, 255, 255, subsection ? 30 : 42);
    const float rounding = subsection ? 5.0f : 6.0f;
    drawList->AddRectFilled(pos, max, bg, rounding);
    drawList->AddRect(pos, max, border, rounding, 0, 1.0f);

    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const float textY = pos.y + (height - textSize.y) * 0.5f;
    drawList->AddText(ImVec2(pos.x + 15.0f, textY), subsection ? IM_COL32(218, 218, 222, 245) : IM_COL32(235, 235, 238, 250), label);
}

inline void BeginBodyGroup(bool subsection = false) noexcept
{
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7.0f, subsection ? 5.0f : 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f, 5.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(47, 47, 47, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(57, 57, 57, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(67, 67, 67, 255));
    ImGui::PushItemWidth(AdaptiveItemWidth(subsection ? 0.68f : 0.64f));
    ImGui::Dummy(ImVec2(0.0f, subsection ? 4.0f : 6.0f));
}

inline void EndBodyGroup(bool subsection = false) noexcept
{
    IM_UNUSED(subsection);
    ImGui::PopItemWidth();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(4);
    ImGui::EndGroup();
}

inline bool BeginSection(const char* label, const char* id = nullptr, bool defaultOpen = true) noexcept
{
    ImGui::PushID(id ? id : label);

    IM_UNUSED(defaultOpen);
    IM_UNUSED(label);

    IsMediusInputSection() = (id && std::strcmp(id, "mouse_section_input_method") == 0);
    BeginBodyGroup(false);
    return true;
}

inline void EndSection() noexcept
{
    if (IsMediusInputSection())
    {
        MediusUI::DrawInputSectionExtras();
        IsMediusInputSection() = false;
    }
    EndBodyGroup(false);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 7.0f));
}

inline bool BeginSubsection(const char* label, bool defaultOpen = true) noexcept
{
    IM_UNUSED(defaultOpen);
    ImGui::PushID(label);
    BeginBodyGroup(true);
    return true;
}

inline void EndSubsection() noexcept
{
    EndBodyGroup(true);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 5.0f));
}
}

#endif // OVERLAY_UI_SECTIONS_H
