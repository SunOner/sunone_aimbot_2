#pragma once

#ifdef USE_CUDA

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "imgui/imgui.h"
#include "tensorrt/trt_monitor.h"

namespace OverlayExportUI
{
struct PhaseSnapshot
{
    std::string name;
    int current = 0;
    int max = 0;
};

struct ProgressSnapshot
{
    std::vector<PhaseSnapshot> phases;
    float aggregate = 0.0f;
    int currentSteps = 0;
    int maxSteps = 0;
    double secondsSinceUpdate = 0.0;
    bool hasPhases = false;
    bool stale = false;
    bool cancelRequested = false;
};

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

inline std::string FitTextToWidth(const std::string& text, float maxWidth)
{
    if (maxWidth <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
    {
        return text;
    }

    static constexpr const char* suffix = "...";
    const float suffixWidth = ImGui::CalcTextSize(suffix).x;
    if (suffixWidth >= maxWidth)
    {
        return suffix;
    }

    std::string out = text;
    while (!out.empty())
    {
        out.pop_back();
        std::string candidate = out + suffix;
        if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth)
        {
            return candidate;
        }
    }
    return suffix;
}

inline ProgressSnapshot CaptureProgress()
{
    ProgressSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(gProgressMutex);
        snapshot.phases.reserve(gProgressPhases.size());
        for (const auto& [name, phase] : gProgressPhases)
        {
            PhaseSnapshot item;
            item.name = name;
            item.current = std::max(0, phase.current);
            item.max = std::max(0, phase.max);
            snapshot.currentSteps += item.current;
            snapshot.maxSteps += item.max;
            snapshot.phases.push_back(std::move(item));
        }
    }

    snapshot.hasPhases = !snapshot.phases.empty();
    snapshot.aggregate = snapshot.maxSteps > 0
        ? std::clamp(snapshot.currentSteps / static_cast<float>(snapshot.maxSteps), 0.0f, 1.0f)
        : 0.0f;

    const long long lastUpdate = gTrtExportLastUpdateMs.load();
    if (lastUpdate > 0)
    {
        snapshot.secondsSinceUpdate = (TrtNowMs() - lastUpdate) / 1000.0;
    }
    snapshot.stale = snapshot.secondsSinceUpdate > 45.0;
    snapshot.cancelRequested = gTrtExportCancelRequested.load();
    return snapshot;
}

inline void DrawActivityDots(ImDrawList* draw, const ImVec2& center, float time)
{
    for (int i = 0; i < 8; ++i)
    {
        const float angle = time * 4.2f + i * 0.785398f;
        const float alphaT = (std::sin(time * 4.2f - i * 0.65f) + 1.0f) * 0.5f;
        const int alpha = static_cast<int>(96.0f + alphaT * 150.0f);
        const ImVec2 p(center.x + std::cos(angle) * 11.0f, center.y + std::sin(angle) * 11.0f);
        draw->AddCircleFilled(p, 2.0f, IM_COL32(96, 205, 255, alpha), 10);
    }
}

inline void DrawProgressBar(ImDrawList* draw, const ImVec2& pos, const ImVec2& size,
    float fraction, bool indeterminate, ImU32 accent)
{
    const ImVec2 end(pos.x + size.x, pos.y + size.y);
    draw->AddRectFilled(pos, end, IM_COL32(52, 54, 60, 245), 4.0f);
    draw->AddRect(pos, end, IM_COL32(255, 255, 255, 24), 4.0f, 0, 1.0f);

    if (indeterminate)
    {
        const float travel = size.x + 80.0f;
        const float x = pos.x - 72.0f + std::fmod(static_cast<float>(ImGui::GetTime()) * 118.0f, travel);
        const ImVec2 fillMin(std::clamp(x, pos.x, end.x), pos.y);
        const ImVec2 fillMax(std::clamp(x + 72.0f, pos.x, end.x), end.y);
        if (fillMax.x > fillMin.x)
        {
            draw->AddRectFilled(fillMin, fillMax, accent, 4.0f);
        }
        return;
    }

    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction <= 0.0f)
    {
        return;
    }

    const ImVec2 fillEnd(pos.x + std::max(size.y, size.x * fraction), end.y);
    draw->AddRectFilled(pos, ImVec2(std::min(fillEnd.x, end.x), fillEnd.y), accent, 4.0f);
    if (fraction > 0.08f)
    {
        draw->AddRectFilled(pos, ImVec2(std::min(pos.x + size.x * fraction, end.x), pos.y + 2.0f),
            IM_COL32(255, 255, 255, 38), 4.0f);
    }
}

