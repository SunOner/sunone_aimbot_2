#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <opencv2/imgproc.hpp>
#include "capture.h"
#include "detection_buffer.h"
#include "Game_overlay.h"
#include "mouse.h"
#include "other_tools.h"
#include "runtime/thread_loops.h"
#include "runtime/input_state.h"
#include "nodes/vision_outputs.h"
#include "nodes/vision_runtime.h"

extern std::string g_iconLastError;

namespace
{
using namespace NodeGraph;
using Clock=std::chrono::steady_clock;
struct ImageResources
{
    std::string iconPath;
    int icon=0,depth=0,mask=0;
    std::shared_ptr<const void> depthStorage,maskStorage;
    int depthColormap=-1;
    cv::Rect maskBounds;
};
struct DemoState{double distance=0;Clock::time_point previous{};};
std::map<int,ImageResources> images;
std::map<int,DemoState> demos;

int integer(const Node& node,const char* key){return static_cast<int>(parameter(node,key));}
bool flag(const Node& node,const char* key){return parameter(node,key)!=0;}
OverlayColor color(const Node& node,const char* prefix)
{
    const auto component=[&](const char* suffix){return static_cast<std::uint8_t>(std::clamp(integer(node,(std::string(prefix)+suffix).c_str()),0,255));};
    return ARGB(component("_a"),component("_r"),component("_g"),component("_b"));
}
void unload(ImageResources& resources)
{
    if(gameOverlayPtr)for(int id:{resources.icon,resources.depth,resources.mask})if(id)gameOverlayPtr->UnloadImage(id);
    resources={};
}
RECT monitorRect(int index)
{
    MONITORINFO info{};info.cbSize=sizeof(info);
    HMONITOR monitor=GetMonitorHandleByIndex(index);
    if(!monitor)monitor=MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
    if(monitor&&GetMonitorInfo(monitor,&info))return info.rcMonitor;
    return RECT{0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
}
float overlap(const cv::Rect& first,const cv::Rect& second)
{
    const auto intersection=first&second;const double sum=static_cast<double>(first.area())+second.area()-intersection.area();
    return sum>0?static_cast<float>(intersection.area()/sum):0;
}
void drawCorrectionDemo(int key,const Node& node,float centerX,float centerY)
{
    const double nearRadius=std::max(10.0,parameter(node,"near_radius")*4);
    const double snapRadius=std::clamp(parameter(node,"snap_radius")*4,6.0,std::max(6.0,nearRadius-4));
    gameOverlayPtr->AddCircle({centerX,centerY,static_cast<float>(nearRadius)},ARGB(180,80,120,255),2);
    gameOverlayPtr->AddCircle({centerX,centerY,static_cast<float>(snapRadius)},ARGB(180,255,100,100),2);
    auto& state=demos[key];const auto now=Clock::now();
    const double dt=state.previous.time_since_epoch().count()?std::clamp(std::chrono::duration<double>(now-state.previous).count(),0.0,0.1):0;
    state.previous=now;if(state.distance<=0||state.distance>nearRadius)state.distance=nearRadius;
    const double minimum=parameter(node,"min_speed"),maximum=parameter(node,"max_speed");
    const double nearUnits=std::max(0.001,parameter(node,"near_radius")),distance=state.distance/4;
    double speed=minimum;
    if(distance<parameter(node,"snap_radius"))speed*=parameter(node,"snap_boost");
    else if(distance<nearUnits)speed+=(maximum-minimum)*(1-std::pow(1-distance/nearUnits,parameter(node,"exponent")));
    else speed+=(maximum-minimum)*std::clamp(distance/nearUnits,0.0,1.0);
    const double duration=std::clamp(2.2/std::max(0.1,maximum),0.6,3.0);
    state.distance-=nearRadius/duration*speed*dt;
    gameOverlayPtr->FillCircle({centerX-static_cast<float>(state.distance),centerY,4},ARGB(255,255,255,80));
}
void updateDepthImage(const NodeVision::OverlayPacket& packet,ImageResources& resource,float baseX,float baseY,float width,float height)
{
    const Node& node=packet.style;
    if(flag(node,"depth_debug_overlay_enabled")&&packet.depth&&packet.depth->storage)
    {
        const int colormap=std::clamp(integer(node,"depth_colormap"),0,21);
        if(resource.depthStorage!=packet.depth->storage||resource.depthColormap!=colormap||!resource.depth)
        {
            cv::Mat depth=NodeVision::cpuImage(packet.depth),normalized,colored,bgra;
            if(!depth.empty())
            {
                if(depth.type()==CV_8UC1)normalized=depth;else cv::normalize(depth,normalized,0,255,cv::NORM_MINMAX,CV_8UC1);
                cv::applyColorMap(normalized,colored,colormap);
                cv::cvtColor(colored,bgra,cv::COLOR_BGR2BGRA);
                resource.depth=gameOverlayPtr->UpdateImageFromBGRA(bgra.data,bgra.cols,bgra.rows,static_cast<int>(bgra.step),resource.depth);
                resource.depthStorage=packet.depth->storage;resource.depthColormap=colormap;
            }
        }
        if(resource.depth)gameOverlayPtr->DrawImage(resource.depth,baseX,baseY,width,height,flag(node,"depth_mask_enabled")?0.3f:1.0f);
    }
    else if(resource.depth){gameOverlayPtr->UnloadImage(resource.depth);resource.depth=0;resource.depthStorage=nullptr;}
    if(flag(node,"depth_mask_enabled")&&packet.mask&&packet.mask->storage)
    {
        const auto native=std::static_pointer_cast<const NodeVision::MaskStorage>(packet.mask->storage);
        if(!native->mask.empty())
        {
            if(resource.maskStorage!=packet.mask->storage||!resource.mask)
            {
                cv::Mat bgra(native->mask.size(),CV_8UC4,cv::Scalar(0,0,0,0));bgra.setTo(cv::Scalar(20,90,255,255),native->mask);
                resource.mask=gameOverlayPtr->UpdateImageFromBGRA(bgra.data,bgra.cols,bgra.rows,static_cast<int>(bgra.step),resource.mask);
                resource.maskStorage=packet.mask->storage;
                std::vector<cv::Point> points;cv::findNonZero(native->mask,points);
                resource.maskBounds=points.empty()?cv::Rect{}:cv::boundingRect(points);
            }
            if(resource.mask)gameOverlayPtr->DrawImage(resource.mask,baseX,baseY,width,height,static_cast<float>(std::clamp(parameter(node,"depth_mask_alpha")/255.0,0.0,1.0)));
            if(!resource.maskBounds.empty())
            {
                const auto bounds=resource.maskBounds;const float sx=width/native->mask.cols,sy=height/native->mask.rows;
                gameOverlayPtr->AddRect({baseX+bounds.x*sx,baseY+bounds.y*sy,bounds.width*sx,bounds.height*sy},ARGB(230,255,240,120),1.8f);
            }
        }
    }
    else if(resource.mask){gameOverlayPtr->UnloadImage(resource.mask);resource.mask=0;resource.maskStorage=nullptr;}
}
void drawPacket(const NodeVision::OverlayPacket& packet,int screenWidth,int screenHeight)
{
    if(!packet.frame||packet.frame->width<=0||packet.frame->height<=0)return;
    const auto& node=packet.style;auto& resource=images[packet.nodeKey];
    const float width=static_cast<float>(std::min(packet.frame->width,screenWidth)),height=static_cast<float>(std::min(packet.frame->height,screenHeight));
    const float baseX=(screenWidth-width)*0.5f,baseY=(screenHeight-height)*0.5f;
    const float scaleX=width/packet.frame->width,scaleY=height/packet.frame->height;
    updateDepthImage(packet,resource,baseX,baseY,width,height);
    const auto frameColor=color(node,"game_overlay_frame"),boxColor=color(node,"game_overlay_box");
    const float frameThickness=static_cast<float>(std::max(0.1,parameter(node,"game_overlay_frame_thickness")));
    const float boxThickness=static_cast<float>(std::max(0.1,parameter(node,"game_overlay_box_thickness")));
    if(flag(node,"game_overlay_draw_frame"))gameOverlayPtr->AddRect({baseX,baseY,width,height},frameColor,frameThickness);
    if(flag(node,"circle_fov_enabled")&&flag(node,"game_overlay_draw_circle_fov"))
        gameOverlayPtr->AddCircle({baseX+width*0.5f,baseY+height*0.5f,static_cast<float>(std::min(width,height)*0.5*parameter(node,"circle_fov_radius_percent")/100)},frameColor,frameThickness);
    const auto now=Clock::now();
    const auto timestamp=packet.detections?packet.detections->timestampSeconds:packet.frame->timestampSeconds;
    const auto observedAt=Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timestamp)));
    const double age=std::clamp(std::chrono::duration<double>(now-observedAt).count(),0.0,0.35);
    const auto camera=flag(node,"game_overlay_compensate_latency")&&globalMouseThread?globalMouseThread->getMotionCompensationSince(observedAt):std::pair<double,double>{0,0};
    const auto project=[&](const Detection& detection,const TrackDebugInfo* track)->std::optional<OverlayRect>{
        const double vx=flag(node,"game_overlay_compensate_latency")&&track?track->velocityX:0;
        const double vy=flag(node,"game_overlay_compensate_latency")&&track?track->velocityY:0;
        const double left=std::clamp(detection.x+vx*age-camera.first,0.0,static_cast<double>(packet.frame->width));
        const double top=std::clamp(detection.y+vy*age-camera.second,0.0,static_cast<double>(packet.frame->height));
        const double right=std::clamp(detection.x+detection.width+vx*age-camera.first,0.0,static_cast<double>(packet.frame->width));
        const double bottom=std::clamp(detection.y+detection.height+vy*age-camera.second,0.0,static_cast<double>(packet.frame->height));
        if(right<=left||bottom<=top)return std::nullopt;
        return OverlayRect{baseX+static_cast<float>(left)*scaleX,baseY+static_cast<float>(top)*scaleY,static_cast<float>(right-left)*scaleX,static_cast<float>(bottom-top)*scaleY};
    };
    const std::string iconPath=textParameter(node,"game_overlay_icon_path");
    if(!flag(node,"game_overlay_icon_enabled")||iconPath!=resource.iconPath)
    {
        if(resource.icon)gameOverlayPtr->UnloadImage(resource.icon);resource.icon=0;resource.iconPath=iconPath;
    }
    if(flag(node,"game_overlay_icon_enabled")&&!resource.icon&&!iconPath.empty())
    {
        try{resource.icon=gameOverlayPtr->LoadImageFromFile(std::filesystem::u8path(iconPath).wstring());g_iconLastError=resource.icon?std::string():"Could not load graph overlay icon: "+iconPath;}
        catch(const std::exception& error){g_iconLastError=error.what();}
    }
    if(packet.detections)for(const auto& detection:packet.detections->objects)
    {
        const TrackDebugInfo* matching=nullptr;
        const cv::Rect box(static_cast<int>(detection.x),static_cast<int>(detection.y),static_cast<int>(detection.width),static_cast<int>(detection.height));
        for(const auto& track:packet.tracks)if(track.classId==detection.classId&&overlap(box,track.box)>=0.35f){matching=&track;break;}
        const auto rectangle=project(detection,matching);if(!rectangle)continue;
        if(flag(node,"game_overlay_draw_boxes"))
        {
            gameOverlayPtr->AddRect(*rectangle,boxColor,boxThickness);
            if(matching)
            {
                std::wstring label=L"ID "+std::to_wstring(matching->trackId);
                if(matching->isLocked||matching->trackId==packet.lockedTrackId)label+=L" *";
                if(!matching->observedThisFrame)label+=L" m"+std::to_wstring(matching->missedFrames);
                gameOverlayPtr->AddText(rectangle->x+2,std::max(baseY,rectangle->y-16),label,15,matching->isLocked?ARGB(255,255,220,70):ARGB(230,180,255,180));
            }
        }
        if(resource.icon&&flag(node,"game_overlay_icon_enabled")&&(integer(node,"game_overlay_icon_class")<0||integer(node,"game_overlay_icon_class")==detection.classId))
        {
            const float iconWidth=static_cast<float>(integer(node,"game_overlay_icon_width")),iconHeight=static_cast<float>(integer(node,"game_overlay_icon_height"));
            float x=rectangle->x+(rectangle->w-iconWidth)*0.5f,y=rectangle->y+(rectangle->h-iconHeight)*0.5f;
            const auto anchor=textParameter(node,"game_overlay_icon_anchor");
            if(anchor=="top"||anchor=="head")y=rectangle->y-iconHeight;else if(anchor=="bottom")y=rectangle->y+rectangle->h;
            x+=static_cast<float>(parameter(node,"game_overlay_icon_offset_x"));y+=static_cast<float>(parameter(node,"game_overlay_icon_offset_y"));
            gameOverlayPtr->DrawImage(resource.icon,x,y,iconWidth,iconHeight,1);
        }
    }
    if(flag(node,"game_overlay_draw_future"))
    {
        const int total=static_cast<int>(packet.futurePositions.size());
        for(int i=0;i<total;++i)
        {
            const int alpha=std::max(12,static_cast<int>(std::max(5,integer(node,"game_overlay_box_a"))*std::exp(-parameter(node,"game_overlay_future_alpha_falloff")*i/std::max(1,total))));
            const float x=baseX+static_cast<float>(packet.futurePositions[i].first)*scaleX,y=baseY+static_cast<float>(packet.futurePositions[i].second)*scaleY;
            if(x<baseX-40||y<baseY-40||x>baseX+width+40||y>baseY+height+40)continue;
            gameOverlayPtr->FillCircle({x,y,static_cast<float>(parameter(node,"game_overlay_future_point_radius"))},ARGB(static_cast<std::uint8_t>(std::clamp(alpha,0,255)),static_cast<std::uint8_t>(255-i*255/std::max(1,total)),50,static_cast<std::uint8_t>(i*255/std::max(1,total))));
        }
    }
    if(flag(node,"game_overlay_draw_wind_tail")&&packet.windTrail.size()>1)
    {
        const auto anchor=packet.windTrail.back();const auto count=packet.windTrail.size();
        for(std::size_t i=1;i<count;++i)
        {
            const auto a=packet.windTrail[i-1],b=packet.windTrail[i];
            gameOverlayPtr->AddLine({baseX+width*0.5f+static_cast<float>(a.first-anchor.first)*scaleX,baseY+height*0.5f+static_cast<float>(a.second-anchor.second)*scaleY,
                baseX+width*0.5f+static_cast<float>(b.first-anchor.first)*scaleX,baseY+height*0.5f+static_cast<float>(b.second-anchor.second)*scaleY},ARGB(static_cast<std::uint8_t>(35+190*i/count),80,210,255),1.3f);
        }
        gameOverlayPtr->FillCircle({baseX+width*0.5f,baseY+height*0.5f,3.5f},ARGB(230,120,230,255));
    }
    if(flag(node,"game_overlay_show_target_correction"))drawCorrectionDemo(packet.nodeKey,node,baseX+width*0.5f,baseY+height*0.5f);
}
}

