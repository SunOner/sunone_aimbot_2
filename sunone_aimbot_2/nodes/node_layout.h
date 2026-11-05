#ifndef SUNONE_NODE_LAYOUT_H
#define SUNONE_NODE_LAYOUT_H

#include "nodes/node_graph.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <vector>

// Editor geometry is shared with the starter layouts. Layout is an explicit,
// bounded operation; none of this runs in the evaluation or rendering loops.
namespace NodeGraph::Layout
{
constexpr float Grid=32,CardWidth=192,HeaderHeight=28,PinSpacing=20,Gap=64,ColumnGap=96;
constexpr float RerouteCenterX=32,RerouteCenterY=16;
constexpr float MinWidth=160,MaxWidth=1024,MaxHeight=2048;
inline float up(float value){return std::ceil(value/Grid)*Grid;}
inline float snap(float value){return std::round(value/Grid)*Grid;}
inline bool isReroute(NodeKind kind)
{
    return (kind>=NodeKind::RerouteBoolean&&kind<=NodeKind::RerouteMovement)||
        kind==NodeKind::RerouteFrame||kind==NodeKind::RerouteDepth||kind==NodeKind::RerouteMask||kind==NodeKind::RerouteInteger||
        kind==NodeKind::RerouteString||kind==NodeKind::RerouteMonitor||kind==NodeKind::RerouteMonitors||kind==NodeKind::RerouteOverlay||kind==NodeKind::RerouteLabels||kind==NodeKind::RerouteMotion||kind==NodeKind::RerouteCalibration;
}
// Cards snap by their top-left corner; point nodes snap by their visible center.
inline float snapNodeX(NodeKind kind,float x){const float anchor=isReroute(kind)?RerouteCenterX:0;return snap(x+anchor)-anchor;}
inline float snapNodeY(NodeKind kind,float y){const float anchor=isReroute(kind)?RerouteCenterY:0;return snap(y+anchor)-anchor;}
inline bool inputVisible(const Node& node,std::size_t pin)
{
    return pin<definition(node.kind).inputs.size();
}
inline std::size_t inputCount(const Node& node)
{
    return definition(node.kind).inputs.size();
}
inline std::size_t inputRow(const Node& node,std::size_t pin)
{
    return std::min(pin,inputCount(node));
}
inline bool hasSettings(const Node& node)
{
    // Runtime settings are edited at their input sockets. Only source literals
    // and the static identifier of a Get/Set pair need a separate local field.
    return node.kind==NodeKind::Number||node.kind==NodeKind::Integer||node.kind==NodeKind::Boolean||variableGet(node.kind)||variableSet(node.kind)||calibrationIdentifier(node.kind);
}
inline float naturalSettingsHeight(const Node& node)
{
    if(!hasSettings(node))return 0;
    float height=12;
    // Every local value or identifier uses the same label + editor row.
    height+=44;
    return height;
}
inline float settingsTop(const Node& node)
{
    const auto& def=definition(node.kind);
    return HeaderHeight+10+PinSpacing*static_cast<float>(std::max(inputCount(node),def.outputs.size()));
}
inline float width(const Node& node){return isReroute(node.kind)?64:node.width>0?std::clamp(snap(node.width),MinWidth,MaxWidth):node.kind==NodeKind::ImagePreview?352:node.kind==NodeKind::MouseCalibration||node.kind==NodeKind::MouseProfile?320:inputCount(node)>8?288:CardWidth;}
inline float previewHeight(const Node& node){return node.kind==NodeKind::ImagePreview?std::clamp((width(node)-24)*9/16,64.0f,384.0f)+8:0;}
inline float minimumHeight(const Node& node)
{
    if(isReroute(node.kind))return 32;
    const float panel=std::min(naturalSettingsHeight(node),64.0f);
    return settingsTop(node)+26+previewHeight(node)+(hasSettings(node)?panel+8:0);
}
inline float height(const Node& node,bool expanded=false)
{
    if(isReroute(node.kind))return 32;
    const auto& def=definition(node.kind);
    if(node.collapsed&&!expanded)return up(std::max(56.0f,24+12.0f*static_cast<float>(std::max(inputCount(node),def.outputs.size())-1)));
    return node.height>0?std::clamp(snap(node.height),up(minimumHeight(node)),MaxHeight):up(settingsTop(node)+26+previewHeight(node)+(hasSettings(node)?naturalSettingsHeight(node)+8:0));
}
inline float settingsHeight(const Node& node){return hasSettings(node)?std::max(0.0f,height(node,true)-settingsTop(node)-34):0;}
struct Extent{float width=0,height=0;};

inline bool arrange(Graph& graph,int bandColumns=4)
{
    bandColumns=std::clamp(bandColumns,1,6);
    const auto before=graph;
    const int count=static_cast<int>(graph.nodes.size()),groupCount=static_cast<int>(graph.groups.size());
    if(!count&&!groupCount)return false;
    const auto index=[&](int id){for(int i=0;i<count;++i)if(graph.nodes[i].id==id)return i;return -1;};
    std::vector<std::vector<int>> next(count),previous(count);
    for(const auto& link:graph.links)
    {const int from=index(link.fromNode),to=index(link.toNode);if(from>=0&&to>=0){next[from].push_back(to);previous[to].push_back(from);}}
    std::vector<int> critical(count,-1),visiting(count,0);
    std::function<int(int)> depth=[&](int node)
    {
        if(critical[node]>=0)return critical[node];if(visiting[node])return 0;
        visiting[node]=1;int result=0;for(int target:next[node])result=std::max(result,1+depth(target));
        visiting[node]=0;return critical[node]=result;
    };
    for(int i=0;i<count;++i)depth(i);

    // Strict member-set containment forms a group tree. A node belongs to its
    // smallest frame, so nested groups are moved and measured as one unit.
    std::vector<std::vector<int>> members(groupCount);
    for(int g=0;g<groupCount;++g)for(int id:graph.groups[g].nodes)
    {const int n=index(id);if(n>=0)members[g].push_back(n);}
    for(auto& ids:members){std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());}
    std::vector<int> parent(groupCount,-1),owner(count,-1);
    for(int g=0;g<groupCount;++g)
    {
        if(members[g].empty())continue;
        for(int other=0;other<groupCount;++other)
            if(members[other].size()>members[g].size()&&std::includes(members[other].begin(),members[other].end(),members[g].begin(),members[g].end())&&
                (parent[g]<0||members[other].size()<members[parent[g]].size()))parent[g]=other;
    }
    for(int n=0;n<count;++n)for(int g=0;g<groupCount;++g)
        if(std::binary_search(members[g].begin(),members[g].end(),n)&&(owner[n]<0||members[g].size()<members[owner[n]].size()))owner[n]=g;