inline void DrawPhaseRows(ImDrawList* draw, const ProgressSnapshot& snapshot,
    const ImVec2& origin, float width, int maxRows, float& y)
{
    const ImU32 phaseAccent = snapshot.stale ? IM_COL32(252, 225, 115, 255) : IM_COL32(96, 205, 255, 255);
    const int count = static_cast<int>(std::min<size_t>(snapshot.phases.size(), static_cast<size_t>(maxRows)));
    for (int i = 0; i < count; ++i)
    {
        const PhaseSnapshot& phase = snapshot.phases[i];
        const float fraction = phase.max > 0
            ? std::clamp(phase.current / static_cast<float>(phase.max), 0.0f, 1.0f)
            : 0.0f;

        char value[32] = {};
        if (phase.max > 0)
        {
            std::snprintf(value, sizeof(value), "%d/%d", phase.current, phase.max);
        }
        else
        {
            std::snprintf(value, sizeof(value), "%d", phase.current);
        }

        const float valueW = ImGui::CalcTextSize(value).x;
        const std::string name = FitTextToWidth(phase.name, width - valueW - 16.0f);
        draw->AddText(ImVec2(origin.x, y), IM_COL32(236, 236, 240, 245), name.c_str());
        draw->AddText(ImVec2(origin.x + width - valueW, y), IM_COL32(186, 190, 198, 235), value);
        y += ImGui::GetTextLineHeight() + 7.0f;

        DrawProgressBar(draw, ImVec2(origin.x, y), ImVec2(width, 7.0f), fraction, phase.max <= 0, phaseAccent);
        y += 17.0f;
    }

    if (static_cast<int>(snapshot.phases.size()) > maxRows)
    {
        const int hidden = static_cast<int>(snapshot.phases.size()) - maxRows;
        char text[64] = {};
        std::snprintf(text, sizeof(text), "%d more export phases running", hidden);
        draw->AddText(ImVec2(origin.x, y), IM_COL32(172, 176, 184, 230), text);
        y += ImGui::GetTextLineHeight() + 4.0f;
    }
}

