#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include "imgui/imgui.h"
#include "sunone_aimbot_2.h"
#include "overlay/draw_settings.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "include/other_tools.h"
#include "kmbox_net/picture.h"

void draw_input_device()
{
    if (OverlayUI::BeginSection("Input Method", "mouse_section_input_method"))
    {
        std::vector<std::string> input_methods = { "WIN32", "GHUB", "RAZER", "ARDUINO", "RP2350", "TEENSY41", "TEENSY41_HID", "KMBOX_NET", "KMBOX_A", "MAKCU" };

        std::vector<const char*> method_items;
        method_items.reserve(input_methods.size());
        for (const auto& item : input_methods)
        {
            method_items.push_back(item.c_str());
        }

        int input_method_index = 0;
        for (size_t i = 0; i < input_methods.size(); ++i)
        {
            if (input_methods[i] == config.input_method)
            {
                input_method_index = static_cast<int>(i);
                break;
            }
        }

        if (OverlayUI::ComboRow("Mouse Input Method", &input_method_index, method_items.data(), static_cast<int>(method_items.size())))
        {
            std::string new_input_method = input_methods[input_method_index];

            if (new_input_method != config.input_method)
            {
                config.input_method = new_input_method;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
        }

        if (config.input_method == "ARDUINO" || config.input_method == "TEENSY41")
        {
            if (arduinoSerial)
            {
                if (arduinoSerial->isOpen())
                {
                    ImGui::TextColored(ImVec4(0, 255, 0, 255), config.input_method == "TEENSY41" ? "Teensy 4.1 connected" : "Arduino connected");
                }
                else
                {
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), config.input_method == "TEENSY41" ? "Teensy 4.1 not connected" : "Arduino not connected");
                }
            }

            std::vector<std::string> port_list;
            for (int i = 1; i <= 30; ++i)
            {
                port_list.push_back("COM" + std::to_string(i));
            }

            std::vector<const char*> port_items;
            port_items.reserve(port_list.size());
            for (const auto& port : port_list)
            {
                port_items.push_back(port.c_str());
            }

            int port_index = 0;
            for (size_t i = 0; i < port_list.size(); ++i)
            {
                if (port_list[i] == config.arduino_port)
                {
                    port_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow(config.input_method == "TEENSY41" ? "Teensy Port" : "Arduino Port", &port_index, port_items.data(), static_cast<int>(port_items.size())))
            {
                config.arduino_port = port_list[port_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            std::vector<int> baud_rate_list = { 9600, 19200, 38400, 57600, 115200 };
            std::vector<std::string> baud_rate_str_list;
            for (const auto& rate : baud_rate_list)
            {
                baud_rate_str_list.push_back(std::to_string(rate));
            }

            std::vector<const char*> baud_rate_items;
            baud_rate_items.reserve(baud_rate_str_list.size());
            for (const auto& rate_str : baud_rate_str_list)
            {
                baud_rate_items.push_back(rate_str.c_str());
            }

            int baud_rate_index = 0;
            for (size_t i = 0; i < baud_rate_list.size(); ++i)
            {
                if (baud_rate_list[i] == config.arduino_baudrate)
                {
                    baud_rate_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow(config.input_method == "TEENSY41" ? "Teensy Baudrate" : "Arduino Baudrate", &baud_rate_index, baud_rate_items.data(), static_cast<int>(baud_rate_items.size())))
            {
                config.arduino_baudrate = baud_rate_list[baud_rate_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            if (config.input_method == "TEENSY41")
            {
                ImGui::TextDisabled("Uses the Teensy 4.1 serial mouse bridge protocol.");
            }
            else
            {
                if (OverlayUI::CheckboxRow("Arduino 16-bit Mouse", &config.arduino_16_bit_mouse))
                {
                    OverlayConfig_MarkDirty();
                    input_method_changed.store(true);
                }
                if (OverlayUI::CheckboxRow("Arduino Enable Keys", &config.arduino_enable_keys))
                {
                    OverlayConfig_MarkDirty();
                    input_method_changed.store(true);
                }
            }
        }
        else if (config.input_method == "RP2350")
        {
            if (rp2350Serial)
            {
                if (rp2350Serial->isOpen())
                {
                    ImGui::TextColored(ImVec4(0, 255, 0, 255), "RP2350 connected");
                }
                else
                {
                    ImGui::TextColored(ImVec4(255, 0, 0, 255), "RP2350 not connected");
                }
            }

            std::vector<std::string> port_list;
            for (int i = 1; i <= 30; ++i)
            {
                port_list.push_back("COM" + std::to_string(i));
            }

            std::vector<const char*> port_items;
            port_items.reserve(port_list.size());
            for (const auto& port : port_list)
            {
                port_items.push_back(port.c_str());
            }

            int port_index = 0;
            for (size_t i = 0; i < port_list.size(); ++i)
            {
                if (port_list[i] == config.rp2350_port)
                {
                    port_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow("RP2350 Port", &port_index, port_items.data(), static_cast<int>(port_items.size())))
            {
                config.rp2350_port = port_list[port_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            std::vector<int> baud_rate_list = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
            std::vector<std::string> baud_rate_str_list;
            for (const auto& rate : baud_rate_list)
            {
                baud_rate_str_list.push_back(std::to_string(rate));
            }

            std::vector<const char*> baud_rate_items;
            baud_rate_items.reserve(baud_rate_str_list.size());
            for (const auto& rate_str : baud_rate_str_list)
            {
                baud_rate_items.push_back(rate_str.c_str());
            }

            int baud_rate_index = 0;
            for (size_t i = 0; i < baud_rate_list.size(); ++i)
            {
                if (baud_rate_list[i] == config.rp2350_baudrate)
                {
                    baud_rate_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow("RP2350 Baudrate", &baud_rate_index, baud_rate_items.data(), static_cast<int>(baud_rate_items.size())))
            {
                config.rp2350_baudrate = baud_rate_list[baud_rate_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            if (OverlayUI::CheckboxRow("RP2350 16-bit Mouse", &config.rp2350_16_bit_mouse))
            {
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
            if (OverlayUI::CheckboxRow("RP2350 Enable Keys", &config.rp2350_enable_keys))
            {
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
        }
        else if (config.input_method == "GHUB")
        {
            static const std::string ghub_version=get_ghub_version();
            if (ghub_version == "13.1.4")
            {
                std::string ghub_version_label = "The correct version of Ghub is installed: " + ghub_version;
                ImGui::Text(ghub_version_label.c_str());
            }
            else
            {
                ImGui::Text("The wrong version of Ghub is installed or the path to Ghub is not set by default.\nDefault system path: C:\\Program Files\\LGHUB");
                if (OverlayUI::ButtonRow("GHub", "Open GHub Docs", "ghub_docs"))
                {
                    ShellExecute(0, 0, L"https://github.com/SunOner/sunone_aimbot_2/blob/main/docs/guides.md#g-hub-input-method", 0, 0, SW_SHOW);
                }
            }

            ImGui::TextColored(ImVec4(255, 0, 0, 255), "Use at your own risk, the method is detected in some games.");
        }
        else if (config.input_method == "TEENSY41_HID")
        {
            bool teensy41Connected = false;
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                teensy41Connected = activeMouseInputOwner && activeMouseInputOwner->isOpen();
            }

            if (teensy41Connected)
            {
                ImGui::TextColored(ImVec4(0, 255, 0, 255), "Teensy 4.1 RawHID connected");
            }
            else
            {
                ImGui::TextColored(ImVec4(255, 0, 0, 255), "Teensy 4.1 RawHID not connected");
            }

            static char serial[64] = "";
            static char vid[16] = "";
            static char pid[16] = "";
            static std::string last_serial;
            static std::string last_vid;
            static std::string last_pid;
            static int usage_page = 0;
            static int usage_id = 0;
            static int open_index = 0;
            static int timeout_ms = 0;
            static int reconnect_ms = 0;

            if (last_serial != config.teensy_hid_serial ||
                last_vid != config.teensy_hid_vid_filter ||
                last_pid != config.teensy_hid_pid_filter ||
                usage_page != config.teensy_hid_usage_page ||
                usage_id != config.teensy_hid_usage_id ||
                open_index != config.teensy_hid_open_index ||
                timeout_ms != config.teensy_hid_packet_timeout_ms ||
                reconnect_ms != config.teensy_hid_reconnect_interval_ms)
            {
                strncpy(serial, config.teensy_hid_serial.c_str(), sizeof(serial));
                strncpy(vid, config.teensy_hid_vid_filter.c_str(), sizeof(vid));
                strncpy(pid, config.teensy_hid_pid_filter.c_str(), sizeof(pid));
                serial[sizeof(serial) - 1] = '\0';
                vid[sizeof(vid) - 1] = '\0';
                pid[sizeof(pid) - 1] = '\0';
                last_serial = config.teensy_hid_serial;
                last_vid = config.teensy_hid_vid_filter;
                last_pid = config.teensy_hid_pid_filter;
                usage_page = config.teensy_hid_usage_page;
                usage_id = config.teensy_hid_usage_id;
                open_index = config.teensy_hid_open_index;
                timeout_ms = config.teensy_hid_packet_timeout_ms;
                reconnect_ms = config.teensy_hid_reconnect_interval_ms;
            }

            OverlayUI::InputTextRow("Serial", serial, sizeof(serial));
            OverlayUI::InputTextRow("VID filter", vid, sizeof(vid));
            OverlayUI::InputTextRow("PID filter", pid, sizeof(pid));
            OverlayUI::InputIntRow("Usage Page", &usage_page);
            OverlayUI::InputIntRow("Usage ID", &usage_id);
            OverlayUI::InputIntRow("Open Index", &open_index);
            OverlayUI::InputIntRow("Packet Timeout ms", &timeout_ms);
            OverlayUI::InputIntRow("Reconnect ms", &reconnect_ms);

            if (OverlayUI::ButtonRow("Teensy HID", "Save & Reconnect", "teensy_hid_save_reconnect"))
            {
                config.teensy_hid_serial = serial;
                config.teensy_hid_vid_filter = vid;
                config.teensy_hid_pid_filter = pid;
                config.teensy_hid_usage_page = usage_page;
                config.teensy_hid_usage_id = usage_id;
                config.teensy_hid_open_index = open_index;
                config.teensy_hid_packet_timeout_ms = timeout_ms;
                config.teensy_hid_reconnect_interval_ms = reconnect_ms;
                last_serial = config.teensy_hid_serial;
                last_vid = config.teensy_hid_vid_filter;
                last_pid = config.teensy_hid_pid_filter;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }
        }
        else if (config.input_method == "RAZER")
        {
            if (razerControl && razerControl->isOpen())
            {
                ImGui::TextColored(ImVec4(0, 255, 0, 255), "Razer rzctl connected");
            }
            else
            {
                ImGui::TextColored(ImVec4(255, 0, 0, 255), "Razer rzctl not connected");
            }
            ImGui::Text("Requires rzctl.dll next to ai.exe.");
            ImGui::TextColored(ImVec4(255, 0, 0, 255), "Use at your own risk, the method is detected in some games.");
        }
        else if (config.input_method == "WIN32")
        {
            ImGui::TextColored(ImVec4(255, 255, 255, 255), "This is a standard mouse input method, it may not work in most games. Use GHUB, RAZER, ARDUINO, RP2350, TEENSY41, or TEENSY41_HID.");
            ImGui::TextColored(ImVec4(255, 0, 0, 255), "Use at your own risk, the method is detected in some games.");
        }
        else if (config.input_method == "KMBOX_NET")
        {
            static char ip[32] = "";
            static char port[8] = "";
            static char uuid[16] = "";
            static std::string last_ip;
            static std::string last_port;
            static std::string last_uuid;

            if (last_ip != config.kmbox_net_ip || last_port != config.kmbox_net_port || last_uuid != config.kmbox_net_uuid)
            {
                strncpy(ip, config.kmbox_net_ip.c_str(), sizeof(ip));
                strncpy(port, config.kmbox_net_port.c_str(), sizeof(port));
                strncpy(uuid, config.kmbox_net_uuid.c_str(), sizeof(uuid));
                ip[sizeof(ip) - 1] = '\0';
                port[sizeof(port) - 1] = '\0';
                uuid[sizeof(uuid) - 1] = '\0';
                last_ip = config.kmbox_net_ip;
                last_port = config.kmbox_net_port;
                last_uuid = config.kmbox_net_uuid;
            }

            OverlayUI::InputTextRow("IP", ip, sizeof(ip));
            OverlayUI::InputTextRow("Port", port, sizeof(port));
            OverlayUI::InputTextRow("UUID", uuid, sizeof(uuid));

            if (OverlayUI::ButtonRow("kmboxNet", "Save & Reconnect", "kmbox_net_save_reconnect"))
            {
                config.kmbox_net_ip = ip;
                config.kmbox_net_port = port;
                config.kmbox_net_uuid = uuid;
                last_ip = config.kmbox_net_ip;
                last_port = config.kmbox_net_port;
                last_uuid = config.kmbox_net_uuid;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            bool kmboxNetConnected = false;
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                KmboxNetConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_NET"
                    ? activeMouseInputOwner->kmboxNet()
                    : nullptr;
                kmboxNetConnected = device && device->isOpen();
            }

            if (kmboxNetConnected)
            {
                ImGui::TextColored(ImVec4(0, 255, 0, 255), "kmboxNet connected");
            }
            else
            {
                ImGui::TextColored(ImVec4(255, 0, 0, 255), "kmboxNet not connected");
            }

            if (!kmboxNetConnected)
                ImGui::BeginDisabled();

            if (OverlayUI::ButtonRow("kmboxNet box", "Reboot box", "kmbox_net_reboot"))
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                KmboxNetConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_NET"
                    ? activeMouseInputOwner->kmboxNet()
                    : nullptr;
                if (device && device->isOpen())
                    device->reboot();
            }

            if (OverlayUI::ButtonRow("kmboxNet image", "Change image", "kmbox_net_image"))
            {
                std::lock_guard<std::mutex> lock(inputDevicesMutex);
                KmboxNetConnection* device =
                    activeMouseInputOwner && std::string(activeMouseInputOwner->name()) == "KMBOX_NET"
                    ? activeMouseInputOwner->kmboxNet()
                    : nullptr;
                if (device && device->isOpen())
                {
                    device->lcdColor(0);
                    device->lcdPicture(gImage_128x160);
                }
            }

            if (!kmboxNetConnected)
                ImGui::EndDisabled();
        }
        else if (config.input_method == "KMBOX_A")
        {
            static char pidvid[32] = "";
            static std::string last_pidvid;

            if (last_pidvid != config.kmbox_a_pidvid)
            {
                strncpy(pidvid, config.kmbox_a_pidvid.c_str(), sizeof(pidvid));
                pidvid[sizeof(pidvid) - 1] = '\0';
                last_pidvid = config.kmbox_a_pidvid;
            }

            OverlayUI::InputTextRow("PIDVID", pidvid, sizeof(pidvid));
            ImGui::TextDisabled("Format: PPPPVVVV (one field)");

            if (OverlayUI::ButtonRow("kmboxA", "Save & Reconnect", "kmbox_a_save_reconnect"))
            {
                config.kmbox_a_pidvid = pidvid;
                last_pidvid = config.kmbox_a_pidvid;
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            if (kmboxASerial && kmboxASerial->isOpen())
            {
                ImGui::TextColored(ImVec4(0, 255, 0, 255), "kmboxA connected");
            }
            else
            {
                ImGui::TextColored(ImVec4(255, 0, 0, 255), "kmboxA not connected");
            }
        }
        else if (config.input_method == "MAKCU")
        {
            std::vector<std::string> port_list;
            for (int i = 1; i <= 30; ++i)
            {
                port_list.push_back("COM" + std::to_string(i));
            }

            std::vector<const char*> port_items;
            port_items.reserve(port_list.size());
            for (const auto& port : port_list)
            {
                port_items.push_back(port.c_str());
            }

            int port_index = 0;
            for (size_t i = 0; i < port_list.size(); ++i)
            {
                if (port_list[i] == config.makcu_port)
                {
                    port_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow("Makcu Port", &port_index, port_items.data(), static_cast<int>(port_items.size())))
            {
                config.makcu_port = port_list[port_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            std::vector<int> baud_list = { 9600, 19200, 38400, 57600, 115200 };
            std::vector<std::string> baud_str_list;
            for (int b : baud_list) baud_str_list.push_back(std::to_string(b));

            std::vector<const char*> baud_items;
            baud_items.reserve(baud_list.size());
            for (const auto& baud : baud_str_list)
            {
                baud_items.push_back(baud.c_str());
            }

            int baud_index = 0;
            for (size_t i = 0; i < baud_list.size(); ++i)
            {
                if (baud_list[i] == config.makcu_baudrate)
                {
                    baud_index = static_cast<int>(i);
                    break;
                }
            }

            if (OverlayUI::ComboRow("Makcu Baudrate", &baud_index, baud_items.data(), static_cast<int>(baud_items.size())))
            {
                config.makcu_baudrate = baud_list[baud_index];
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            if (OverlayUI::CheckboxRow("Makcu Enable Keys", &config.makcu_enable_keys))
            {
                OverlayConfig_MarkDirty();
                input_method_changed.store(true);
            }

            if (makcuSerial && makcuSerial->isOpen())
            {
                ImGui::TextColored(ImVec4(0, 255, 0, 255), "Makcu connected");
            }
            else
            {
                ImGui::TextColored(ImVec4(255, 0, 0, 255), "Makcu not connected");
            }
        }

        OverlayUI::EndSection();
    }

}
