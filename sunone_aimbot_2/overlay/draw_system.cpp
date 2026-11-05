#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include "imgui/imgui.h"
#include "overlay/overlay.h"
#include "overlay/draw_settings.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include "overlay/node_editor.h"
#include "mcp/workflow_host.h"
#include <algorithm>
#include <thread>
void draw_system()
{
    if(OverlayUI::BeginSection("Agent access","agent_access"))
    {
        auto connection=WorkflowMcp::status();
        if(OverlayUI::CheckboxRow("Local MCP",&config.local_mcp_enabled))OverlayConfig_MarkDirty();
        ImGui::TextWrapped("%s",connection.message.c_str());
        if(connection.running)
        {
            ImGui::TextDisabled("Requests handled: %llu",connection.calls);
            static int client=0;
            const char* clients[]={"Claude Code","Claude Desktop","Codex","Other MCP client"};
            OverlayUI::ComboRow("Agent",&client,clients,4);
            const auto selected=static_cast<WorkflowMcp::Client>(client);
            if(client==0||client==2)
            {
                if(ImGui::Button("Copy install command")){const auto command=WorkflowMcp::clientConfiguration(selected,true);ImGui::SetClipboardText(command.c_str());}
                ImGui::SameLine();
            }
            if(ImGui::Button(client==2?"Copy TOML":"Copy MCP config")){const auto configuration=WorkflowMcp::clientConfiguration(selected);ImGui::SetClipboardText(configuration.c_str());}
            if(client==0)ImGui::TextWrapped("Paste the command in PowerShell, then reconnect the MCP server in Claude Code. JSON is also available for .mcp.json.");
            else if(client==1)ImGui::TextWrapped("Merge mcpServers into Claude Desktop's Developer > Edit Config, then restart Claude. Keep your other server entries.");
            else if(client==2)ImGui::TextWrapped("Paste the command in PowerShell, or add the TOML to your Codex config.toml. Then restart the MCP connection.");
        }
        ImGui::TextWrapped("Local agents can edit workflows with Undo and autosave. Python 3.10+ is required by the connector.");
        OverlayUI::EndSection();
    }
    ImGui::TextWrapped("Resource reservations apply on the next application launch.");
    if(OverlayUI::BeginSection("Startup resources","system_resources"))
    {
        const auto integer=[](const char* label,int& value,int maximum){
            if(OverlayUI::InputIntRow(label,&value,1,64)){value=std::clamp(value,0,maximum);OverlayConfig_MarkDirty();}
        };
        integer("CPU cores reserved for the system",config.cpuCoreReserveCount,std::max(0,static_cast<int>(std::thread::hardware_concurrency())-1));
        MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
        const int maximum=GlobalMemoryStatusEx(&memory)?static_cast<int>(memory.ullTotalPhys/(1024*1024)):1048576;
        integer("Reserved system memory (MB)",config.systemMemoryReserveMB,maximum);
#ifdef USE_CUDA
        integer("Reserved GPU memory (MB)",config.gpuMemoryReserveMB,65536);
        if(OverlayUI::CheckboxRow("GPU exclusive mode",&config.enableGpuExclusiveMode))OverlayConfig_MarkDirty();
#endif
        ImGui::TextDisabled("0 disables the corresponding reservation.");OverlayUI::EndSection();
    }
}