void gameOverlayRenderLoop()
{
    RECT previous{};bool hasBounds=false;
    while(!gameOverlayShouldExit.load())
    {
        auto packets=NodeVision::overlayPackets();
        if(packets.empty()||detectionBuffer.isPaused())
        {
            if(gameOverlayPtr){for(auto& entry:images)unload(entry.second);images.clear();demos.clear();gameOverlayPtr->Stop();delete gameOverlayPtr;gameOverlayPtr=nullptr;}
            hasBounds=false;std::this_thread::sleep_for(std::chrono::milliseconds(25));continue;
        }
        int monitorIndex=0;bool exclude=true;
        {std::lock_guard<std::mutex> lock(configMutex);monitorIndex=config.monitor_idx;exclude=config.overlay_exclude_from_capture;}
        const RECT bounds=monitorRect(monitorIndex);const int width=std::max(1,static_cast<int>(bounds.right-bounds.left)),height=std::max(1,static_cast<int>(bounds.bottom-bounds.top));
        if(!gameOverlayPtr)gameOverlayPtr=new Game_overlay();
        if(!hasBounds||bounds.left!=previous.left||bounds.top!=previous.top||bounds.right!=previous.right||bounds.bottom!=previous.bottom)
        {gameOverlayPtr->SetWindowBounds(bounds.left,bounds.top,width,height);previous=bounds;hasBounds=true;}
        if(!gameOverlayPtr->IsRunning()&&!gameOverlayPtr->Start()){std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;}
        unsigned fps=1;bool unlimited=false;
        for(const auto& packet:packets){const int rate=integer(packet->style,"game_overlay_max_fps");if(rate<=0)unlimited=true;else fps=std::max(fps,static_cast<unsigned>(rate));}
        gameOverlayPtr->SetMaxFPS(unlimited?0:fps);gameOverlayPtr->SetExcludeFromCapture(exclude);
        for(auto it=images.begin();it!=images.end();)
        {if(std::none_of(packets.begin(),packets.end(),[&](const auto& packet){return packet->nodeKey==it->first;})){unload(it->second);demos.erase(it->first);it=images.erase(it);}else++it;}
        gameOverlayPtr->BeginFrame();
        for(const auto& packet:packets)try{drawPacket(*packet,width,height);}catch(const std::exception& error){g_iconLastError=error.what();}
        gameOverlayPtr->EndFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(unlimited?8:std::max(1u,1000u/fps)));
    }
    if(gameOverlayPtr){for(auto& entry:images)unload(entry.second);images.clear();demos.clear();gameOverlayPtr->Stop();delete gameOverlayPtr;gameOverlayPtr=nullptr;}
}
