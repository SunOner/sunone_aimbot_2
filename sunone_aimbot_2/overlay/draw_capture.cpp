#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <string.h>
#include <algorithm>
#include <vector>

#include <imgui/imgui.h>
#include "imgui/imgui_internal.h"

#include "config.h"
#include "sunone_aimbot_2.h"
#include "capture.h"
#include "other_tools.h"
#include "virtual_camera.h"
#include "draw_settings.h"
#include "overlay.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"

bool disable_winrt_futures = checkwin1903();
int monitors = get_active_monitors();

static std::vector<std::string> virtual_cameras;
static std::vector<CaptureWindowInfo> capture_windows;
static char virtual_camera_filter_buf[128] = "";
static char capture_window_filter_buf[128] = "";
static char udp_ip_buf[64] = "";
static int udp_port_buf = 1234;
static bool capture_windows_loaded = false;
static bool udp_settings_init = false;

static void requestWinrtCaptureRestart()
{
    capture_method_changed.store(true);
    capture_window_changed.store(true);
}

static void refreshCaptureWindowList()
{
    capture_windows = EnumerateCaptureWindows();
    capture_windows_loaded = true;
}

static bool captureWindowMatchesTitle(const CaptureWindowInfo& window, const std::string& title)
{
    const std::string needle = OtherTools::TrimAscii(title);
    if (needle.empty())
        return false;

    return window.title == needle ||
        window.title.find(needle) != std::string::npos ||
        OtherTools::ContainsCaseInsensitive(window.title, needle);
}

static bool currentWindowTitleIsInList()
{
    for (const auto& window : capture_windows)
        if (captureWindowMatchesTitle(window, config.capture_window_title))
            return true;
    return false;
}

static void applyWinrtWindowTarget(const std::string& title)
{
    if (config.capture_window_title != title)
    {
        config.capture_window_title = title;
        OverlayConfig_MarkDirty();
    }

    requestWinrtCaptureRestart();
}

void ensureVirtualCamerasLoaded()
{
    if (virtual_cameras.empty())
    {
        virtual_cameras = VirtualCameraCapture::GetAvailableVirtualCameras();
    }
}

