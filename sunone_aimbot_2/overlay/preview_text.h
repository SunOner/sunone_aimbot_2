#pragma once
#include "nodes/node_graph.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include <algorithm>
#include <cmath>

namespace NodePreview
{
struct TextLayout
{
    NodeGraph::Preview content;
    ImFont* font=nullptr;
    float fontSize=0,width=0,scroll=0;
    std::vector<std::pair<std::size_t,std::size_t>> lines;
    std::uint64_t builds=0;
    void prepare(const NodeGraph::Preview& next,ImFont* nextFont,float size,float wrap)
    {
        if(content==next&&font==nextFont&&fontSize==size&&width==wrap)return;
        content=next;font=nextFont;fontSize=size;width=wrap;lines.clear();++builds;
        if(!content)return;
        const auto& text=content->text;const char* begin=text.data();const char* end=begin+text.size();const char* cursor=begin;
        while(cursor<end)
        {
            const char* newline=std::find(cursor,end,'\n');const char* stop=newline;
            if(stop>cursor&&stop[-1]=='\r')--stop;
            if(cursor==stop)lines.emplace_back(cursor-begin,cursor-begin);
            while(cursor<stop)
            {
                const char* last=font->CalcWordWrapPosition(fontSize,cursor,stop,width);
                if(last<=cursor){unsigned int code=0;last=cursor+std::max(1,ImTextCharFromUtf8(&code,cursor,stop));}
                lines.emplace_back(cursor-begin,last-begin);cursor=last;
                if(cursor<stop)while(cursor<stop&&(*cursor==' '||*cursor=='\t'))++cursor;
            }
            cursor=newline<end?newline+1:end;
        }
        if(text.empty()||text.back()=='\n')lines.emplace_back(text.size(),text.size());
    }
    void draw(ImDrawList* draw,const ImRect& panel,float zoom,bool wheel)
    {
        if(!content)return;
        const float padding=6*zoom,pitch=fontSize+2*zoom,visible=std::max(1.0f,(panel.GetHeight()-2*padding)/pitch);
        const float maximum=std::max(0.0f,static_cast<float>(lines.size())-visible);
        if(wheel)scroll-=ImGui::GetIO().MouseWheel*3;
        scroll=std::clamp(scroll,0.0f,maximum);
        if(content->text.empty())draw->AddText(font,fontSize,ImVec2(panel.Min.x+padding,panel.Min.y+padding),IM_COL32(137,146,159,255),"Empty text");
        const std::size_t first=static_cast<std::size_t>(scroll),last=std::min(lines.size(),first+static_cast<std::size_t>(std::ceil(visible))+1);
        for(std::size_t i=first;i<last;++i)draw->AddText(font,fontSize,ImVec2(panel.Min.x+padding,panel.Min.y+padding+(static_cast<float>(i)-scroll)*pitch),IM_COL32(208,216,226,255),content->text.data()+lines[i].first,content->text.data()+lines[i].second);
        if(maximum>0)
        {
            const float track=panel.GetHeight()-2*padding,thumb=std::max(12*zoom,track*visible/lines.size());
            const float y=panel.Min.y+padding+(track-thumb)*scroll/maximum;
            draw->AddRectFilled(ImVec2(panel.Max.x-5*zoom,y),ImVec2(panel.Max.x-2*zoom,y+thumb),IM_COL32(102,122,141,220),2*zoom);
        }
    }
};
}