inline void DrawTensorRtExportPanel(const char* id, const char* title,
    const char* subtitle, const char* modelName, const char* cancelLabel,void (*cancel)()=nullptr)
{
    ProgressSnapshot snapshot = CaptureProgress();

    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::GetContentRegionAvail().x;
    const float pad = 16.0f;
    const int phaseRows = snapshot.hasPhases ? static_cast<int>(std::min<size_t>(snapshot.phases.size(), 5)) : 1;
    const float warningH = snapshot.stale ? 28.0f : 0.0f;
    const float cardH = 184.0f + warningH + phaseRows * 40.0f +
        (snapshot.phases.size() > static_cast<size_t>(phaseRows) ? 22.0f : 0.0f);
    const ImVec2 cardMin = ImGui::GetCursorScreenPos();
    const ImVec2 cardMax(cardMin.x + width, cardMin.y + cardH);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    draw->AddRectFilled(cardMin, cardMax, IM_COL32(35, 37, 42, 246), 8.0f);
    draw->AddRectFilledMultiColor(
        ImVec2(cardMin.x + 1.0f, cardMin.y + 1.0f),
        ImVec2(cardMax.x - 1.0f, cardMin.y + 72.0f),
        IM_COL32(48, 55, 64, 160), IM_COL32(38, 43, 51, 70),
        IM_COL32(35, 37, 42, 0), IM_COL32(35, 37, 42, 0));
    draw->AddRect(cardMin, cardMax, IM_COL32(255, 255, 255, 36), 8.0f, 0, 1.0f);

    const ImU32 accent = snapshot.stale ? IM_COL32(252, 225, 115, 255) : IM_COL32(96, 205, 255, 255);
    DrawActivityDots(draw, ImVec2(cardMin.x + pad + 15.0f, cardMin.y + 30.0f), static_cast<float>(ImGui::GetTime()));

    const float textX = cardMin.x + pad + 42.0f;
    draw->AddText(ImVec2(textX, cardMin.y + 15.0f), IM_COL32(246, 247, 250, 255), title ? title : "TensorRT export");

    std::string detail;
    if (modelName && *modelName)
    {
        detail = modelName;
    }
    else if (subtitle && *subtitle)
    {
        detail = subtitle;
    }
    else
    {
        detail = "Building optimized engine";
    }
    detail = FitTextToWidth(detail, width - 210.0f);
    draw->AddText(ImVec2(textX, cardMin.y + 39.0f), IM_COL32(184, 190, 200, 238), detail.c_str());

    const char* stateText = snapshot.cancelRequested ? "Canceling" : (snapshot.stale ? "Working" : "Running");
    const ImVec2 stateSize = ImGui::CalcTextSize(stateText);
    const ImVec2 pillMin(cardMax.x - pad - stateSize.x - 22.0f, cardMin.y + 18.0f);
    const ImVec2 pillMax(cardMax.x - pad, cardMin.y + 45.0f);
    draw->AddRectFilled(pillMin, pillMax,
        snapshot.cancelRequested ? IM_COL32(88, 58, 58, 245) : IM_COL32(42, 55, 62, 245), 7.0f);
    draw->AddRect(pillMin, pillMax, accent, 7.0f, 0, 1.0f);
    draw->AddText(ImVec2(pillMin.x + 11.0f, pillMin.y + 6.0f), IM_COL32(244, 246, 248, 255), stateText);

    float y = cardMin.y + 76.0f;
    const float innerX = cardMin.x + pad;
    const float innerW = width - pad * 2.0f;
    char totalLabel[48] = {};
    if (snapshot.hasPhases && snapshot.maxSteps > 0)
    {
        std::snprintf(totalLabel, sizeof(totalLabel), "%d%%", static_cast<int>(std::round(snapshot.aggregate * 100.0f)));
    }
    else
    {
        std::snprintf(totalLabel, sizeof(totalLabel), "Preparing");
    }
    draw->AddText(ImVec2(innerX, y), IM_COL32(236, 236, 240, 245), "Overall progress");
    const float totalLabelW = ImGui::CalcTextSize(totalLabel).x;
    draw->AddText(ImVec2(innerX + innerW - totalLabelW, y), IM_COL32(186, 190, 198, 235), totalLabel);
    y += ImGui::GetTextLineHeight() + 8.0f;
    DrawProgressBar(draw, ImVec2(innerX, y), ImVec2(innerW, 10.0f), snapshot.aggregate, !snapshot.hasPhases, accent);
    y += 25.0f;

    if (snapshot.stale)
    {
        const char* warning = "TensorRT may spend a long time selecting tactics without reporting new steps.";
        const std::string fittedWarning = FitTextToWidth(warning, innerW - 20.0f);
        draw->AddRectFilled(ImVec2(innerX, y), ImVec2(innerX + innerW, y + 23.0f), IM_COL32(70, 60, 36, 210), 5.0f);
        draw->AddText(ImVec2(innerX + 10.0f, y + 4.0f), IM_COL32(252, 225, 115, 255),
            fittedWarning.c_str());
        y += 31.0f;
    }

    if (snapshot.hasPhases)
    {
        DrawPhaseRows(draw, snapshot, ImVec2(innerX, y), innerW, 5, y);
    }
    else
    {
        draw->AddText(ImVec2(innerX, y), IM_COL32(186, 190, 198, 235),
            subtitle && *subtitle ? subtitle : "Waiting for TensorRT to report build phases");
        y += 30.0f;
    }

    const float buttonW = std::min(190.0f, std::max(132.0f, innerW * 0.36f));
    ImGui::SetCursorScreenPos(ImVec2(innerX, cardMax.y - pad - ImGui::GetFrameHeight()));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, snapshot.cancelRequested ? IM_COL32(64, 64, 68, 255) : IM_COL32(72, 74, 80, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(92, 60, 62, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(114, 64, 66, 255));
    if (snapshot.cancelRequested)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(cancelLabel ? cancelLabel : "Cancel export", ImVec2(buttonW, 0.0f)))
    {
        if(cancel)cancel();else gTrtExportCancelRequested = true;
    }
    if (snapshot.cancelRequested)
    {
        ImGui::EndDisabled();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    char updated[64] = {};
    std::snprintf(updated, sizeof(updated), "Last update %.1f s ago", snapshot.secondsSinceUpdate);
    const float updatedW = ImGui::CalcTextSize(updated).x;
    draw->AddText(ImVec2(cardMax.x - pad - updatedW, cardMax.y - pad - ImGui::GetTextLineHeight() - 4.0f),
        snapshot.stale ? IM_COL32(252, 225, 115, 255) : IM_COL32(156, 162, 172, 230), updated);

    ImGui::SetCursorScreenPos(ImVec2(cardMin.x, cardMax.y + style.ItemSpacing.y));
    ImGui::Dummy(ImVec2(width, 1.0f));
    ImGui::PopID();
}
}

#endif