void draw_capture_settings()
{
    static const int allowed_resolutions[] = { 160, 320, 640 };
    static int current_resolution_idx = 1;

    for (int i = 0; i < 3; ++i)
        if (config.detection_resolution == allowed_resolutions[i])
            current_resolution_idx = i;

    if (OverlayUI::BeginSection("General Capture", "capture_section_general"))
    {
        {
            const auto row = OverlayUI::BeginSettingRow("Detection Resolution");
            if (ImGui::Combo("##value", &current_resolution_idx, "160\0""320\0""640\0"))
            {
                config.detection_resolution = allowed_resolutions[current_resolution_idx];
                detection_resolution_changed.store(true);
                detector_model_changed.store(true);

                globalMouseThread->updateConfig(config);
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        {
            const auto row = OverlayUI::BeginSettingRow("Capture FPS");
            if (ImGui::SliderInt("##value", &config.capture_fps, 0, 360))
            {
                capture_fps_changed.store(true);
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        if (config.capture_fps == 0)
        {
            OverlayUI::TextRow("Capture FPS cap is disabled.", IM_COL32(255, 188, 108, 255));
        }
        else if (config.capture_fps >= 61)
        {
            OverlayUI::TextRow("WARNING: High FPS can reduce performance.");
        }

        {
            const auto row = OverlayUI::BeginSettingRow("Circle FOV");
            if (ImGui::Checkbox("##value", &config.circle_fov_enabled))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

        if (config.circle_fov_enabled)
        {
            const auto row = OverlayUI::BeginSettingRow("Circle FOV size");
            if (ImGui::SliderInt("##value", &config.circle_fov_radius_percent, 1, 100, "%d%%"))
            {
                OverlayConfig_MarkDirty();
            }
            OverlayUI::EndSettingRow(row);
        }

#ifdef USE_CUDA
        if (config.backend == "TRT")
        {
            const bool cudaCaptureAvailable = (config.capture_method == "duplication_api");
            if (!cudaCaptureAvailable)
            {
                ImGui::BeginDisabled();
            }

            {
                const auto row = OverlayUI::BeginSettingRow("Use CUDA Direct Capture");
                if (ImGui::Checkbox("##value", &config.capture_use_cuda))
                {
                    OverlayConfig_MarkDirty();
                }
                OverlayUI::EndSettingRow(row);
            }

            if (!cudaCaptureAvailable)
            {
                ImGui::EndDisabled();
                OverlayUI::TextRow("Available only with duplication_api.", IM_COL32(188, 188, 188, 255));
            }
        }
#endif

        std::vector<std::string> captureMethodOptions = { "duplication_api", "winrt", "virtual_camera", "udp_capture" };
        std::vector<const char*> captureMethodItems;

        for (const auto& option : captureMethodOptions)
        {
            captureMethodItems.push_back(option.c_str());
        }

        int currentcaptureMethodIndex = 0;
        for (size_t i = 0; i < captureMethodOptions.size(); ++i)
        {
            if (captureMethodOptions[i] == config.capture_method)
            {
                currentcaptureMethodIndex = static_cast<int>(i);
                break;
            }
        }

        {
            const auto row = OverlayUI::BeginSettingRow("Capture method");
            if (ImGui::Combo("##value", &currentcaptureMethodIndex, captureMethodItems.data(), static_cast<int>(captureMethodItems.size())))
            {
                config.capture_method = captureMethodOptions[currentcaptureMethodIndex];
                OverlayConfig_MarkDirty();
                capture_method_changed.store(true);
            }
            OverlayUI::EndSettingRow(row);
        }

        OverlayUI::EndSection();
    }

    draw_capture_preview();

    if (config.capture_method == "winrt")
    {
        if (OverlayUI::BeginSection("WinRT", "capture_section_winrt"))
        {
            {
                std::vector<std::string> targetOptions = { "monitor", "window" };
                int currentTargetIndex = (config.capture_target == "window") ? 1 : 0;
                {
                    const auto row = OverlayUI::BeginSettingRow("Capture target (WinRT)");
                    if (ImGui::Combo("##value", &currentTargetIndex,
                        [](void* data, int idx) -> const char* {
                            const auto* v = static_cast<const std::vector<std::string>*>(data);
                            if (idx < 0 || idx >= (int)v->size()) return nullptr;
                            return v->at(idx).c_str();
                        }, (void*)&targetOptions, (int)targetOptions.size()))
                    {
                        config.capture_target = targetOptions[currentTargetIndex];
                        OverlayConfig_MarkDirty();
                        requestWinrtCaptureRestart();
                    }
                    OverlayUI::EndSettingRow(row);
                }
            }

            if (config.capture_target == "window")
            {
                if (!capture_windows_loaded)
                    refreshCaptureWindowList();

                {
                    const auto row = OverlayUI::BeginSettingRow("Window filter");
                    ImGui::InputText("##value", capture_window_filter_buf, IM_ARRAYSIZE(capture_window_filter_buf));
                    OverlayUI::EndSettingRow(row);
                }

                const std::string filterLower = OtherTools::ToLowerAscii(capture_window_filter_buf);
                std::vector<int> filteredWindowIndices;
                filteredWindowIndices.reserve(capture_windows.size());
                for (int i = 0; i < static_cast<int>(capture_windows.size()); ++i)
                {
                    const std::string displayLower = OtherTools::ToLowerAscii(capture_windows[i].displayName);
                    if (filterLower.empty() || displayLower.find(filterLower) != std::string::npos)
                        filteredWindowIndices.push_back(i);
                }

                if (!filteredWindowIndices.empty())
                {
                    const std::string preview = config.capture_window_title.empty()
                        ? std::string("Select window")
                        : config.capture_window_title;

                    const auto row = OverlayUI::BeginSettingRow("Window");
                    if (ImGui::BeginCombo("##value", preview.c_str()))
                    {
                        for (int index : filteredWindowIndices)
                        {
                            const CaptureWindowInfo& window = capture_windows[index];
                            const bool selected = captureWindowMatchesTitle(window, config.capture_window_title);

                            ImGui::PushID(window.hwnd);
                            if (ImGui::Selectable(window.displayName.c_str(), selected))
                            {
                                applyWinrtWindowTarget(window.title);
                            }
                            if (selected)
                                ImGui::SetItemDefaultFocus();
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    OverlayUI::EndSettingRow(row);
                }
                else
                {
                    OverlayUI::TextRow("No matching windows", IM_COL32(188, 188, 188, 255));
                }

                {
                    const auto row = OverlayUI::BeginSettingRow("Window list");
                    if (ImGui::Button("Refresh", ImVec2(row.controlWidth, 0.0f)))
                    {
                        refreshCaptureWindowList();
                        if (currentWindowTitleIsInList())
                            requestWinrtCaptureRestart();
                    }
                    OverlayUI::EndSettingRow(row);
                }
            }

            if (disable_winrt_futures)
            {
                ImGui::BeginDisabled();
            }

            {
                const auto row = OverlayUI::BeginSettingRow("Capture Borders");
                if (ImGui::Checkbox("##value", &config.capture_borders))
                {
                    capture_borders_changed.store(true);
                    OverlayConfig_MarkDirty();
                }
                OverlayUI::EndSettingRow(row);
            }

            {
                const auto row = OverlayUI::BeginSettingRow("Capture Cursor");
                if (ImGui::Checkbox("##value", &config.capture_cursor))
                {
                    capture_cursor_changed.store(true);
                    OverlayConfig_MarkDirty();
                }
                OverlayUI::EndSettingRow(row);
            }

            if (disable_winrt_futures)
            {
                ImGui::EndDisabled();
            }

            OverlayUI::EndSection();
        }
    }

    if (config.capture_method == "duplication_api" || (config.capture_method == "winrt" && config.capture_target != "window"))
    {
        if (OverlayUI::BeginSection("Monitor Capture", "capture_section_monitor"))
        {
            std::vector<std::string> monitorNames;
            int monitorCount = monitors;
            if (monitorCount <= 0)
            {
                monitorNames.push_back("Monitor 1");
                monitorCount = 1;
            }
            else
            {
                for (int i = 0; i < monitorCount; ++i)
                {
                    monitorNames.push_back("Monitor " + std::to_string(i + 1));
                }
            }

            std::vector<const char*> monitorItems;
            for (const auto& name : monitorNames)
            {
                monitorItems.push_back(name.c_str());
            }

            int selectedMonitor = std::clamp(config.monitor_idx, 0, monitorCount - 1);
            {
                const auto row = OverlayUI::BeginSettingRow("Capture monitor");
                if (ImGui::Combo("##value", &selectedMonitor, monitorItems.data(), static_cast<int>(monitorItems.size())))
                {
                    config.monitor_idx = selectedMonitor;
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }

            OverlayUI::EndSection();
        }
    }

    if (config.capture_method == "virtual_camera")
    {
        if (OverlayUI::BeginSection("Virtual Camera", "capture_section_virtual_camera"))
        {
            ensureVirtualCamerasLoaded();

            {
                const auto row = OverlayUI::BeginSettingRow("Filter");
                ImGui::InputText("##value", virtual_camera_filter_buf, IM_ARRAYSIZE(virtual_camera_filter_buf));
                OverlayUI::EndSettingRow(row);
            }

            std::string filter_lower = OtherTools::ToLowerAscii(virtual_camera_filter_buf);

            std::vector<int> filtered_indices;
            for (int i = 0; i < static_cast<int>(virtual_cameras.size()); ++i)
            {
                std::string name_lower = OtherTools::ToLowerAscii(virtual_cameras[i]);
                if (filter_lower.empty() || name_lower.find(filter_lower) != std::string::npos)
                {
                    filtered_indices.push_back(i);
                }
            }

            if (!filtered_indices.empty())
            {
                int currentIndex = 0;
                for (int fi = 0; fi < static_cast<int>(filtered_indices.size()); ++fi)
                {
                    if (virtual_cameras[filtered_indices[fi]] == config.virtual_camera_name)
                    {
                        currentIndex = fi;
                        break;
                    }
                }

                std::vector<const char*> items;
                items.reserve(filtered_indices.size());
                for (int idx : filtered_indices)
                {
                    items.push_back(virtual_cameras[idx].c_str());
                }

                {
                    const auto row = OverlayUI::BeginSettingRow("Virtual camera");
                    if (ImGui::Combo("##value", &currentIndex, items.data(), static_cast<int>(items.size())))
                    {
                        config.virtual_camera_name = virtual_cameras[filtered_indices[currentIndex]];
                        OverlayConfig_MarkDirty();
                        capture_method_changed.store(true);
                    }
                    OverlayUI::EndSettingRow(row);
                }
            }
            else
            {
                OverlayUI::TextRow("No matching virtual cameras", IM_COL32(188, 188, 188, 255));
            }

            {
                const auto row = OverlayUI::BeginSettingRow("Camera list");
                if (ImGui::Button("Refresh", ImVec2(row.controlWidth, 0.0f)))
                {
                    VirtualCameraCapture::ClearCachedCameraList();
                    virtual_cameras = VirtualCameraCapture::GetAvailableVirtualCameras(true);
                    virtual_camera_filter_buf[0] = '\0';
                }
                OverlayUI::EndSettingRow(row);
            }

            {
                const auto row = OverlayUI::BeginSettingRow("Virtual camera width");
                if (ImGui::SliderInt("##value", &config.virtual_camera_width, 128, 3840))
                {
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }

            {
                const auto row = OverlayUI::BeginSettingRow("Virtual camera height");
                if (ImGui::SliderInt("##value", &config.virtual_camera_heigth, 128, 2160))
                {
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }

            OverlayUI::EndSection();
        }
    }

    if (config.capture_method == "udp_capture")
    {
        if (OverlayUI::BeginSection("UDP Capture", "capture_section_udp"))
        {
            if (!udp_settings_init)
            {
                memset(udp_ip_buf, 0, sizeof(udp_ip_buf));
                std::string ip = config.udp_ip;
                if (ip.size() >= sizeof(udp_ip_buf))
                    ip = ip.substr(0, sizeof(udp_ip_buf) - 1);
                memcpy(udp_ip_buf, ip.c_str(), ip.size());
                udp_port_buf = config.udp_port;
                udp_settings_init = true;
            }

            {
                const auto row = OverlayUI::BeginSettingRow("UDP IP");
                ImGui::InputText("##value", udp_ip_buf, IM_ARRAYSIZE(udp_ip_buf));
                OverlayUI::EndSettingRow(row);
            }
            {
                const auto row = OverlayUI::BeginSettingRow("UDP Port");
                ImGui::InputInt("##value", &udp_port_buf);
                OverlayUI::EndSettingRow(row);
            }
            {
                const auto row = OverlayUI::BeginSettingRow("UDP settings");
                if (ImGui::Button("Apply UDP Settings", ImVec2(row.controlWidth, 0.0f)))
                {
                    udp_port_buf = std::clamp(udp_port_buf, 1, 65535);
                    config.udp_ip = udp_ip_buf;
                    config.udp_port = udp_port_buf;
                    OverlayConfig_MarkDirty();
                    capture_method_changed.store(true);
                }
                OverlayUI::EndSettingRow(row);
            }

            OverlayUI::EndSection();
        }
    }
}