    const auto offsetNodes=[&](const std::vector<int>& ids,float dx,float dy){for(int n:ids){graph.nodes[n].x+=dx;graph.nodes[n].y+=dy;}};
    std::function<void(int,float,float)> offsetGroup=[&](int g,float dx,float dy)
    {
        graph.groups[g].x+=dx;graph.groups[g].y+=dy;
        for(int n=0;n<count;++n)if(owner[n]==g){graph.nodes[n].x+=dx;graph.nodes[n].y+=dy;}
        for(int child=0;child<groupCount;++child)if(parent[child]==g)offsetGroup(child,dx,dy);
    };
    const auto componentLayout=[&](const std::vector<int>& ids)
    {
        std::vector<int> rank(count,-1),indegree(count,0),order;
        for(int n:ids){rank[n]=0;for(int p:previous[n])if(std::find(ids.begin(),ids.end(),p)!=ids.end())++indegree[n];}
        for(std::size_t step=0;step<ids.size();++step)
        {
            int chosen=-1;
            for(int n:ids)if(indegree[n]==0&&std::find(order.begin(),order.end(),n)==order.end()&&
                (chosen<0||critical[n]>critical[chosen]||(critical[n]==critical[chosen]&&graph.nodes[n].id<graph.nodes[chosen].id)))chosen=n;
            if(chosen<0)break;order.push_back(chosen);
            for(int to:next[chosen])if(rank[to]>=0){rank[to]=std::max(rank[to],rank[chosen]+1);--indegree[to];}
        }
        for(int n:ids)if(std::find(order.begin(),order.end(),n)==order.end())order.push_back(n);
        // Place short side branches beside their consumer rather than leaving
        // a source far away at the beginning of the main processing chain.
        for(auto it=order.rbegin();it!=order.rend();++it)
        {
            int latest=std::numeric_limits<int>::max();
            for(int to:next[*it])if(rank[to]>=0)latest=std::min(latest,rank[to]-1);
            if(latest!=std::numeric_limits<int>::max())rank[*it]=std::max(rank[*it],latest);
        }
        int maximum=0;for(int n:ids)maximum=std::max(maximum,rank[n]);
        float bandY=0,right=0;
        for(int band=0;band<=maximum;band+=bandColumns)
        {
            float x=0,bandHeight=0;
            for(int column=band;column<=std::min(maximum,band+bandColumns-1);++column)
            {
                std::vector<int> layer;for(int n:order)if(rank[n]==column)layer.push_back(n);
                std::stable_sort(layer.begin(),layer.end(),[&](int a,int b){return critical[a]!=critical[b]?critical[a]>critical[b]:graph.nodes[a].id<graph.nodes[b].id;});
                float y=bandY,columnWidth=0;
                for(int n:layer)
                {
                    auto& node=graph.nodes[n];node.x=snapNodeX(node.kind,x);node.y=snapNodeY(node.kind,y+(isReroute(node.kind)?Grid:0));
                    columnWidth=std::max(columnWidth,width(node));y=up(node.y+height(node,true)+Gap);
                }
                if(!layer.empty()){bandHeight=std::max(bandHeight,y-bandY-Gap);right=std::max(right,x+columnWidth);x+=up(columnWidth+ColumnGap);}
            }
            bandY+=up(bandHeight+2*Gap);
        }
        return Extent{up(right),std::max(Grid,bandY-2*Gap)};
    };

