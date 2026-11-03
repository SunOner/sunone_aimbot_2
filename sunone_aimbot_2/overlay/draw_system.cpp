#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include "imgui/imgui.h"
#include "overlay/overlay.h"
#include "overlay/draw_settings.h"
#include "overlay/config_dirty.h"
#include "overlay/ui_sections.h"
#include <algorithm>
#include <thread>
void draw_system()
{
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
