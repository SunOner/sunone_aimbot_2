#pragma once
#include "nodes/node_graph.h"
#include "overlay/Game_overlay.h"
#include <algorithm>
#include <cmath>

namespace GraphDrawing
{
struct Viewport
{
    double x=0,y=0,sx=1,sy=1;
    NodeGraph::Point project(NodeGraph::Point p)const{return {x+p.x*sx,y+p.y*sy};}
};
inline Viewport viewport(NodeGraph::Point canvas,int width,int height,int coordinates)
{
    if(coordinates==1)return {};
    if(coordinates==2)return {0,0,static_cast<double>(width),static_cast<double>(height)};
    const double w=std::min(canvas.x,static_cast<double>(width)),h=std::min(canvas.y,static_cast<double>(height));
    return {(width-w)*0.5,(height-h)*0.5,w/std::max(1.0,canvas.x),h/std::max(1.0,canvas.y)};
}
inline double option(const NodeGraph::Settings& style,const char* key,double fallback)
{
    if(style){const auto found=style->numbers.find(key);if(found!=style->numbers.end()&&std::isfinite(found->second))return found->second;}
    return fallback;
}
inline std::wstring wideText(const std::string& text)
{
    // Decode bounded UTF-8 without depending on the application or a platform API.
    std::wstring result;result.reserve(text.size());
    for(std::size_t i=0;i<text.size();)
    {
        const auto first=static_cast<unsigned char>(text[i++]);std::uint32_t code=first;int remaining=0;
        if(first>=0xf0){code=first&7;remaining=3;}else if(first>=0xe0){code=first&15;remaining=2;}else if(first>=0xc0){code=first&31;remaining=1;}
        bool valid=first<0x80||(first>=0xc2&&first<=0xf4);
        for(int n=0;n<remaining;++n){if(i>=text.size()||(static_cast<unsigned char>(text[i])&0xc0)!=0x80){valid=false;break;}code=(code<<6)|(static_cast<unsigned char>(text[i++])&63);}
        const std::uint32_t minimum=remaining==3?0x10000:remaining==2?0x800:remaining==1?0x80:0;
        if(!valid||code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff))code=0xfffd;
        if(sizeof(wchar_t)==2&&code>0xffff){code-=0x10000;result.push_back(static_cast<wchar_t>(0xd800+(code>>10)));result.push_back(static_cast<wchar_t>(0xdc00+(code&1023)));}
        else result.push_back(static_cast<wchar_t>(code));
    }
    return result;
}
template<class Canvas>
void render(Canvas& canvas,const NodeGraph::DrawingData& drawing,const Viewport& view,std::size_t& budget)
{
    using Kind=NodeGraph::DrawingCommand::Kind;
    for(const auto& command:drawing.commands)
    {
        if(!budget)return;
        const auto finite=[](NodeGraph::Point p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::abs(p.x)<=1e7&&std::abs(p.y)<=1e7;};
        if(!finite(command.from)||!finite(command.to))continue;
        const auto component=[&](const char* key,int fallback){return static_cast<std::uint8_t>(std::clamp(option(command.style,key,fallback),0.0,255.0));};
        const auto r=component("draw_r",90),g=component("draw_g",220),b=component("draw_b",255),a=component("draw_a",255);
        const auto color=ARGB(a,r,g,b);const float thickness=static_cast<float>(std::clamp(option(command.style,"draw_width",1.5),0.1,32.0));
        const bool filled=option(command.style,"draw_fill",0)!=0;const auto p=view.project(command.from);if(!finite(p))continue;
        const float x=static_cast<float>(p.x),y=static_cast<float>(p.y),radius=static_cast<float>(std::clamp(command.radius,0.1,10000.0));
        if(command.kind==Kind::Polyline)
        {
            if(!command.points||command.points->empty())continue;
            if(command.points->size()==1){const auto at=view.project(command.points->front());if(finite(at)){canvas.FillCircle({static_cast<float>(at.x),static_cast<float>(at.y),radius},color);--budget;}continue;}
            const auto count=command.points->size();const double fade=std::clamp(option(command.style,"draw_fade",0),0.0,1.0);
            for(std::size_t index=1;index<count+(command.closed?1:0)&&budget;++index)
            {
                const auto from=view.project((*command.points)[index-1]),to=view.project((*command.points)[index%count]);if(!finite(from)||!finite(to))continue;
                const auto alpha=static_cast<std::uint8_t>(a*(1-fade+fade*std::min(1.0,static_cast<double>(index)/(count-1))));
                canvas.AddLine({static_cast<float>(from.x),static_cast<float>(from.y),static_cast<float>(to.x),static_cast<float>(to.y)},ARGB(alpha,r,g,b),thickness);--budget;
            }
            continue;
        }
        --budget;
        if(command.kind==Kind::Point)canvas.FillCircle({x,y,radius},color);
        else if(command.kind==Kind::Circle){if(filled)canvas.FillCircle({x,y,radius},color);else canvas.AddCircle({x,y,radius},color,thickness);}
        else if(command.kind==Kind::Line){const auto to=view.project(command.to);if(finite(to))canvas.AddLine({x,y,static_cast<float>(to.x),static_cast<float>(to.y)},color,thickness);}
        else if(command.kind==Kind::Box)
        {
            const auto to=view.project({command.from.x+command.to.x,command.from.y+command.to.y});
            if(!finite(to))continue;
            const OverlayRect box{static_cast<float>(std::min(p.x,to.x)),static_cast<float>(std::min(p.y,to.y)),static_cast<float>(std::abs(to.x-p.x)),static_cast<float>(std::abs(to.y-p.y))};
            if(filled)canvas.FillRect(box,color);else canvas.AddRect(box,color,thickness);
        }
        else if(command.kind==Kind::Text&&command.text)canvas.AddText(x,y,wideText(*command.text),static_cast<float>(std::clamp(option(command.style,"draw_font_size",16),6.0,128.0)),color);
    }
}
}