    std::function<Extent(int)> pack=[&](int group)
    {
        struct Unit{int group=-1;std::vector<int> nodes;Extent size;};std::vector<Unit> units;
        for(int child=0;child<groupCount;++child)if(parent[child]==group)units.push_back({child,members[child],pack(child)});
        std::vector<int> loose;for(int n=0;n<count;++n)if(owner[n]==group)loose.push_back(n);
        std::sort(loose.begin(),loose.end(),[&](int a,int b){return graph.nodes[a].id<graph.nodes[b].id;});
        std::vector<bool> seen(count,false);
        for(int n:loose)if(!seen[n])
        {
            std::vector<int> ids{n};seen[n]=true;
            for(std::size_t at=0;at<ids.size();++at)
                for(const auto* edges:{&next[ids[at]],&previous[ids[at]]})for(int other:*edges)
                    if(owner[other]==group&&!seen[other]){seen[other]=true;ids.push_back(other);}
            const auto size=componentLayout(ids);units.push_back({-1,std::move(ids),size});
        }
        std::vector<int> unitOf(count,-1),degree(units.size(),0),unitOrder;
        for(std::size_t u=0;u<units.size();++u)for(int n:units[u].nodes)if(unitOf[n]<0)unitOf[n]=static_cast<int>(u);
        for(int n=0;n<count;++n)if(unitOf[n]>=0)for(int to:next[n])
            if(unitOf[to]>=0&&unitOf[n]!=unitOf[to])++degree[unitOf[to]];
        for(std::size_t step=0;step<units.size();++step)
        {
            int chosen=-1;
            for(std::size_t u=0;u<units.size();++u)if(degree[u]==0&&std::find(unitOrder.begin(),unitOrder.end(),static_cast<int>(u))==unitOrder.end()){chosen=static_cast<int>(u);break;}
            if(chosen<0)break;unitOrder.push_back(chosen);
            for(int n=0;n<count;++n)if(unitOf[n]==chosen)for(int to:next[n])
                if(unitOf[to]>=0&&unitOf[to]!=chosen)--degree[unitOf[to]];
        }
        for(std::size_t u=0;u<units.size();++u)if(std::find(unitOrder.begin(),unitOrder.end(),static_cast<int>(u))==unitOrder.end())unitOrder.push_back(static_cast<int>(u));
        std::vector<Unit> ordered;ordered.reserve(units.size());for(int u:unitOrder)ordered.push_back(std::move(units[u]));units=std::move(ordered);
        // Frames follow their dependencies, keeping parallel independent
        // branches on a row rather than stacking unrelated outputs.
        const bool framed=std::any_of(units.begin(),units.end(),[](const Unit& u){return u.group>=0;});
        const int columns=framed?(group<0&&units.size()>=7?3:2):3;
        const float left=group<0?0:Grid,top=group<0?0:2*Grid;
        struct Placed{float x,y,width,height;};std::vector<Placed> placed;
        float x=left,right=left,bottom=top;int column=0;
        for(auto& unit:units)
        {
            if(column==columns){x=left;column=0;}
            // Fill the space below shorter frames without crossing a wider
            // frame in the preceding row. A tall output only reserves its lane.
            float y=top;
            for(const auto& previous:placed)
                if(x<previous.x+previous.width+ColumnGap&&previous.x<x+unit.size.width+ColumnGap)
                    y=std::max(y,up(previous.y+previous.height+ColumnGap));
            if(unit.group>=0)offsetGroup(unit.group,x,y);else offsetNodes(unit.nodes,x,y);
            placed.push_back({x,y,unit.size.width,unit.size.height});
            right=std::max(right,x+unit.size.width);bottom=std::max(bottom,y+unit.size.height);
            x+=up(unit.size.width+ColumnGap);++column;
        }
        Extent size{up(right+(group<0?0:Grid)),up(bottom+(group<0?0:Grid))};
        if(group>=0)
        {
            auto& frame=graph.groups[group];frame.x=frame.y=0;
            frame.width=size.width=std::max(256.0f,size.width);frame.height=size.height=std::max(160.0f,size.height);
        }
        return size;
    };
    pack(-1);
    // Shared memberships are legal even when frames are not strictly nested.
    // Keep every frame around all of its members after placing their owners.
    std::vector<int> groupOrder(groupCount);std::iota(groupOrder.begin(),groupOrder.end(),0);
    const auto nesting=[&](int g){int result=0;while(parent[g]>=0){g=parent[g];++result;}return result;};
    std::stable_sort(groupOrder.begin(),groupOrder.end(),[&](int a,int b){return nesting(a)>nesting(b);});
    for(int g:groupOrder)
    {
        auto& frame=graph.groups[g];float left=frame.x,top=frame.y,right=frame.x+frame.width,bottom=frame.y+frame.height;
        for(int n:members[g])
        {
            const auto& node=graph.nodes[n];left=std::min(left,node.x-Grid);top=std::min(top,node.y-2*Grid);
            right=std::max(right,node.x+width(node)+Grid);bottom=std::max(bottom,node.y+height(node,true)+Grid);
        }
        for(int child=0;child<groupCount;++child)if(parent[child]==g)
        {
            const auto& box=graph.groups[child];left=std::min(left,box.x-Grid);top=std::min(top,box.y-2*Grid);
            right=std::max(right,box.x+box.width+Grid);bottom=std::max(bottom,box.y+box.height+Grid);
        }
        frame.x=std::floor(left/Grid)*Grid;frame.y=std::floor(top/Grid)*Grid;frame.width=up(right-frame.x);frame.height=up(bottom-frame.y);
    }
    // Keep tie membership and rotation, placing each strap near its own wires.
    for(auto& tie:graph.ties)
    {
        float x=0,y=0,uses=0;
        for(const auto& link:graph.links)if(std::find(link.ties.begin(),link.ties.end(),tie.id)!=link.ties.end())
        {
            const int a=index(link.fromNode),b=index(link.toNode);if(a<0||b<0)continue;
            x+=(graph.nodes[a].x+width(graph.nodes[a])+graph.nodes[b].x)*0.5f;
            y+=(graph.nodes[a].y+graph.nodes[b].y)*0.5f+HeaderHeight+15;++uses;
        }
        if(!uses)continue;tie.x=snap(x/uses);tie.y=snap(y/uses);
        for(int attempt=0;attempt<count+1;++attempt)
        {
            bool blocked=false;
            for(const auto& node:graph.nodes)if(tie.x>=node.x-16&&tie.x<=node.x+width(node)+16&&tie.y>=node.y-16&&tie.y<=node.y+height(node,true)+16)
            {tie.y=up(node.y+height(node,true)+Grid);blocked=true;break;}
            if(!blocked)break;
        }
    }
    bool changed=false;
    for(int n=0;n<count;++n)changed|=graph.nodes[n].x!=before.nodes[n].x||graph.nodes[n].y!=before.nodes[n].y;
    for(int g=0;g<groupCount;++g)changed|=graph.groups[g].x!=before.groups[g].x||graph.groups[g].y!=before.groups[g].y||graph.groups[g].width!=before.groups[g].width||graph.groups[g].height!=before.groups[g].height;
    for(std::size_t t=0;t<graph.ties.size();++t)changed|=graph.ties[t].x!=before.ties[t].x||graph.ties[t].y!=before.ties[t].y;
    return changed;
}
}
#endif
