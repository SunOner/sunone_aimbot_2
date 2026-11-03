#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include "overlay/source_choices.h"
#include "other_tools.h"
#include "virtual_camera.h"
#include <algorithm>

namespace SourceChoices
{
std::vector<Choice> enumerate(bool camera)
{
    std::vector<Choice> choices;
    if(camera)for(const auto& name:VirtualCameraCapture::GetAvailableVirtualCameras(true))choices.push_back({name,name});
    else for(const auto& window:EnumerateCaptureWindows())choices.push_back({window.displayName,window.title});
    std::sort(choices.begin(),choices.end(),[](const auto& a,const auto& b){return a.label<b.label;});return choices;
}
}
