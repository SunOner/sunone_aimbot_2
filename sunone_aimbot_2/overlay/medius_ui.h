#pragma once

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "../config/config.h"
#include "../imgui/imgui.h"
#include "../mouse/MediusRuntime.h"
#include "config_dirty.h"

extern Config config;
extern std::atomic<bool> input_method_changed;

namespace MediusUI
{
inline bool IsActive() noexcept
{
    return config.input_method == "MEDIUS";
}

inline void Select()
{
    if (config.input_method == "MEDIUS")
        return;
    config.input_method = "MEDIUS";
    OverlayConfig_MarkDirty();
    input_method_changed.store(true);
}

inline void DrawInputSectionExtras()
{
    if (!IsActive())
        return;

    MediusRuntime& runtime = MediusRuntime::instance();
    const MediusRuntime::Status status = runtime.status();

    ImGui::Separator();
    ImGui::TextUnformatted("Medius");

    static char portBuffer[32] = "AUTO";
    static std::string loadedPort;
    static std::string portMessage;
    const std::string currentPort = runtime.preferredPort();
    if (loadedPort != currentPort && !ImGui::IsAnyItemActive())
    {
        std::snprintf(portBuffer, sizeof(portBuffer), "%s", currentPort.c_str());
        loadedPort = currentPort;
    }

    ImGui::TextUnformatted("COM Port");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("##medius_port", portBuffer, sizeof(portBuffer));
    ImGui::SameLine();
    if (ImGui::Button("Apply##medius_port"))
    {
        if (runtime.setPreferredPort(portBuffer))
        {
            loadedPort = runtime.preferredPort();
            std::snprintf(portBuffer, sizeof(portBuffer), "%s", loadedPort.c_str());
            portMessage = "Port saved. Reconnecting Medius...";
            input_method_changed.store(true);
        }
        else
        {
            portMessage = "Invalid port. Use AUTO or COM1..COM4096.";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto##medius_port"))
    {
        runtime.setPreferredPort("AUTO");
        loadedPort = "AUTO";
        std::snprintf(portBuffer, sizeof(portBuffer), "%s", "AUTO");
        portMessage = "Automatic mouse-box discovery enabled.";
        input_method_changed.store(true);
    }

    if (!portMessage.empty())
        ImGui::TextDisabled("%s", portMessage.c_str());

    int baud = static_cast<int>(MediusRuntime::controlBaudRate());
    ImGui::TextUnformatted("Baud Rate");
    ImGui::SetNextItemWidth(180.0f);
    ImGui::BeginDisabled();
    ImGui::InputInt("##medius_baud", &baud, 0, 0);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("fixed by Medius C API");
    ImGui::TextDisabled("Current upstream control transport uses 4,000,000 baud; the C ABI has no custom baud parameter.");

    ImGui::Separator();
    if (status.deviceConnected)
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f), "Device: Connected");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "Device: Not connected");

    ImGui::Text("Selected port: %s", currentPort.c_str());
    ImGui::Text("Control baud: %u", MediusRuntime::controlBaudRate());

    if (status.runtimeLoaded)
        ImGui::Text("Runtime: v%s / ABI %u", status.runtimeVersion.c_str(), status.runtimeAbi);
    else
        ImGui::TextDisabled("Runtime: not loaded yet");

    const std::string supported = MediusRuntime::supportedAbiLabel();
    ImGui::Text("Supported ABI: %s", supported.c_str());

    if (status.latestKnown)
    {
        if (status.latestSupported)
        {
            ImGui::TextColored(
                ImVec4(0.35f, 0.90f, 0.45f, 1.0f),
                "Latest: v%s / ABI %u - supported",
                status.latestVersion.c_str(),
                status.latestAbi);
        }
        else
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                "Latest: v%s / ABI %u - NOT supported",
                status.latestVersion.c_str(),
                status.latestAbi);
        }
    }
    else if (status.updateChecked)
    {
        ImGui::TextDisabled("Latest release: check failed or ABI could not be probed");
    }
    else
    {
        ImGui::TextDisabled("Latest release: will be checked automatically when Medius connects");
    }

    ImGui::TextDisabled("Auto update: enabled (SHA-256 verification + ABI gate + rollback)");
    if (ImGui::Button("Check updates & reconnect##medius_update"))
    {
        runtime.requestForceUpdateCheck();
        input_method_changed.store(true);
    }

    const MediusRuntime::Status refreshed = runtime.status();
    if (!refreshed.message.empty())
        ImGui::TextWrapped("%s", refreshed.message.c_str());
}
}
