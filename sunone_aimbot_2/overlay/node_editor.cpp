#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include "overlay/node_editor.h"
#include "overlay/node_preview.h"
#include "nodes/node_graph.h"
#include "nodes/node_layout.h"
#include "nodes/node_config_bridge.h"
#include "nodes/vision_runtime.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "config.h"
#include "keycodes.h"
#include "overlay/overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#pragma comment(lib, "Comdlg32.lib")

extern Config config;

namespace
{
using namespace NodeGraph;
constexpr const char* GraphFile = "node_graphs.ini";
constexpr float CardWidth = Layout::CardWidth;
constexpr float HeaderHeight = Layout::HeaderHeight;
constexpr float PinSpacing = Layout::PinSpacing;
constexpr float GridSize = Layout::Grid;
float snap(float value) { return std::round(value / GridSize) * GridSize; }
float snapAngle(float angle){return std::remainder(std::round(angle/15.0f)*15.0f,360.0f);}
bool reroute(NodeKind kind)
{
    return (kind >= NodeKind::RerouteBoolean && kind <= NodeKind::RerouteMovement) ||
           Layout::isReroute(kind);
}
NodeKind rerouteKind(ValueType type)
{
    switch (type)
    {
    case ValueType::Boolean: return NodeKind::RerouteBoolean;
    case ValueType::Number: return NodeKind::RerouteNumber;
    case ValueType::Integer: return NodeKind::RerouteInteger;
    case ValueType::String:return NodeKind::RerouteString;
    case ValueType::Monitor:return NodeKind::RerouteMonitor;
    case ValueType::Monitors:return NodeKind::RerouteMonitors;
    case ValueType::Point: return NodeKind::ReroutePoint;
    case ValueType::Detections: return NodeKind::RerouteDetections;
    case ValueType::Target: return NodeKind::RerouteTarget;
    case ValueType::Movement: return NodeKind::RerouteMovement;
    case ValueType::Frame: return NodeKind::RerouteFrame;
    case ValueType::Depth: return NodeKind::RerouteDepth;
    case ValueType::Mask: return NodeKind::RerouteMask;
    case ValueType::OverlayStyle:return NodeKind::RerouteOverlay;
    case ValueType::LabelOptions:return NodeKind::RerouteLabels;
    }
    return NodeKind::RerouteBoolean;
}
float cardWidth(const Node& node) { return Layout::width(node); }
bool hasNodeSettings(const Node& node)
{
    return Layout::hasSettings(node);
}
float nodeSettingsHeight(const Node& node)
{
    return Layout::settingsHeight(node);
}
float nodeSettingsTop(const Node& node)
{
    return Layout::settingsTop(node);
}
const ImU32 Cyan = IM_COL32(96, 205, 255, 255);
struct WireAnimation
{
    std::uint64_t signal=0;
    double born=-1;
    double started=-1;
    float duration=0.25f;
    double phase=1;
    float strength=0;
    float activity=0;
    int frame=-1;
};
struct SignalEvent {std::uint64_t sequence=0;float age=1;float strength=0;};
constexpr float SignalDuration=0.24f;
constexpr std::size_t MaxSignalPulses=64;
constexpr double MinSignalInterval=0.125;
struct RoutedWire
{
    std::vector<ImVec2> path,localPath;
    std::vector<float> tieDistances,localTieDistances,cumulative;
    ImVec2 minimum,maximum,localMinimum,localMaximum;
    ImVec2 bendPoint,localBendPoint;
    float length=0;
    std::size_t from=0,to=0;
};
struct OverviewCaption
{
    struct Line{std::size_t begin=0,end=0;float width=0;};
    NodeKind kind=NodeKind::Count;
    ImFont* font=nullptr;
    std::string text;
    std::vector<Line> lines;
    ImVec2 extent,available;
    float baseSize=0,cardWidth=0,cardHeight=0,fontSize=0,wrapWidth=0,top=0;
    int zoomBucket=-1;
    bool badge=false;
    std::uint64_t layouts=0;
};
enum class InputDefaultKind { Unset, Number, Boolean, Text, Required, Center, Local, Empty };
struct InputDefaultDisplay
{
    InputDefaultKind kind=InputDefaultKind::Unset;
    const ParameterSpec* parameter=nullptr;
    const TextParameterSpec* textParameter=nullptr;
    double number=0;
    std::string source,text,fullText,shown;
    std::vector<char> editBuffer;
    ImFont* labelFont=nullptr;
    float labelFontSize=0,labelWidth=0;
    ImFont* font=nullptr;
    float fontSize=0,limit=-1,width=0;
    ImFont* measureFont=nullptr;
    float measureSize=0,naturalWidth=-1;
};
struct NodeDisplay
{
    int id=0;
    NodeKind kind=NodeKind::Count;
    std::uint64_t sequence=0,textSequence=0,formats=0,flashes=0;
    double lastEvent=-1,flashAt=-1,lastFlash=-1,rapidUntil=-1,textAt=-1,constant=0;
    float strength=0;
    int quickEvents=0;
    bool available=false,disabled=false;
    std::string text,error;
    float settingsContent=0;
    std::map<std::string,std::vector<char>> settingsBuffers;
    OverviewCaption caption;
    NodeKind defaultKind=NodeKind::Count;
    std::vector<InputDefaultDisplay> defaults;
};
struct CatalogPreviewState
{
    struct Row
    {
        float y=0,height=0,labelWidth=0,valueX=0,valueY=0,valueWidth=0,valueHeight=0,outputWidth=0;
    };
    NodeKind requested=NodeKind::Count,kind=NodeKind::Count;
    Node node;
    NodeDisplay display;
    ImRect anchor,bounds;
    ImGuiID owner=0;
    ImFont* measuredFont=nullptr;
    float measuredSize=0,desiredWidth=380,contentHeight=0;
    std::vector<Row> rows;
    float layoutWidth=-1,headerHeight=0,cardHeight=0,settingsY=0;
    std::uint64_t layouts=0;
    double hoveredAt=-1;
    bool interacting=false;
    int requestFrame=-1,drawnFrame=-1,inputRows=0,outputRows=0;
    std::uint64_t builds=0;
};
struct CanvasRoutes
{
    std::vector<std::uint64_t> key,scratch;
    std::vector<RoutedWire> routes;
    std::vector<std::array<std::uint64_t,2>> connections;
    std::vector<NodeDisplay> displays;
    ImVec2 translation;
    float zoom=0;
    std::uint64_t rebuilds=0;
};
struct TieHit {std::array<int,4> link;std::size_t slot=0;ImVec2 point;};
struct CutPiece {std::vector<ImVec2> points;float length=0;ImVec2 recoil;};
struct CutAnimation {int graph=0;double born=0;ImU32 color=0;ImVec2 minimum,maximum;std::array<CutPiece,2> pieces;};
constexpr std::size_t MaxCutAnimations=24;
constexpr float CutDuration=0.36f;
struct CatalogSearchState
{
    std::string query,error,pendingText;
    ImVec2 popupAnchor{},popupSize{};
    std::vector<NodeKind> nodes,available;
    int selected=0,step=0,graph=-1,anchor=-1,port=-1;
    bool output=true,browse=false,navigate=false,accept=false,reveal=false,refocus=false,candidatesReady=false,filteredReady=false;
    std::uint64_t revision=0,builds=0,candidateBuilds=0;
};
struct Editor
{
    Workspace draft;
    bool initialized = false;
    bool dirty = false;
    std::uint64_t observedRevision = 0;
    int activeGraph = -1;
    std::array<bool,GraphCount> fit{};
    std::array<bool,GraphCount> autoFit{};
    ImVec2 canvasSize[GraphCount];
    ImVec2 canvasOrigin[GraphCount];
    ImVec2 pastePosition[GraphCount];
    bool canvasHovered[GraphCount] = {};
    bool focusSelection[GraphCount] = {};
    int selected[GraphCount] = {};
    std::vector<int> selection[GraphCount];
    std::vector<int> tieSelection[GraphCount];
    int selectedTie[GraphCount] = {};
    struct PortFocus {int node=0;int port=0;bool output=false;};
    std::array<PortFocus,GraphCount> portFocus{};
    bool keyboardWire = false;
    int selectedGroup[GraphCount] = {};
    int dragNode = 0;
    int toggleSelectionOnRelease = 0;
    int dragGroup = 0;
    int resizeGroup = 0;
    int resizeNode = 0,resizeMask = 0,resizeGraph = -1;
    Workspace resizeOriginal;
    bool resizeChanged = false;
    int dragTie = 0;
    bool rotateTie = false;
    int bendGraph = -1;
    std::array<int,4> bendLink{};
    WireBend bendStart;
    ImVec2 bendStartMouse;
    float bendSensitivity = 1;
    bool bendChanged = false;
    Workspace bendOriginal;
    int bendHandleGraph = -1;
    std::array<int,4> bendHandleLink{};
    float bendHandleFade = 0;
    int wireNode = 0;
    int wirePort = 0;
    bool wireOutput = true;
    bool moved = false;
    ImVec2 dragStartMouse;
    ImVec2 dragStartNode;
    struct NodePosition { int id; float x; float y; };
    std::vector<NodePosition> dragPositions;
    struct TiePosition {int id;float x;float y;float angle;};
    std::vector<TiePosition> dragTiePositions;
    struct GroupPosition {int id;float x;float y;};
    std::vector<GroupPosition> dragGroupPositions;
    ImVec2 dragStartGroup;
    ImVec2 dragStartSize;
    int panButton = -1;
    bool panned = false;
    bool emptyClickCandidate = false;
    ImVec2 panStartMouse;
    ImVec2 panStart;
    int contextNode = 0;
    int contextGroup = 0;
    int contextTie = 0;
    std::array<int,4> contextLink{};
    bool hasContextLink=false;
    bool tieMode = false;
    bool tieStroke = false;
    ImVec2 tieStart,tieLast;
    std::vector<TieHit> tieHits;
    std::vector<int> crossedTies;
    bool keyboardMove = false;
    bool pointerMove = false;
    int moveGraph = -1;
    bool confirmKeyboardMove = false;
    Workspace moveOriginal;
    std::vector<int> moveSelection,moveTies;
    int moveGroup = 0;
    bool boxSelectMode = false;
    bool showShortcuts = false;
    bool popupNavigation = false;
    bool popupUsesKeyboard = false;
    int popupInputFrame = -1;
    bool restoreKeyboardNavigation = false;
    int marqueeButton = ImGuiMouseButton_Right;
    bool cutStroke = false;
    ImVec2 cutStart,cutLast;
    std::vector<TieHit> cutHits;
    std::vector<CutAnimation> cutAnimations;
    std::vector<ImVec2> cutScratch;
    bool rightGesture = false;
    bool marquee = false;
    ImVec2 marqueeStart;
    std::vector<int> marqueeBase;
    std::vector<int> marqueeBaseTies;
    char groupTitle[129] = {};
    ImVec2 catalogPosition;
    char search[96] = {};
    CatalogSearchState catalogSearch;
    CatalogPreviewState catalogPreview;
    std::string message;
    std::string shownMessage;
    double messageAt = 0;
    bool error = false;
    std::vector<Workspace> undo;
    std::vector<Workspace> redo;
    Telemetry live;
    std::array<std::vector<std::pair<int,std::size_t>>,GraphCount> liveIndex;
    double telemetryAt = -1.0;
    std::chrono::steady_clock::time_point appliedAt{};
    std::chrono::steady_clock::time_point editedAt{};
    std::chrono::steady_clock::time_point saveAttemptAt{};
    int fileAction = 0;
    int fileGraph = 0;
    int fileNode = 0;
    std::string fileParameter;
    bool preview = false;
    int viewMode = 0; // Automatic, detailed, compact.
    bool saveFailed = false;
    bool loadFailed = false;
    std::string loadError;
    bool inspectorOpen = false;
    int inspectedGraph = -1;
    int inspectedNode = 0;
    bool catalogFocus = false;
    int catalogWireNode = 0;
    int catalogWirePort = 0;
    bool catalogWireOutput = true;
    ImGuiID parameterUndoItem = 0;
    ImGuiID inlineActiveItem = 0;
    int inlineActiveNode = 0;
    int inputEditNode = 0,inputEditPin = -1;
    int inputPopupNode=0,inputPopupPin=-1;
    ImGuiID inputPopup=0;
    std::map<std::array<int,5>,WireAnimation> wireAnimations;
    std::array<CanvasRoutes,GraphCount> canvasRoutes;
    std::size_t drawnSignalPulses=0;
    std::array<ImVec4,16> toolbarItems{};
    std::size_t toolbarItemCount=0;
    Editor(){fit.fill(true);autoFit.fill(true);}
} editor;

bool overviewNode(const Graph& graph)
{
    return editor.viewMode==2||(editor.viewMode==0&&graph.zoom<0.65f);
}
bool compactNode(const Graph& graph,const Node& node)
{
    return node.collapsed||overviewNode(graph);
}

ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
float distanceSquared(ImVec2 a, ImVec2 b) { return (a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y); }
bool contains(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x>=a.x && p.y>=a.y && p.x<=b.x && p.y<=b.y; }
ImU32 pinColor(ValueType type)
{
    switch(type)
    {
    case ValueType::Boolean:return IM_COL32(133,226,154,255);
    case ValueType::Float:return IM_COL32(246,195,101,255);
    case ValueType::Integer:return IM_COL32(110,160,255,255);
    case ValueType::Point:return IM_COL32(88,218,239,255);
    case ValueType::Detections:return IM_COL32(180,140,245,255);
    case ValueType::Target:return IM_COL32(244,143,83,255);
    case ValueType::Movement:return IM_COL32(240,126,172,255);
    case ValueType::Frame:return IM_COL32(216,217,233,255);
    case ValueType::Depth:return IM_COL32(75,194,171,255);
    case ValueType::Mask:return IM_COL32(219,128,239,255);
    case ValueType::String:return IM_COL32(239,164,208,255);
    case ValueType::Monitor:return IM_COL32(177,213,103,255);
    case ValueType::Monitors:return IM_COL32(215,225,160,255);
    case ValueType::OverlayStyle:return IM_COL32(255,164,106,255);
    case ValueType::LabelOptions:return IM_COL32(151,190,122,255);
    }
    return IM_COL32(180,180,180,255);
}
ImU32 wireColor(ValueType type,bool active,bool disabled)
{
    return (pinColor(type)&0x00ffffff)|IM_COL32(0,0,0,disabled?105:active?255:165);
}
float cardHeight(const Node& node)
{
    return Layout::height(node);
}
void fitContainingGroups(Graph& graph,int id);
bool normalizeInputPorts(Graph& graph)
{
    bool changed=false;
    for(auto& node:graph.nodes)if(node.height>0&&node.height<Layout::minimumHeight(node))
    {node.height=Layout::up(Layout::minimumHeight(node));changed=true;}
    if(changed)for(const auto& node:graph.nodes)fitContainingGroups(graph,node.id);
    return changed;
}
bool snapLayout(Graph& graph)
{
    bool changed=normalizeInputPorts(graph);
    for(auto& node:graph.nodes)
    {
        const float x=snap(node.x),y=snap(node.y);changed|=x!=node.x||y!=node.y;node.x=x;node.y=y;
        if(node.width>0){const float width=std::clamp(snap(node.width),Layout::MinWidth,Layout::MaxWidth);changed|=width!=node.width;node.width=width;}
        if(node.height>0){const float height=std::clamp(snap(node.height),Layout::up(Layout::minimumHeight(node)),Layout::MaxHeight);changed|=height!=node.height;node.height=height;}
    }
    for(auto& group:graph.groups)
    {
        const float right=group.x+group.width,bottom=group.y+group.height;
        const float x=std::floor(group.x/GridSize)*GridSize,y=std::floor(group.y/GridSize)*GridSize;
        const float width=std::ceil((right-x)/GridSize)*GridSize,height=std::ceil((bottom-y)/GridSize)*GridSize;
        changed|=x!=group.x||y!=group.y||width!=group.width||height!=group.height;
        group.x=x;group.y=y;group.width=width;group.height=height;
    }
    for(auto& tie:graph.ties){const float x=snap(tie.x),y=snap(tie.y),angle=snapAngle(tie.angle);changed|=x!=tie.x||y!=tie.y||angle!=tie.angle;tie.x=x;tie.y=y;tie.angle=angle;}
    return changed;
}
bool normalizeLayout(Graph& graph,bool legacy)
{
    bool changed=migrateStarterLayout(graph);
    if(!changed&&legacy)
    {
        bool cramped=false;
        for(std::size_t a=0;a<graph.nodes.size();++a)
            for(std::size_t b=a+1;b<graph.nodes.size();++b)
            {
                const auto& first=graph.nodes[a];const auto& second=graph.nodes[b];
                if(first.x+cardWidth(first)+GridSize>second.x&&second.x+cardWidth(second)+GridSize>first.x&&
                    first.y+Layout::height(first,true)+GridSize>second.y&&second.y+Layout::height(second,true)+GridSize>first.y)cramped=true;
            }
        for(const auto& group:graph.groups)for(int id:group.nodes)
        {
            const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==id;});
            if(found!=graph.nodes.end()&&(found->x<group.x+GridSize||found->y<group.y+2*GridSize||
                found->x+cardWidth(*found)>group.x+group.width-GridSize||found->y+Layout::height(*found,true)>group.y+group.height-GridSize))cramped=true;
        }
        if(cramped&&Layout::arrange(graph)){graph.panX=graph.panY=40;graph.zoom=1;changed=true;}
    }
    return snapLayout(graph)||changed;
}
float renderHeight(const Graph& graph,const Node& node)
{
    // Semantic zoom changes detail only. User collapse remains the sole action
    // that changes a card's shape or the positions of its ports.
    return cardHeight(node);
}
const char* compactTitle(NodeKind kind)
{
    using K=NodeKind;
    switch(kind)
    {
    case K::FilterNms:return "NMS";
    case K::CaptureSource:return "Capture";
    case K::Inference:return "Detector";
    case K::DepthInference:return "Depth";
    case K::DepthToMask:return "Depth mask";
    case K::ApplyMask:return "Apply mask";
    case K::FilterMask:return "Mask filter";
    case K::FilterClasses:return "Class filter";
    case K::FilterConfidence:return "Conf. filter";
    case K::FilterCircle:return "FOV filter";
    case K::FilterSize:return "Size filter";
    case K::LimitDetections:return "Limit";
    case K::RenderOverlay:return "Overlay";
    case K::OverlayOutput:return "Overlay";
    case K::OverlayBoxes:return "Boxes";
    case K::OverlayFrame:return "Frame / FOV";
    case K::OverlayMotion:return "Trails";
    case K::OverlayIcon:return "Icon";
    case K::OverlayDepth:return "Depth / mask";
    case K::OverlayCorrection:return "Correction";
    case K::DatasetOutput:return "Dataset";
    case K::DatasetLabels:return "Labels";
    case K::CollectDataset:return "Dataset";
    case K::ScreenshotOutput:return "Save image";
    case K::TargetNearest:return "Nearest";
    case K::TargetTracker:return "Tracker";
    case K::CameraCompensation:return "Cam. comp.";
    case K::NormalizeFps:return "FPS scale";
    case K::SpeedCurve:return "Speed";
    case K::WindMotion:return "Wind";
    case K::AutoAimEnabled:return "Auto aim";
    case K::AutoShootEnabled:return "Shoot on";
    case K::RecoilEnabled:return "Recoil on";
    case K::AimOutput:return "Mouse out";
    case K::RecoilOutput:return "Recoil";
    case K::GetMonitors:return "Monitors";
    case K::MonitorAt:return "Monitor";
    case K::PrimaryMonitor:return "Primary";
    case K::Center:return "Center";
    case K::CaptureFps:return "FPS";
    default:return definition(kind).title;
    }
}
const OverviewCaption& overviewCaption(NodeDisplay& display,const Node& node,float zoom,ImFont* font,float baseSize)
{
    auto& caption=display.caption;
    const int bucket=static_cast<int>(std::floor(zoom*64));
    const float height=cardHeight(node);const bool badge=(node.bypassed||node.disabled)&&zoom>=0.55f;
    if(caption.kind==node.kind&&caption.font==font&&caption.baseSize==baseSize&&caption.cardWidth==cardWidth(node)&&caption.cardHeight==height&&
        caption.zoomBucket==bucket&&caption.badge==badge)return caption;
    caption.kind=node.kind;caption.font=font;caption.baseSize=baseSize;caption.cardWidth=cardWidth(node);caption.cardHeight=height;caption.zoomBucket=bucket;caption.badge=badge;++caption.layouts;
    caption.text=compactTitle(node.kind);
    for(auto& letter:caption.text)if(letter>='a'&&letter<='z')letter+='A'-'a';
    // Fit at the lower edge of a small zoom bucket. Cached text stays inside
    // the card throughout that bucket; integer sizes bound the font atlas too.
    const float scale=std::max(0.25f,bucket/64.0f),padding=std::max(3.0f,8*scale);
    const float vertical=std::max(2.0f,6*scale);caption.top=badge?HeaderHeight*scale:0;
    caption.available=ImVec2(std::max(1.0f,cardWidth(node)*scale-2*padding),std::max(1.0f,(height*scale-caption.top)-2*vertical));
    caption.wrapWidth=caption.available.x;
    caption.fontSize=std::clamp(std::round(baseSize*1.8f*std::sqrt(scale)),8.0f,28.0f);
    const auto measure=[&](){return font->CalcTextSizeA(caption.fontSize,std::numeric_limits<float>::max(),caption.wrapWidth,caption.text.c_str());};
    const auto wordWidth=[&](){
        float width=0;const char* cursor=caption.text.c_str();
        while(*cursor)
        {
            while(*cursor==' '||*cursor=='\n')++cursor;
            const char* end=cursor;while(*end&&*end!=' '&&*end!='\n')++end;
            width=std::max(width,font->CalcTextSizeA(caption.fontSize,std::numeric_limits<float>::max(),0,cursor,end).x);cursor=end;
        }
        return width;
    };
    caption.extent=measure();
    while(caption.fontSize>8&&(caption.extent.x>caption.available.x+0.1f||caption.extent.y>caption.available.y+0.1f||wordWidth()>caption.available.x+0.1f))
    {caption.fontSize-=1;caption.extent=measure();}
    if(caption.extent.x>caption.available.x+0.1f||caption.extent.y>caption.available.y+0.1f)
    {
        // A manually collapsed card can be only 14 screen pixels high. Keep
        // its label legible and clipped; hovering still shows the full name.
        const std::string full=caption.text;
        for(std::size_t length=full.size();length>0;--length)
        {
            caption.text=full.substr(0,length)+"..";caption.extent=measure();
            if(caption.extent.x<=caption.available.x+0.1f&&caption.extent.y<=caption.available.y+0.1f)break;
        }
    }
    caption.lines.clear();caption.extent=ImVec2(0,0);
    const char* begin=caption.text.c_str();const char* end=begin+caption.text.size();
    for(const char* cursor=begin;cursor<end;)
    {
        const char* lineEnd=font->CalcWordWrapPosition(caption.fontSize,cursor,end,caption.wrapWidth);
        if(lineEnd==cursor){unsigned int codepoint;lineEnd=cursor+std::max(1,ImTextCharFromUtf8(&codepoint,cursor,end));}
        const char* visibleEnd=lineEnd;while(visibleEnd>cursor&&(visibleEnd[-1]==' '||visibleEnd[-1]=='\n'))--visibleEnd;
        const float width=font->CalcTextSizeA(caption.fontSize,std::numeric_limits<float>::max(),0,cursor,visibleEnd).x;
        caption.lines.push_back({static_cast<std::size_t>(cursor-begin),static_cast<std::size_t>(visibleEnd-begin),width});
        caption.extent.x=std::max(caption.extent.x,width);caption.extent.y+=caption.fontSize;
        cursor=lineEnd;while(cursor<end&&(*cursor==' '||*cursor=='\n'))++cursor;
    }
    return caption;
}
Node* findNode(Graph& graph, int id)
{
    for (auto& node : graph.nodes) if (node.id == id) return &node;
    return nullptr;
}
const Node* findNode(const Graph& graph,int id)
{
    for(const auto& node:graph.nodes)if(node.id==id)return &node;
    return nullptr;
}
Group* findGroup(Graph& graph, int id)
{
    for (auto& group : graph.groups) if (group.id == id) return &group;
    return nullptr;
}
const Group* findGroup(const Graph& graph,int id)
{for(const auto& group:graph.groups)if(group.id==id)return &group;return nullptr;}
WireTie* findTie(Graph& graph,int id)
{for(auto& tie:graph.ties)if(tie.id==id)return &tie;return nullptr;}
const WireTie* findTie(const Graph& graph,int id)
{for(const auto& tie:graph.ties)if(tie.id==id)return &tie;return nullptr;}
std::array<int,4> linkKey(const Link& link){return {link.fromNode,link.fromPort,link.toNode,link.toPort};}
bool looseReroute(const Graph& graph,const Node& node)
{
    return reroute(node.kind)&&node.inputValues.empty()&&node.inputTexts.empty()&&
        std::none_of(graph.links.begin(),graph.links.end(),[&](const Link& link){return link.fromNode==node.id||link.toNode==node.id;});
}
ValueType connectionType(const Graph& graph,const Link& link)
{
    const auto* source=findNode(graph,link.fromNode);const auto* target=findNode(graph,link.toNode);
    if(source&&target&&looseReroute(graph,*source)&&!looseReroute(graph,*target))return definition(target->kind).inputs[link.toPort].type;
    return source?definition(source->kind).outputs[link.fromPort].type:ValueType::Float;
}
void adaptLooseReroutes(Graph& graph,const Link& link)
{
    auto* source=findNode(graph,link.fromNode);auto* target=findNode(graph,link.toNode);
    if(!source||!target||source==target||link.fromPort<0||link.toPort<0||
        static_cast<std::size_t>(link.fromPort)>=definition(source->kind).outputs.size()||
        static_cast<std::size_t>(link.toPort)>=definition(target->kind).inputs.size())return;
    const auto type=connectionType(graph,link);
    if(looseReroute(graph,*source))source->kind=rerouteKind(type);
    if(looseReroute(graph,*target))target->kind=rerouteKind(type);
}
bool canConnectReroutes(const Graph& graph,const Link& link,std::string& error)
{
    const auto* source=findNode(graph,link.fromNode);const auto* target=findNode(graph,link.toNode);
    if((source&&looseReroute(graph,*source))||(target&&looseReroute(graph,*target)))
    {auto candidate=graph;adaptLooseReroutes(candidate,link);return canConnect(candidate,link,error);}
    return canConnect(graph,link,error);
}
bool usesTie(const Link& link,int id){return std::find(link.ties.begin(),link.ties.end(),id)!=link.ties.end();}
bool isTieSelected(int tab,int id)
{const auto& selected=editor.tieSelection[tab];return std::find(selected.begin(),selected.end(),id)!=selected.end();}
void pruneTies(Graph& graph)
{
    graph.ties.erase(std::remove_if(graph.ties.begin(),graph.ties.end(),[&](const WireTie& tie){
        return std::none_of(graph.links.begin(),graph.links.end(),[&](const Link& link){return usesTie(link,tie.id);});}),graph.ties.end());
}
void detachTie(Graph& graph,int id)
{
    for(auto& link:graph.links)link.ties.erase(std::remove(link.ties.begin(),link.ties.end(),id),link.ties.end());
    graph.ties.erase(std::remove_if(graph.ties.begin(),graph.ties.end(),[&](const WireTie& tie){return tie.id==id;}),graph.ties.end());
}
bool tieInGroup(const Graph& graph,int id,const Group& group)
{
    bool used=false;
    for(const auto& link:graph.links)if(usesTie(link,id))
    {
        used=true;
        if(std::find(group.nodes.begin(),group.nodes.end(),link.fromNode)==group.nodes.end()||std::find(group.nodes.begin(),group.nodes.end(),link.toNode)==group.nodes.end())return false;
    }
    return used;
}
bool isSelected(int tab,int id);
void captureDragSelection(const Graph& graph,int tab,bool includeGroups=false)
{
    editor.dragPositions.clear();editor.dragTiePositions.clear();editor.dragGroupPositions.clear();
    for(const auto& node:graph.nodes)if(isSelected(tab,node.id))editor.dragPositions.push_back({node.id,node.x,node.y});
    for(const auto& tie:graph.ties)if(isTieSelected(tab,tie.id))editor.dragTiePositions.push_back({tie.id,tie.x,tie.y,tie.angle});
    if(includeGroups)for(const auto& group:graph.groups)
        if(group.id==editor.selectedGroup[tab]||(!group.nodes.empty()&&std::all_of(group.nodes.begin(),group.nodes.end(),[&](int id){return isSelected(tab,id);})) )
            editor.dragGroupPositions.push_back({group.id,group.x,group.y});
}
bool isSelected(int tab, int id)
{
    const auto& selection = editor.selection[tab];
    return std::find(selection.begin(), selection.end(), id) != selection.end();
}
void clearSelection(int tab)
{
    editor.selection[tab].clear(); editor.selected[tab] = 0;editor.tieSelection[tab].clear();editor.selectedTie[tab]=0;editor.portFocus[tab]={};
}
void selectOne(int tab, int id)
{
    clearSelection(tab);
    if (id) editor.selection[tab].push_back(id);
    editor.selected[tab] = id; editor.selectedGroup[tab] = 0;
}
void selectOneTie(int tab,int id)
{clearSelection(tab);editor.tieSelection[tab]={id};editor.selectedTie[tab]=id;editor.selectedGroup[tab]=0;}
void cancelLeftGesture()
{
    editor.dragNode = editor.wireNode = editor.dragGroup = editor.resizeGroup = editor.dragTie = 0;
    editor.resizeNode=editor.resizeMask=0;editor.resizeGraph=-1;editor.resizeChanged=false;
    editor.toggleSelectionOnRelease = 0;
    editor.dragPositions.clear();editor.dragTiePositions.clear();editor.dragGroupPositions.clear();editor.tieStroke=false;editor.tieHits.clear();editor.crossedTies.clear();
    editor.keyboardMove=false;editor.confirmKeyboardMove=false;
    editor.pointerMove=false;editor.moveGraph=-1;
    editor.keyboardWire=false;
    editor.bendGraph=-1;editor.bendChanged=false;
}
void cancelGestures()
{
    cancelLeftGesture();editor.panButton=-1;editor.panned=false;editor.rightGesture=editor.marquee=false;editor.tieMode=editor.boxSelectMode=editor.cutStroke=false;
    editor.emptyClickCandidate=false;
    editor.portFocus.fill({});
}
void beginPan(const Graph& graph,int button)
{
    editor.panButton=button;editor.panned=false;editor.panStartMouse=ImGui::GetIO().MousePos;
    editor.panStart=ImVec2(graph.panX,graph.panY);
}
int nextNodeId(const Graph& graph)
{
    for (int id=1;id<=static_cast<int>(MaxNodes)+1;++id)
    {
        bool used=false;
        for (const auto& node:graph.nodes) if (node.id==id) { used=true; break; }
        if (!used) return id;
    }
    return 0;
}
void rememberSnapshot(const Workspace& snapshot)
{
    if (editor.undo.size() >= 40) editor.undo.erase(editor.undo.begin());
    editor.undo.push_back(snapshot);
    editor.redo.clear();
}
void remember(){rememberSnapshot(editor.draft);}
void edited(bool logic = true)
{
    for(std::size_t tab=0;tab<GraphCount;++tab)
    {
        auto& graph=editor.draft.graphs[tab];normalizeInputPorts(graph);pruneTies(graph);
        auto& selected=editor.tieSelection[tab];selected.erase(std::remove_if(selected.begin(),selected.end(),[&](int id){return !findTie(graph,id);}),selected.end());
        if(!findTie(graph,editor.selectedTie[tab]))editor.selectedTie[tab]=selected.empty()?0:selected.back();
    }
    std::string error;
    if (!service().publish(editor.draft,error))
    {
        editor.draft=service().workspace();editor.error=true;editor.message=error;cancelGestures();return;
    }
    editor.observedRevision=service().revision();
    editor.editedAt=std::chrono::steady_clock::now();
    if (logic) editor.appliedAt=editor.editedAt;
    // Keep an unreadable file intact until an explicit import or restore.
    if(editor.loadFailed)return;
    editor.dirty=true;
    editor.error = false;
    editor.message.clear();
}
bool sameBend(WireBend a,WireBend b){return a.roundness==b.roundness&&a.twist==b.twist;}
void beginWireBend(int index,const Link& link)
{
    editor.bendGraph=index;editor.bendLink=linkKey(link);editor.bendStart=link.bend;editor.bendChanged=false;
    editor.bendOriginal=editor.draft;editor.bendStartMouse=ImGui::GetIO().MousePos;
    editor.bendSensitivity=ImGui::GetIO().KeyShift?0.25f:1.0f;editor.portFocus[index]={};
}
void updateWireBend(ImVec2 pointer)
{
    auto& graph=editor.draft.graphs[editor.bendGraph];
    const auto found=std::find_if(graph.links.begin(),graph.links.end(),[](const Link& link){return linkKey(link)==editor.bendLink;});
    if(found==graph.links.end())return;
    const float sensitivity=editor.bendSensitivity/180.0f;
    const WireBend next{std::clamp(editor.bendStart.roundness+(pointer.x-editor.bendStartMouse.x)*sensitivity,0.0f,1.0f),
        std::clamp(editor.bendStart.twist+(pointer.y-editor.bendStartMouse.y)*sensitivity,-1.0f,1.0f)};
    if(sameBend(found->bend,next))return;
    found->bend=next;editor.bendChanged=!sameBend(next,editor.bendStart);edited(false);
}
void finishWireBend(bool confirm)
{
    if(editor.bendGraph<0)return;
    const int index=editor.bendGraph;const bool changed=editor.bendChanged;
    editor.bendGraph=-1;editor.bendChanged=false;
    if(!changed)return;
    if(confirm)rememberSnapshot(editor.bendOriginal);
    else
    {
        auto& links=editor.draft.graphs[index].links;
        const auto found=std::find_if(links.begin(),links.end(),[](const Link& link){return linkKey(link)==editor.bendLink;});
        if(found!=links.end()){found->bend=editor.bendStart;edited(false);}
    }
}
void resetWireBend(Link& link)
{
    if(sameBend(link.bend,WireBend{}))return;
    remember();link.bend={};edited(false);
}
void fitContainingGroups(Graph& graph,int id)
{
    const auto* node=findNode(graph,id);if(!node)return;
    for(auto& group:graph.groups)if(std::find(group.nodes.begin(),group.nodes.end(),id)!=group.nodes.end())
    {
        const float right=std::max(group.x+group.width,node->x+cardWidth(*node)+GridSize);
        const float bottom=std::max(group.y+group.height,node->y+Layout::height(*node,true)+GridSize);
        group.x=std::min(group.x,std::floor((node->x-GridSize)/GridSize)*GridSize);
        group.y=std::min(group.y,std::floor((node->y-2*GridSize)/GridSize)*GridSize);
        group.width=Layout::up(right-group.x);group.height=Layout::up(bottom-group.y);
    }
}
enum ResizeEdge{ResizeLeft=1,ResizeRight=2,ResizeTop=4,ResizeBottom=8};
ImVec2 screenPoint(const Graph& graph,ImVec2 origin,float x,float y);
int nodeResizeHit(const Graph& graph,const Node& node,ImVec2 origin,ImVec2 pointer)
{
    if(node.collapsed||reroute(node.kind))return 0;
    const auto p=screenPoint(graph,origin,node.x,node.y),q=add(p,ImVec2(cardWidth(node)*graph.zoom,cardHeight(node)*graph.zoom));
    const float outer=std::clamp(6*graph.zoom,4.0f,8.0f),inner=std::min(2.0f,2*graph.zoom);
    if(!contains(pointer,add(p,ImVec2(-outer,-outer)),add(q,ImVec2(outer,outer))))return 0;
    int edges=0;
    if(pointer.x<=p.x+inner)edges|=ResizeLeft;else if(pointer.x>=q.x-inner)edges|=ResizeRight;
    if(pointer.y<=p.y+inner)edges|=ResizeTop;else if(pointer.y>=q.y-inner)edges|=ResizeBottom;
    return edges;
}
void beginNodeResize(int index,int id,int edges)
{
    editor.resizeOriginal=editor.draft;editor.resizeNode=id;editor.resizeMask=edges;editor.resizeGraph=index;editor.resizeChanged=false;
    editor.dragStartMouse=ImGui::GetIO().MousePos;editor.portFocus[index]={};editor.autoFit[index]=false;
}
void updateNodeResize(Graph& graph,int index,ImVec2 pointer)
{
    auto* node=findNode(graph,editor.resizeNode);
    const auto* original=findNode(editor.resizeOriginal.graphs[index],editor.resizeNode);if(!node||!original)return;
    const float dx=snap((pointer.x-editor.dragStartMouse.x)/graph.zoom),dy=snap((pointer.y-editor.dragStartMouse.y)/graph.zoom);
    const auto before=*node;Node resized=*original;
    if((editor.resizeMask&(ResizeLeft|ResizeRight))&&dx!=0)
    {
        resized.width=std::clamp(snap(cardWidth(*original)+(editor.resizeMask&ResizeLeft?-dx:dx)),Layout::MinWidth,Layout::MaxWidth);
        if(editor.resizeMask&ResizeLeft)resized.x=snap(original->x+cardWidth(*original)-resized.width);
    }
    if((editor.resizeMask&(ResizeTop|ResizeBottom))&&dy!=0)
    {
        resized.height=std::clamp(snap(Layout::height(*original,true)+(editor.resizeMask&ResizeTop?-dy:dy)),Layout::up(Layout::minimumHeight(resized)),Layout::MaxHeight);
        if(editor.resizeMask&ResizeTop)resized.y=snap(original->y+Layout::height(*original,true)-resized.height);
    }
    if(before.x==resized.x&&before.y==resized.y&&before.width==resized.width&&before.height==resized.height)return;
    *node=std::move(resized);graph.groups=editor.resizeOriginal.graphs[index].groups;fitContainingGroups(graph,node->id);
    editor.resizeChanged=node->x!=original->x||node->y!=original->y||node->width!=original->width||node->height!=original->height;
    // Returning to the starting point also restores the exact original frames.
    if(!editor.resizeChanged)graph.groups=editor.resizeOriginal.graphs[index].groups;
    edited(false);
}
void finishNodeResize(bool confirm)
{
    if(!editor.resizeNode)return;
    const bool changed=editor.resizeChanged;auto original=std::move(editor.resizeOriginal);
    editor.resizeNode=editor.resizeMask=0;editor.resizeGraph=-1;editor.resizeChanged=false;
    if(changed)
    {
        if(confirm)rememberSnapshot(original);
        else{editor.draft=std::move(original);edited(false);}
    }
}
enum class NodeSizePreset{Automatic,Compact,Wide,FitContent};
void resizeSelection(Graph& graph,int index,NodeSizePreset preset)
{
    Graph resized=graph;bool changed=false;
    for(auto& node:resized.nodes)if(isSelected(index,node.id)&&!reroute(node.kind))
    {
        const float oldWidth=node.width,oldHeight=node.height;
        if(preset==NodeSizePreset::Automatic){node.width=node.height=0;}
        if(preset==NodeSizePreset::Compact){node.width=Layout::MinWidth;node.height=0;}
        if(preset==NodeSizePreset::Wide){node.width=384;node.height=0;}
        if(preset==NodeSizePreset::FitContent)
        {
            const auto& displays=editor.canvasRoutes[index].displays;
            const auto display=std::find_if(displays.begin(),displays.end(),[&](const NodeDisplay& state){return state.id==node.id;});
            if(!hasNodeSettings(node))node.height=0;
            else
            {
                float content=Layout::naturalSettingsHeight(node);
                if(display!=displays.end()&&display->settingsContent>0)content=display->settingsContent;
                node.height=std::clamp(Layout::up(nodeSettingsTop(node)+34+content),Layout::up(Layout::minimumHeight(node)),Layout::MaxHeight);
            }
        }
        if(node.width!=oldWidth||node.height!=oldHeight){changed=true;fitContainingGroups(resized,node.id);}
    }
    if(changed){remember();graph=std::move(resized);editor.autoFit[index]=false;edited(false);}
}
ImGuiMouseCursor resizeCursor(int edges)
{
    if(((edges&ResizeLeft)&&(edges&ResizeTop))||((edges&ResizeRight)&&(edges&ResizeBottom)))return ImGuiMouseCursor_ResizeNWSE;
    if(((edges&ResizeLeft)&&(edges&ResizeBottom))||((edges&ResizeRight)&&(edges&ResizeTop)))return ImGuiMouseCursor_ResizeNESW;
    return edges&(ResizeLeft|ResizeRight)?ImGuiMouseCursor_ResizeEW:ImGuiMouseCursor_ResizeNS;
}
bool primaryOutput(NodeKind kind)
{
    return kind==NodeKind::AimOutput||kind==NodeKind::AutoShootOutput||kind==NodeKind::RecoilOutput||kind==NodeKind::VisionOutput;
}
bool kindAllowed(const Graph& graph,NodeKind kind)
{
    if((kind==NodeKind::AimOutput&&graph.kind!=GraphKind::Aim)||
        (kind==NodeKind::AutoShootOutput&&graph.kind!=GraphKind::AutoShoot)||
        (kind==NodeKind::RecoilOutput&&graph.kind!=GraphKind::Recoil)||
        (kind==NodeKind::VisionOutput&&graph.kind!=GraphKind::Vision))return false;
    const bool visionOnly=(kind>=NodeKind::CaptureSource&&kind<=NodeKind::VisionOutput)||kind==NodeKind::ScreenshotOutput||kind==NodeKind::ImagePreview||
        (kind>=NodeKind::OverlayBoxes&&kind<=NodeKind::SetLabels);
    if(visionOnly&&graph.kind!=GraphKind::Vision)return false;
    return !primaryOutput(kind)||std::none_of(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.kind==kind;});
}
void openProperties(int index)
{
    editor.inspectorOpen=true;
    editor.focusSelection[index]=true;editor.autoFit[index]=false;
}
void collapseSelection(Graph& graph,int index);
void history(bool redo)
{
    auto& source = redo ? editor.redo : editor.undo;
    auto& destination = redo ? editor.undo : editor.redo;
    if (source.empty()) return;
    destination.push_back(editor.draft);
    editor.draft = source.back();
    source.pop_back();
    editor.cutAnimations.clear();
    cancelGestures();
    for (int i=0;i<static_cast<int>(GraphCount);++i) { clearSelection(i); editor.selectedGroup[i]=0; }
    edited();
}
void removeNode(Graph& graph, int id)
{
    remember();
    graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(), [id](const Link& link) {
        return link.fromNode == id || link.toNode == id;
    }), graph.links.end());
    graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(), [id](const Node& node) { return node.id == id; }), graph.nodes.end());
    for (auto& group : graph.groups)
        group.nodes.erase(std::remove(group.nodes.begin(), group.nodes.end(), id), group.nodes.end());
    edited();
}
void selectAll(Graph& graph, int tab)
{
    clearSelection(tab); editor.selectedGroup[tab]=0;
    for (const auto& node : graph.nodes) editor.selection[tab].push_back(node.id);
    for(const auto& tie:graph.ties)editor.tieSelection[tab].push_back(tie.id);
    if(!graph.ties.empty())editor.selectedTie[tab]=graph.ties.back().id;
    if (!editor.selection[tab].empty()) editor.selected[tab]=editor.selection[tab].back();
}
void deleteSelection(Graph& graph, int tab)
{
    if(editor.selection[tab].empty()&&!editor.tieSelection[tab].empty())
    {
        remember();for(const int id:editor.tieSelection[tab])detachTie(graph,id);
        clearSelection(tab);cancelGestures();edited(false);return;
    }
    if (editor.selection[tab].empty())
    {
        const int id=editor.selectedGroup[tab];
        if (!id) return;
        remember();
        graph.groups.erase(std::remove_if(graph.groups.begin(),graph.groups.end(),[id](const Group& g) {return g.id==id;}),graph.groups.end());
        editor.selectedGroup[tab]=0; edited(false); return;
    }
    remember();
    for(const int id:editor.tieSelection[tab])detachTie(graph,id);
    const auto selected=[tab](int id) {return isSelected(tab,id);};
    graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[&](const Link& l) {return selected(l.fromNode)||selected(l.toNode);}),graph.links.end());
    graph.nodes.erase(std::remove_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& n) {return selected(n.id);}),graph.nodes.end());
    for (auto& group:graph.groups)
        group.nodes.erase(std::remove_if(group.nodes.begin(),group.nodes.end(),selected),group.nodes.end());
    clearSelection(tab); cancelGestures(); edited();
}
void groupSelection(Graph& graph, int tab)
{
    if (editor.selection[tab].empty()) return;
    if (graph.groups.size()>=MaxGroups) {editor.message="Group limit reached (32).";editor.error=true;return;}
    float left=1e9f,top=1e9f,right=-1e9f,bottom=-1e9f;
    for (const auto& node:graph.nodes) if (isSelected(tab,node.id))
    {left=std::min(left,node.x);top=std::min(top,node.y);right=std::max(right,node.x+cardWidth(node));bottom=std::max(bottom,node.y+cardHeight(node));}
    if (right<left) return;
    int id=1;
    while (findGroup(graph,id)) ++id;
    remember();
    for (auto& group:graph.groups)
        group.nodes.erase(std::remove_if(group.nodes.begin(),group.nodes.end(),[tab](int n) {return isSelected(tab,n);}),group.nodes.end());
    Group group;
    group.id=id;group.title="Group "+std::to_string(id);
    group.x=std::floor((left-32)/GridSize)*GridSize;group.y=std::floor((top-64)/GridSize)*GridSize;
    group.width=std::ceil((right+32-group.x)/GridSize)*GridSize;
    group.height=std::ceil((bottom+32-group.y)/GridSize)*GridSize;
    group.nodes=editor.selection[tab]; graph.groups.push_back(std::move(group));
    clearSelection(tab);editor.selectedGroup[tab]=id;edited(false);
}
std::vector<WireTie> routingTies(const Graph& graph,const std::vector<Link>& links)
{
    std::vector<WireTie> ties;
    for(const auto& tie:graph.ties)if(std::any_of(links.begin(),links.end(),[&](const Link& link){return usesTie(link,tie.id);}))ties.push_back(tie);
    return ties;
}
void appendTies(Graph& destination,std::vector<Link>& links,std::vector<WireTie> ties,float dx,float dy)
{
    std::map<int,int> ids;
    for(auto tie:ties)
    {
        int id=1;while(findTie(destination,id))++id;ids[tie.id]=id;tie.id=id;tie.x=snap(tie.x+dx);tie.y=snap(tie.y+dy);destination.ties.push_back(tie);
    }
    for(auto& link:links)for(auto& id:link.ties)id=ids.at(id);
}
void remapNamedCopies(const Graph& graph,std::vector<Node>& nodes)
{
    std::vector<std::string> used;for(const auto& node:graph.nodes)if(variableSet(node.kind))used.push_back(textParameter(node,"name"));
    std::map<std::string,std::string> names;
    for(auto& node:nodes)if(variableSet(node.kind))
    {
        const auto original=textParameter(node,"name");auto name=original;int suffix=2;
        while(std::find(used.begin(),used.end(),name)!=used.end())
        {const auto ending=" "+std::to_string(suffix++);std::size_t length=std::min(original.size(),128-ending.size());while(length&&length<original.size()&&(static_cast<unsigned char>(original[length])&0xc0)==0x80)--length;name=original.substr(0,length)+ending;}
        used.push_back(name);names[original]=name;node.textParameters["name"]=name;
    }
    for(auto& node:nodes)if(variableGet(node.kind)){const auto found=names.find(textParameter(node,"name"));if(found!=names.end())node.textParameters["name"]=found->second;}
}
void duplicateSelection(Graph& graph,int tab,bool recordHistory=true)
{
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<Group> groups;
    const auto* selectedGroup=findGroup(graph,editor.selectedGroup[tab]);
    const auto selected=[&](int id){return isSelected(tab,id)||(selectedGroup&&std::find(selectedGroup->nodes.begin(),selectedGroup->nodes.end(),id)!=selectedGroup->nodes.end());};
    for (const auto& node:graph.nodes) if (selected(node.id)&&!primaryOutput(node.kind)) nodes.push_back(node);
    const auto copied=[&](int id){return std::any_of(nodes.begin(),nodes.end(),[id](const Node& n){return n.id==id;});};
    for (const auto& link:graph.links) if (copied(link.fromNode)&&copied(link.toNode)) links.push_back(link);
    for(auto group:graph.groups)
    {
        group.nodes.erase(std::remove_if(group.nodes.begin(),group.nodes.end(),[&](int id){return !copied(id);}),group.nodes.end());
        if(!group.nodes.empty())groups.push_back(std::move(group));
    }
    if (nodes.empty()) return;
    remapNamedCopies(graph,nodes);
    const auto ties=routingTies(graph,links);
    if (graph.nodes.size()+nodes.size()>MaxNodes || graph.links.size()+links.size()>MaxLinks || graph.ties.size()+ties.size()>MaxWireTies || graph.groups.size()+groups.size()>MaxGroups)
    {editor.message="Selection would exceed the node or link limit.";editor.error=true;return;}
    if(recordHistory)remember();clearSelection(tab);editor.selectedGroup[tab]=0;
    std::vector<std::pair<int,int>> ids;
    for (auto node:nodes)
    {
        const int old=node.id;node.id=nextNodeId(graph);node.x+=GridSize;node.y+=GridSize;
        ids.push_back({old,node.id});editor.selection[tab].push_back(node.id);graph.nodes.push_back(node);
    }
    for (auto& link:links)
    {
        for (const auto& id:ids) {if (link.fromNode==id.first) link.fromNode=-id.second;if (link.toNode==id.first) link.toNode=-id.second;}
        link.fromNode=-link.fromNode;link.toNode=-link.toNode;
    }
    const auto firstTie=graph.ties.size();appendTies(graph,links,ties,GridSize,GridSize);graph.links.insert(graph.links.end(),links.begin(),links.end());
    for(std::size_t i=firstTie;i<graph.ties.size();++i)editor.tieSelection[tab].push_back(graph.ties[i].id);
    if(!editor.tieSelection[tab].empty())editor.selectedTie[tab]=editor.tieSelection[tab].back();
    for(auto group:groups)
    {
        int id=1;while(findGroup(graph,id))++id;group.id=id;
        for(auto& member:group.nodes)for(const auto& mapping:ids)if(member==mapping.first){member=mapping.second;break;}
        group.x+=GridSize;group.y+=GridSize;graph.groups.push_back(std::move(group));
    }
    editor.selected[tab]=editor.selection[tab].back();edited();
}
bool sameLayout(const Graph& left,const Graph& right)
{
    if(left.nodes.size()!=right.nodes.size()||left.groups.size()!=right.groups.size()||left.ties.size()!=right.ties.size())return false;
    for(std::size_t i=0;i<left.nodes.size();++i)
    {
        const auto& a=left.nodes[i];const auto& b=right.nodes[i];
        if(a.id!=b.id||a.x!=b.x||a.y!=b.y||a.width!=b.width||a.height!=b.height)return false;
    }
    for(std::size_t i=0;i<left.groups.size();++i)
    {
        const auto& a=left.groups[i];const auto& b=right.groups[i];
        if(a.id!=b.id||a.x!=b.x||a.y!=b.y||a.width!=b.width||a.height!=b.height)return false;
    }
    for(std::size_t i=0;i<left.ties.size();++i)
    {
        const auto& a=left.ties[i];const auto& b=right.ties[i];
        if(a.id!=b.id||a.x!=b.x||a.y!=b.y||a.angle!=b.angle)return false;
    }
    return true;
}
void beginPointerMove(int tab)
{
    editor.moveOriginal=editor.draft;editor.moveGraph=tab;editor.pointerMove=true;editor.moved=false;
    editor.dragStartMouse=ImGui::GetIO().MousePos;
}
void updateLayoutMove(Graph& graph,ImVec2 pointer,bool commit=false)
{
    const ImVec2 delta((pointer.x-editor.dragStartMouse.x)/graph.zoom,(pointer.y-editor.dragStartMouse.y)/graph.zoom);
    const bool rotating=editor.dragTie&&editor.rotateTie;
    const float angle=std::round((pointer.x-editor.dragStartMouse.x-pointer.y+editor.dragStartMouse.y)*0.75f/15)*15;
    const bool displaced=delta.x!=0||delta.y!=0;
    const auto coordinate=[&](float start,float offset){return commit&&displaced?snap(start+offset):start+offset;};
    bool changed=false;
    const auto assign=[&](float& target,float value){if(target!=value){target=value;changed=true;}};
    if(!rotating)
    {
        for(const auto& start:editor.dragPositions)if(auto* node=findNode(graph,start.id))
        {assign(node->x,coordinate(start.x,delta.x));assign(node->y,coordinate(start.y,delta.y));}
        for(const auto& start:editor.dragGroupPositions)if(auto* group=findGroup(graph,start.id))
        {assign(group->x,coordinate(start.x,delta.x));assign(group->y,coordinate(start.y,delta.y));}
        if(auto* group=findGroup(graph,editor.dragGroup))
        {assign(group->x,coordinate(editor.dragStartGroup.x,delta.x));assign(group->y,coordinate(editor.dragStartGroup.y,delta.y));}
    }
    for(const auto& start:editor.dragTiePositions)if(auto* tie=findTie(graph,start.id))
    {
        if(rotating)assign(tie->angle,angle!=0?snapAngle(start.angle+angle):start.angle);
        else{assign(tie->x,coordinate(start.x,delta.x));assign(tie->y,coordinate(start.y,delta.y));}
    }
    if(auto* group=findGroup(graph,editor.resizeGroup))
    {
        float minWidth=128,minHeight=96;
        for(int id:group->nodes)if(const auto* node=findNode(graph,id))
        {minWidth=std::max(minWidth,node->x+cardWidth(*node)+16-group->x);minHeight=std::max(minHeight,node->y+cardHeight(*node)+16-group->y);}
        assign(group->width,std::max(Layout::up(minWidth),snap(editor.dragStartSize.x+delta.x)));
        assign(group->height,std::max(Layout::up(minHeight),snap(editor.dragStartSize.y+delta.y)));
    }
    if(!changed)return;
    editor.moved=true;editor.autoFit[static_cast<int>(graph.kind)]=false;
    if(distanceSquared(pointer,editor.dragStartMouse)>=ImGui::GetIO().MouseDragThreshold*ImGui::GetIO().MouseDragThreshold)editor.toggleSelectionOnRelease=0;
    edited(false);
}
void finishPointerMove(bool confirm)
{
    if(!editor.pointerMove)return;
    const int tab=editor.moveGraph;
    if(confirm)updateLayoutMove(editor.draft.graphs[tab],ImGui::GetIO().MousePos,true);
    const bool changed=!sameLayout(editor.draft.graphs[tab],editor.moveOriginal.graphs[tab]);
    editor.pointerMove=false;editor.moveGraph=-1;
    if(changed)
    {
        if(confirm)rememberSnapshot(editor.moveOriginal);
        else{editor.draft=editor.moveOriginal;edited(false);}
    }
}
void beginKeyboardMove(Graph& graph,int tab,bool duplicate)
{
    const auto* group=findGroup(graph,editor.selectedGroup[tab]);
    if(editor.selection[tab].empty()&&editor.tieSelection[tab].empty()&&!group)return;
    editor.moveOriginal=editor.draft;editor.moveSelection=editor.selection[tab];editor.moveTies=editor.tieSelection[tab];editor.moveGroup=editor.selectedGroup[tab];
    if(group&&editor.selection[tab].empty()&&editor.tieSelection[tab].empty())
    {
        editor.selection[tab]=group->nodes;if(!group->nodes.empty())editor.selected[tab]=group->nodes.back();
        for(const auto& tie:graph.ties)if(tieInGroup(graph,tie.id,*group))editor.tieSelection[tab].push_back(tie.id);
    }
    const auto count=graph.nodes.size();
    if(duplicate)
    {
        duplicateSelection(graph,tab,false);
        if(graph.nodes.size()==count){editor.selection[tab]=editor.moveSelection;editor.tieSelection[tab]=editor.moveTies;editor.selected[tab]=editor.selection[tab].empty()?0:editor.selection[tab].back();return;}
    }
    captureDragSelection(graph,tab,true);editor.keyboardMove=true;editor.moveGraph=tab;editor.confirmKeyboardMove=false;editor.moved=duplicate;
    editor.portFocus[tab]={};
    editor.dragStartMouse=ImGui::GetIO().MousePos;editor.autoFit[tab]=false;
}
void finishKeyboardMove(int tab,bool confirm)
{
    if(!editor.keyboardMove)return;
    if(confirm)
    {
        updateLayoutMove(editor.draft.graphs[tab],ImGui::GetIO().MousePos,true);
        if(!sameLayout(editor.draft.graphs[tab],editor.moveOriginal.graphs[tab]))rememberSnapshot(editor.moveOriginal);
    }
    else
    {
        editor.draft=editor.moveOriginal;editor.selection[tab]=editor.moveSelection;editor.tieSelection[tab]=editor.moveTies;
        editor.selectedGroup[tab]=editor.moveGroup;
        editor.selected[tab]=editor.selection[tab].empty()?0:editor.selection[tab].back();
        editor.selectedTie[tab]=editor.tieSelection[tab].empty()?0:editor.tieSelection[tab].back();edited(false);
    }
    cancelLeftGesture();
}
void invertSelection(const Graph& graph,int tab)
{
    const auto nodes=editor.selection[tab],ties=editor.tieSelection[tab];clearSelection(tab);editor.selectedGroup[tab]=0;
    for(const auto& node:graph.nodes)if(std::find(nodes.begin(),nodes.end(),node.id)==nodes.end())editor.selection[tab].push_back(node.id);
    for(const auto& tie:graph.ties)if(std::find(ties.begin(),ties.end(),tie.id)==ties.end())editor.tieSelection[tab].push_back(tie.id);
    if(!editor.selection[tab].empty())editor.selected[tab]=editor.selection[tab].back();
    if(!editor.tieSelection[tab].empty())editor.selectedTie[tab]=editor.tieSelection[tab].back();
}
void disableSelection(Graph& graph,int tab)
{
    bool any=false,all=true;for(const auto& node:graph.nodes)if(isSelected(tab,node.id)){any=true;all&=node.disabled;}
    if(!any)return;remember();for(auto& node:graph.nodes)if(isSelected(tab,node.id))node.disabled=!all;edited();
}
std::vector<Editor::PortFocus> nodePorts(const Graph& graph,int id,bool compatibleOnly=false)
{
    std::vector<Editor::PortFocus> result;const auto* node=findNode(graph,id);if(!node)return result;
    const auto& def=definition(node->kind);
    for(int side=0;side<2;++side)
    {
        const auto& pins=side?def.outputs:def.inputs;
        for(std::size_t pin=0;pin<pins.size();++pin)
        {
            if(!side&&!Layout::inputVisible(*node,pin))continue;
            if(compatibleOnly)
            {
                if((side!=0)==editor.wireOutput||id==editor.wireNode)continue;
                Link link=editor.wireOutput?Link{editor.wireNode,editor.wirePort,id,static_cast<int>(pin)}:Link{id,static_cast<int>(pin),editor.wireNode,editor.wirePort};
                std::string error;if(!canConnect(graph,link,error))continue;
            }
            result.push_back({id,static_cast<int>(pin),side!=0});
        }
    }
    return result;
}
void focusPort(const Graph& graph,int tab,Editor::PortFocus port)
{
    if(!findNode(graph,port.node))return;selectOne(tab,port.node);editor.portFocus[tab]=port;
    editor.focusSelection[tab]=true;editor.autoFit[tab]=false;
}
void nextPortNode(const Graph& graph,int tab,bool backwards)
{
    if(graph.nodes.empty())return;
    const auto focused=editor.portFocus[tab];const int current=focused.node?focused.node:editor.selected[tab];
    auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==current;});
    const int count=static_cast<int>(graph.nodes.size()),base=found==graph.nodes.end()?0:static_cast<int>(found-graph.nodes.begin());
    for(int offset=focused.node?1:0;offset<count+(focused.node?1:0);++offset)
    {
        const int index=(base+(backwards?-offset:offset)+count)%count;const auto ports=nodePorts(graph,graph.nodes[index].id,editor.keyboardWire);
        if(!ports.empty()){focusPort(graph,tab,backwards?ports.back():ports.front());return;}
    }
}
void cyclePort(const Graph& graph,int tab,int direction,bool endpoint=false)
{
    const auto focus=editor.portFocus[tab];const auto ports=nodePorts(graph,focus.node,editor.keyboardWire);if(ports.empty())return;
    auto found=std::find_if(ports.begin(),ports.end(),[&](const Editor::PortFocus& port){return port.port==focus.port&&port.output==focus.output;});
    const int count=static_cast<int>(ports.size()),index=found==ports.end()?0:static_cast<int>(found-ports.begin());
    focusPort(graph,tab,ports[endpoint?(direction<0?0:count-1):(index+direction+count)%count]);
}
void followPort(const Graph& graph,int tab,bool downstream)
{
    const auto focus=editor.portFocus[tab];
    auto found=std::find_if(graph.links.begin(),graph.links.end(),[&](const Link& link){return downstream?
        link.fromNode==focus.node&&(!focus.output||link.fromPort==focus.port):link.toNode==focus.node&&(focus.output||link.toPort==focus.port);});
    if(found==graph.links.end())return;
    focusPort(graph,tab,downstream?Editor::PortFocus{found->toNode,found->toPort,false}:Editor::PortFocus{found->fromNode,found->fromPort,true});
}
void activatePort(Graph& graph,int tab)
{
    const auto focus=editor.portFocus[tab];if(!findNode(graph,focus.node))return;
    if(!editor.keyboardWire)
    {editor.wireNode=focus.node;editor.wirePort=focus.port;editor.wireOutput=focus.output;editor.keyboardWire=true;return;}
    if(focus.node==editor.wireNode&&focus.port==editor.wirePort&&focus.output==editor.wireOutput)
    {editor.wireNode=0;editor.keyboardWire=false;return;}
    if(focus.output==editor.wireOutput){editor.message="Choose a compatible port on the opposite side.";editor.error=true;return;}
    const Link link=editor.wireOutput?Link{editor.wireNode,editor.wirePort,focus.node,focus.port}:Link{focus.node,focus.port,editor.wireNode,editor.wirePort};
    std::string error;if(!canConnectReroutes(graph,link,error)){editor.message=error;editor.error=true;return;}
    remember();adaptLooseReroutes(graph,link);graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[&](const Link& existing){return existing.toNode==link.toNode&&existing.toPort==link.toPort;}),graph.links.end());
    graph.links.push_back(link);editor.wireNode=0;editor.keyboardWire=false;edited();
}
void disconnectPort(Graph& graph,int tab)
{
    const auto focus=editor.portFocus[tab];const auto matches=[&](const Link& link){return focus.output?link.fromNode==focus.node&&link.fromPort==focus.port:link.toNode==focus.node&&link.toPort==focus.port;};
    if(std::none_of(graph.links.begin(),graph.links.end(),matches))return;
    remember();graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),matches),graph.links.end());edited();
}
constexpr const char* ClipboardHeader="SUNONE_NODE_SELECTION 1\n";
bool selectionText(const Graph& graph,int tab,std::string& text,std::string& error)
{
    Workspace fragment;
    for(size_t i=0;i<GraphCount;++i)fragment.graphs[i].kind=static_cast<GraphKind>(i);
    auto& selected=fragment.graphs[static_cast<size_t>(graph.kind)];
    const auto* selectedGroup=findGroup(graph,editor.selectedGroup[tab]);
    const auto included=[&](int id){return isSelected(tab,id)||(selectedGroup&&std::find(selectedGroup->nodes.begin(),selectedGroup->nodes.end(),id)!=selectedGroup->nodes.end());};
    for(const auto& node:graph.nodes)if(included(node.id))selected.nodes.push_back(node);
    if(selected.nodes.empty()){error="Select nodes to copy.";return false;}
    for(const auto& link:graph.links)if(included(link.fromNode)&&included(link.toNode))selected.links.push_back(link);
    selected.ties=routingTies(graph,selected.links);
    for(auto group:graph.groups)
    {
        group.nodes.erase(std::remove_if(group.nodes.begin(),group.nodes.end(),[&](int id){return !included(id);}),group.nodes.end());
        if(!group.nodes.empty())selected.groups.push_back(std::move(group));
    }
    std::string encoded;
    if(!serialize(fragment,encoded,error))return false;
    text=std::string(ClipboardHeader)+encoded;return true;
}
bool copySelection(const Graph& graph,int tab)
{
    std::string text,error;
    if(!selectionText(graph,tab,text,error)){editor.message=error;editor.error=true;return false;}
    ImGui::SetClipboardText(text.c_str());editor.message="Selection copied.";editor.error=false;return true;
}
void cutSelection(Graph& graph,int tab)
{
    if(!copySelection(graph,tab))return;
    const int groupId=editor.selectedGroup[tab];
    if(const auto* group=findGroup(graph,groupId))
    {
        for(const int id:group->nodes)if(!isSelected(tab,id))editor.selection[tab].push_back(id);
        if(!editor.selection[tab].empty())editor.selected[tab]=editor.selection[tab].back();
    }
    deleteSelection(graph,tab);
    if(groupId)
    {
        graph.groups.erase(std::remove_if(graph.groups.begin(),graph.groups.end(),[&](const Group& group){return group.id==groupId;}),graph.groups.end());
        edited(false);
    }
}
bool pasteSelectionText(Graph& graph,int tab,const std::string& text,ImVec2 position,std::string& error)
{
    const size_t headerLength=std::strlen(ClipboardHeader);
    if(text.size()>MaxWorkspaceBytes+headerLength||text.compare(0,headerLength,ClipboardHeader)!=0)
    {error="The clipboard does not contain a node selection.";return false;}
    Workspace fragment;
    if(!deserialize(text.substr(headerLength),fragment,error))return false;
    const Graph* source=nullptr;
    for(const auto& item:fragment.graphs)if(!item.nodes.empty())
    {if(source){error="A selection must belong to one graph.";return false;}source=&item;}
    if(!source){error="The copied selection is empty.";return false;}
    if(graph.nodes.size()+source->nodes.size()>MaxNodes||graph.links.size()+source->links.size()>MaxLinks||graph.groups.size()+source->groups.size()>MaxGroups||graph.ties.size()+source->ties.size()>MaxWireTies)
    {error="Pasting would exceed the graph limits.";return false;}
    Graph candidate=graph;
    float left=source->nodes.front().x,top=source->nodes.front().y;
    for(const auto& node:source->nodes){left=std::min(left,node.x);top=std::min(top,node.y);}
    ImVec2 delta(snap(position.x-left),snap(position.y-top));
    for(size_t attempt=0;attempt<MaxNodes;++attempt)
    {
        const bool overlaps=std::any_of(source->nodes.begin(),source->nodes.end(),[&](const Node& copied){
            return std::any_of(graph.nodes.begin(),graph.nodes.end(),[&](const Node& existing){return existing.x==snap(copied.x+delta.x)&&existing.y==snap(copied.y+delta.y);});});
        if(!overlaps)break;
        delta.x+=GridSize;delta.y+=GridSize;
    }
    std::map<int,int> ids;
    std::vector<int> added;
    auto copiedNodes=source->nodes;remapNamedCopies(graph,copiedNodes);
    for(auto node:copiedNodes)
    {
        if(!kindAllowed(candidate,node.kind)){error="This selection contains an output or provider that cannot be pasted into this graph.";return false;}
        const int id=nextNodeId(candidate);ids[node.id]=id;node.id=id;
        node.x=snap(node.x+delta.x);node.y=snap(node.y+delta.y);added.push_back(id);candidate.nodes.push_back(std::move(node));
    }
    auto links=source->links;appendTies(candidate,links,source->ties,delta.x,delta.y);
    for(auto& link:links)
    {link.fromNode=ids.at(link.fromNode);link.toNode=ids.at(link.toNode);candidate.links.push_back(link);}
    for(auto group:source->groups)
    {
        int id=1;while(findGroup(candidate,id))++id;group.id=id;
        float x=1e9f,y=1e9f,right=-1e9f,bottom=-1e9f;
        for(auto& member:group.nodes)
        {
            member=ids.at(member);const auto* node=findNode(candidate,member);
            x=std::min(x,node->x);y=std::min(y,node->y);
            right=std::max(right,node->x+cardWidth(*node));bottom=std::max(bottom,node->y+cardHeight(*node));
        }
        group.x=std::floor((x-32)/GridSize)*GridSize;group.y=std::floor((y-64)/GridSize)*GridSize;
        group.width=std::ceil((right+32-group.x)/GridSize)*GridSize;
        group.height=std::ceil((bottom+32-group.y)/GridSize)*GridSize;candidate.groups.push_back(std::move(group));
    }
    auto workspace=editor.draft;workspace.graphs[tab]=candidate;
    if(!validate(workspace,error))return false;
    remember();graph=std::move(candidate);clearSelection(tab);editor.selection[tab]=std::move(added);
    for(const auto& tie:graph.ties)if(std::none_of(editor.undo.back().graphs[tab].ties.begin(),editor.undo.back().graphs[tab].ties.end(),[&](const WireTie& previous){return previous.id==tie.id;}))editor.tieSelection[tab].push_back(tie.id);
    if(!editor.tieSelection[tab].empty())editor.selectedTie[tab]=editor.tieSelection[tab].back();
    editor.selected[tab]=editor.selection[tab].back();editor.selectedGroup[tab]=0;cancelGestures();edited();return true;
}
void pasteSelection(Graph& graph,int tab)
{
    const char* clipboard=ImGui::GetClipboardText();std::string error;
    if(!clipboard||!pasteSelectionText(graph,tab,clipboard?clipboard:"",editor.pastePosition[tab],error))
    {editor.message=error.empty()?"The clipboard is empty.":error;editor.error=true;}
}
void bypassSelection(Graph& graph,int tab)
{
    bool eligible=false,allBypassed=true;
    for (const auto& node:graph.nodes) if (isSelected(tab,node.id)&&canBypass(node.kind)) {eligible=true;allBypassed&=node.bypassed;}
    if (!eligible) return;
    remember();
    for (auto& node:graph.nodes) if (isSelected(tab,node.id)&&canBypass(node.kind)) node.bypassed=!allBypassed;
    edited();
}
ImVec2 screenPoint(const Graph& graph, ImVec2 origin, float x, float y)
{
    return ImVec2(origin.x + graph.panX + x*graph.zoom, origin.y + graph.panY + y*graph.zoom);
}
ImVec2 rerouteCenter(const Graph& graph,ImVec2 origin,const Node& node)
{
    return screenPoint(graph,origin,node.x+32,node.y+16);
}
float rerouteRadius(float zoom){return std::max(3.0f,5.0f*zoom);}
float visualEase(const char* key,float target)
{
    auto* storage=ImGui::GetStateStorage();const auto id=ImGui::GetID(key);
    const float previous=storage->GetFloat(id,0);
    const float dt=std::clamp(ImGui::GetIO().DeltaTime,0.0f,1.0f/15.0f);
    const float value=target+(previous-target)*std::exp(-14*dt);
    storage->SetFloat(id,value);return value;
}
void drawReroutePoint(ImDrawList* draw,ImVec2 center,float radius,ImU32 color,bool selected,float hover)
{
    const ImU32 rgb=color&0x00ffffff;
    if(hover>0.001f||selected)
        draw->AddCircleFilled(center,radius+3+(selected?1:0),rgb|IM_COL32(0,0,0,static_cast<int>(22*hover+(selected?22:0))));
    draw->AddCircleFilled(add(center,ImVec2(0,1)),radius+1,IM_COL32(0,0,0,100));
    draw->AddCircleFilled(center,radius+1,IM_COL32(27,29,33,255));
    draw->AddCircle(center,radius,selected?Cyan:color,0,selected?1.8f:1.4f);
    draw->AddCircleFilled(center,std::max(1.5f,radius-2),color);
}
ImVec2 portPoint(const Graph& graph, ImVec2 origin, const Node& node, bool output, int pin)
{
    const auto& def=definition(node.kind);
    const auto count=output?def.outputs.size():Layout::inputCount(node);
    const auto row=output?static_cast<std::size_t>(pin):Layout::inputRow(node,static_cast<std::size_t>(pin));
    const float y=reroute(node.kind)?16.0f:node.collapsed?renderHeight(graph,node)*0.5f+(static_cast<float>(row)-(static_cast<float>(count)-1)*0.5f)*12.0f:HeaderHeight+15.0f+row*PinSpacing;
    return screenPoint(graph, origin, node.x + (output ? cardWidth(node) : 0), node.y+y);
}
void fitGraph(Graph& graph, ImVec2 size)
{
    if (graph.nodes.empty()&&graph.ties.empty()) { graph.panX = graph.panY = 40; graph.zoom = 1; return; }
    float left=1e9f,top=1e9f,right=-1e9f,bottom=-1e9f;
    for (const auto& node : graph.nodes)
    {
        left = std::min(left, node.x); top = std::min(top, node.y);
        right = std::max(right, node.x + cardWidth(node)); bottom = std::max(bottom, node.y + cardHeight(node));
    }
    for (const auto& group:graph.groups)
    {left=std::min(left,group.x);top=std::min(top,group.y);right=std::max(right,group.x+group.width);bottom=std::max(bottom,group.y+group.height);}
    for(const auto& tie:graph.ties){left=std::min(left,tie.x-48);top=std::min(top,tie.y-48);right=std::max(right,tie.x+48);bottom=std::max(bottom,tie.y+48);}
    graph.zoom = std::clamp(std::min((size.x-44)/(right-left), (size.y-44)/(bottom-top)), 0.25f, 1.2f);
    graph.panX = (size.x-(right-left)*graph.zoom)*0.5f-left*graph.zoom;
    graph.panY = (size.y-(bottom-top)*graph.zoom)*0.5f-top*graph.zoom;
}
void frameSelection(Graph& graph,int index,ImVec2 size,bool changeZoom)
{
    Graph selected=graph;selected.nodes.clear();selected.groups.clear();selected.ties.clear();
    const auto* group=findGroup(graph,editor.selectedGroup[index]);
    for(const auto& node:graph.nodes)if(isSelected(index,node.id)||(group&&std::find(group->nodes.begin(),group->nodes.end(),node.id)!=group->nodes.end()))selected.nodes.push_back(node);
    for(const auto& tie:graph.ties)if(isTieSelected(index,tie.id)||(group&&tieInGroup(graph,tie.id,*group)))selected.ties.push_back(tie);
    if(selected.nodes.empty()&&selected.ties.empty())return;
    if(group)selected.groups.push_back(*group);
    if(changeZoom)
    {fitGraph(selected,size);graph.zoom=selected.zoom;graph.panX=selected.panX;graph.panY=selected.panY;}
    else
    {
        float left=1e9f,top=1e9f,right=-1e9f,bottom=-1e9f;
        for(const auto& node:selected.nodes){left=std::min(left,node.x);top=std::min(top,node.y);right=std::max(right,node.x+cardWidth(node));bottom=std::max(bottom,node.y+cardHeight(node));}
        for(const auto& tie:selected.ties){left=std::min(left,tie.x-48);top=std::min(top,tie.y-48);right=std::max(right,tie.x+48);bottom=std::max(bottom,tie.y+48);}
        graph.panX=size.x*0.5f-(left+right)*0.5f*graph.zoom;graph.panY=size.y*0.5f-(top+bottom)*0.5f*graph.zoom;
    }
    editor.autoFit[index]=false;edited(false);
}
void arrangeGraph(Graph& graph,int index)
{
    Graph arranged=graph;
    if(Layout::arrange(arranged))
    {
        remember();graph=std::move(arranged);cancelGestures();editor.cutAnimations.clear();edited(false);
    }
    editor.fit[index]=editor.autoFit[index]=true;editor.focusSelection[index]=false;
}
void revealSelection(Graph& graph,int index,ImVec2 size)
{
    const auto* node=findNode(graph,editor.selected[index]);if(!node)return;
    const auto revealAxis=[](float start,float extent,float available,float pan)
    {
        constexpr float margin=18;
        if(extent>available-2*margin)return (available-extent)*0.5f-start;
        if(start+pan<margin)return margin-start;
        if(start+pan+extent>available-margin)return available-margin-start-extent;
        return pan;
    };
    const float x=revealAxis(node->x*graph.zoom,cardWidth(*node)*graph.zoom,size.x,graph.panX);
    const float y=revealAxis(node->y*graph.zoom,renderHeight(graph,*node)*graph.zoom,size.y,graph.panY);
    if(x!=graph.panX||y!=graph.panY){graph.panX=x;graph.panY=y;edited(false);}
}
void appendCubic(std::vector<ImVec2>& path,ImVec2 controlA,ImVec2 controlB,ImVec2 end,int steps)
{
    const auto start=path.back();
    for(int i=1;i<=steps;++i)
    {
        const float t=static_cast<float>(i)/steps,u=1-t;
        path.emplace_back(u*u*u*start.x+3*u*u*t*controlA.x+3*u*t*t*controlB.x+t*t*t*end.x,
            u*u*u*start.y+3*u*u*t*controlA.y+3*u*t*t*controlB.y+t*t*t*end.y);
    }
}
ImVec2 multiply(ImVec2 value,float scalar);
ImVec2 appendWireSpline(std::vector<ImVec2>& path,ImVec2 end,ImVec2 startDirection,ImVec2 endDirection,float tangent,float scale,WireBend bend,int steps=32)
{
    const auto start=path.back();
    constexpr float neutral=WireBend{}.roundness;
    const float curvature=bend.roundness<=neutral?0.28f+0.72f*bend.roundness/neutral:
        1+0.72f*(bend.roundness-neutral)/(1-neutral);
    const auto a=add(start,multiply(startDirection,tangent*curvature)),b=add(end,multiply(endDirection,-tangent*curvature));
    const auto midpoint=[](ImVec2 left,ImVec2 right){return multiply(add(left,right),0.5f);};
    const auto outerA=midpoint(start,a),outerB=midpoint(b,end),shared=midpoint(a,b);
    auto innerA=midpoint(outerA,shared),innerB=midpoint(shared,outerB);
    const auto center=midpoint(innerA,innerB);
    const float twist=bend.twist*std::clamp(std::sqrt(distanceSquared(start,end))*0.28f,48*scale,240*scale);
    innerA.y-=twist;innerB.y+=twist;
    // Shared center and tangent keep the two cubic halves smooth. Only the
    // endpoint/tie anchors and the user's two parameters determine the shape.
    appendCubic(path,outerA,innerA,center,steps);
    appendCubic(path,innerB,outerB,end,steps);
    return center;
}
std::vector<ImVec2> wirePath(ImVec2 start,ImVec2 end,float scale,WireBend bend={})
{
    std::vector<ImVec2> path;path.reserve(65);path.push_back(start);
    const float tangent=std::clamp(std::abs(end.x-start.x)*0.45f+std::sqrt(distanceSquared(start,end))*0.08f,42*scale,240*scale);
    appendWireSpline(path,end,ImVec2(1,0),ImVec2(1,0),tangent,scale,bend);
    return path;
}
float wireLength(const std::vector<ImVec2>& path)
{
    float length=0;for(size_t i=1;i<path.size();++i)length+=std::sqrt(distanceSquared(path[i-1],path[i]));return length;
}
ImVec2 wirePosition(const std::vector<ImVec2>& path,float distance)
{
    if(path.empty())return {};
    distance=std::max(0.0f,distance);
    for(size_t i=1;i<path.size();++i)
    {
        const float segment=std::sqrt(distanceSquared(path[i-1],path[i]));
        if(distance<=segment&&segment>0)
        {
            const float part=distance/segment;
            return ImVec2(path[i-1].x+(path[i].x-path[i-1].x)*part,path[i-1].y+(path[i].y-path[i-1].y)*part);
        }
        distance-=segment;
    }
    return path.back();
}
void advanceWireAnimation(WireAnimation& animation,float length,float scale,const SignalEvent& event,double now,int frame,bool allowStart=true)
{
    if(animation.frame==frame)return;
    animation.frame=frame;
    if(animation.started>=0)animation.phase=std::clamp((now-animation.born)/animation.duration,0.0,1.0);
    if(event.sequence!=animation.signal)
    {
        animation.signal=event.sequence;
        if(allowStart&&animation.phase>=1&&now-animation.started>=MinSignalInterval&&event.sequence&&event.strength>0&&event.age>=0&&event.age<SignalDuration)
        {
            animation.born=now-event.age;animation.started=now;
            animation.duration=std::clamp(length/std::max(1.0f,720*scale),0.12f,0.36f);
            animation.strength=std::clamp(event.strength,0.0f,1.0f);
            animation.phase=std::clamp((now-animation.born)/animation.duration,0.0,1.0);
        }
    }
    animation.activity=animation.strength*static_cast<float>((1-animation.phase)*std::min(1.0,animation.phase*10+0.2));
}
ImVec2 routedPosition(const RoutedWire& route,float phase)
{
    if(route.path.empty())return {};
    if(route.cumulative.size()!=route.path.size())return wirePosition(route.path,phase*wireLength(route.path));
    const float at=std::clamp(phase,0.0f,1.0f)*route.cumulative.back();
    auto next=std::lower_bound(route.cumulative.begin(),route.cumulative.end(),at);
    const std::size_t i=static_cast<std::size_t>(next-route.cumulative.begin());
    if(!i)return route.path.front();if(i>=route.path.size())return route.path.back();
    const float length=route.cumulative[i]-route.cumulative[i-1];const float t=length>0?(at-route.cumulative[i-1])/length:0;
    return add(route.path[i-1],ImVec2((route.path[i].x-route.path[i-1].x)*t,(route.path[i].y-route.path[i-1].y)*t));
}
void wire(ImDrawList* draw,const RoutedWire& route,ImU32 color,float thickness,const WireAnimation& animation,float scale)
{
    const auto& path=route.path;
    draw->AddPolyline(path.data(),static_cast<int>(path.size()),color,0,thickness);
    if(animation.activity<0.001f)return;
    const ImU32 rgb=color&0x00ffffff;
    const ImVec2 position=routedPosition(route,static_cast<float>(animation.phase));
    draw->AddCircleFilled(position,std::clamp(5*scale,3.0f,6.0f),rgb|IM_COL32(0,0,0,static_cast<int>(30*animation.activity)),8);
    draw->AddCircleFilled(position,std::clamp(2.2f*scale,1.2f,3.0f),IM_COL32(220,245,255,static_cast<int>(230*animation.activity)),8);
}
void previewWire(ImDrawList* draw,ImVec2 start,ImVec2 end,ImU32 color)
{
    // A free endpoint follows the pointer in either direction. Routing an
    // unfinished link as a return connection creates a loop around its source.
    const float bend=(end.x-start.x)*0.45f;
    draw->AddBezierCubic(start,add(start,ImVec2(bend,0)),add(end,ImVec2(-bend,0)),end,color,2);
    draw->AddCircleFilled(end,3,color);
}
float wireDistance(const std::vector<ImVec2>& path,ImVec2 mouse)
{
    float nearest=1e20f;
    for(size_t i=1;i<path.size();++i)
    {
        const ImVec2 a=path[i-1],b=path[i];const float dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;
        const float fraction=length>0?std::clamp(((mouse.x-a.x)*dx+(mouse.y-a.y)*dy)/length,0.0f,1.0f):0;
        nearest=std::min(nearest,distanceSquared(mouse,ImVec2(a.x+fraction*dx,a.y+fraction*dy)));
    }
    return nearest;
}
constexpr float Pi=3.14159265358979323846f;
ImVec2 multiply(ImVec2 v,float scale){return ImVec2(v.x*scale,v.y*scale);}
float dot(ImVec2 a,ImVec2 b){return a.x*b.x+a.y*b.y;}
float cross(ImVec2 a,ImVec2 b){return a.x*b.y-a.y*b.x;}
float normalizedAngle(float angle){return std::remainder(angle,360.0f);}
int tieWireCount(const Graph& graph,int id)
{return static_cast<int>(std::count_if(graph.links.begin(),graph.links.end(),[&](const Link& link){return usesTie(link,id);}));}
float tieSpacing(int count){return count>1?std::min(4.0f,72.0f/(count-1)):4.0f;}
float tieHalfSpan(const Graph& graph,int id){const int count=tieWireCount(graph,id);return std::max(10.0f,(count-1)*tieSpacing(count)*0.5f+5);}
ImVec2 tieDirection(const WireTie& tie){const float a=tie.angle*Pi/180;return ImVec2(std::cos(a),std::sin(a));}
ImVec2 tieLane(const Graph& graph,const Link& link,const WireTie& tie,ImVec2 origin)
{
    int count=0,rank=0;
    for(const auto& candidate:graph.links)if(usesTie(candidate,tie.id))
    {if(linkKey(candidate)==linkKey(link))rank=count;++count;}
    const auto direction=tieDirection(tie);const ImVec2 axis(-direction.y,direction.x);
    return add(screenPoint(graph,origin,tie.x,tie.y),multiply(axis,(rank-(count-1)*0.5f)*tieSpacing(count)*graph.zoom));
}
void drawWireTie(ImDrawList* draw,const Graph& graph,const WireTie& tie,ImVec2 origin,bool selected,float hover)
{
    const auto center=screenPoint(graph,origin,tie.x,tie.y),direction=tieDirection(tie);const ImVec2 axis(-direction.y,direction.x);
    const float scale=graph.zoom,span=tieHalfSpan(graph,tie.id)*scale;
    const auto head=add(center,multiply(axis,-span)),tail=add(center,multiply(axis,span));
    const ImU32 color=selected?Cyan:IM_COL32(137+static_cast<int>(30*hover),158+static_cast<int>(25*hover),177+static_cast<int>(20*hover),255);
    draw->AddLine(add(head,ImVec2(1,1)),add(tail,ImVec2(1,1)),IM_COL32(0,0,0,230),std::max(3.0f,5*scale));
    draw->AddLine(head,tail,IM_COL32(36,44,53,255),std::max(2.2f,3.5f*scale));
    draw->AddLine(head,tail,color,std::max(1.2f,1.7f*scale));
    const auto end=add(tail,add(multiply(axis,5*scale),multiply(direction,4*scale)));
    draw->AddBezierCubic(tail,add(tail,multiply(axis,5*scale)),add(end,multiply(direction,-4*scale)),end,color,std::max(1.2f,1.7f*scale));
    const auto u=multiply(direction,std::max(1.7f,3.2f*scale)),v=multiply(axis,std::max(1.7f,3.5f*scale));
    const ImVec2 cap[]={add(head,add(multiply(u,-1),multiply(v,-1))),add(head,add(u,multiply(v,-1))),add(head,add(u,v)),add(head,add(multiply(u,-1),v))};
    draw->AddConvexPolyFilled(cap,4,color);draw->AddPolyline(cap,4,IM_COL32(28,34,41,255),ImDrawFlags_Closed,std::max(1.0f,scale));
    if(selected)draw->AddCircle(center,std::max(7.0f,7*scale),IM_COL32(96,205,255,85),0,1);
}
ImVec2 appendCurve(std::vector<ImVec2>& path,ImVec2 end,ImVec2 startDirection,ImVec2 endDirection,float scale,WireBend bend={})
{
    const float tangent=std::clamp(std::sqrt(distanceSquared(path.back(),end))*0.42f,12*scale,240*scale);
    return appendWireSpline(path,end,startDirection,endDirection,tangent,scale,bend,16);
}
RoutedWire tiedWirePath(const Graph& graph,const Link& link,ImVec2 origin,ImVec2 start,ImVec2 end)
{
    RoutedWire route;route.path.push_back(start);ImVec2 previousDirection(1,0);float longest=-1;
    const auto span=[&](ImVec2 target,ImVec2 direction)
    {
        const float length=distanceSquared(route.path.back(),target);
        const auto center=appendCurve(route.path,target,previousDirection,direction,graph.zoom,link.bend);
        if(length>longest){longest=length;route.bendPoint=center;}
    };
    for(std::size_t i=0;i<link.ties.size();++i)
    {
        const auto* tie=findTie(graph,link.ties[i]);if(!tie)continue;
        const auto point=tieLane(graph,link,*tie,origin);auto direction=tieDirection(*tie);
        const auto next=i+1<link.ties.size()?tieLane(graph,link,*findTie(graph,link.ties[i+1]),origin):end;
        const ImVec2 displacement(next.x-route.path.back().x,next.y-route.path.back().y);
        if(dot(displacement,direction)<0)direction=multiply(direction,-1);
        span(add(point,multiply(direction,-8*graph.zoom)),direction);
        route.path.push_back(point);route.tieDistances.push_back(wireLength(route.path));
        route.path.push_back(add(point,multiply(direction,8*graph.zoom)));previousDirection=direction;
    }
    span(end,ImVec2(1,0));return route;
}
CanvasRoutes& prepareRoutes(const Graph& graph,int index,ImVec2 origin)
{
    auto& cache=editor.canvasRoutes[index];auto& key=cache.scratch;key.clear();
    key.reserve(8+graph.nodes.size()*7+graph.links.size()*7+graph.ties.size()*4);
    const auto integer=[&](std::uint64_t value){key.push_back(value);};
    const auto scalar=[&](float value){std::uint32_t bits;std::memcpy(&bits,&value,sizeof(bits));integer(bits);};
    integer(graph.nodes.size());integer(graph.links.size());integer(graph.ties.size());
    for(const auto& node:graph.nodes)
    {integer(node.id);integer(static_cast<int>(node.kind));scalar(node.x);scalar(node.y);scalar(cardWidth(node));scalar(renderHeight(graph,node));}
    for(const auto& link:graph.links)
    {integer(link.fromNode);integer(link.fromPort);integer(link.toNode);integer(link.toPort);integer(link.ties.size());for(const int tie:link.ties)integer(tie);scalar(link.bend.roundness);scalar(link.bend.twist);}
    for(const auto& tie:graph.ties){integer(tie.id);scalar(tie.x);scalar(tie.y);scalar(tie.angle);}
    const bool rebuild=cache.key!=key;
    const ImVec2 translation=add(origin,ImVec2(graph.panX,graph.panY));
    if(rebuild)
    {
        cache.key.swap(key);++cache.rebuilds;cache.routes.resize(graph.links.size());cache.connections.assign(graph.nodes.size(),{});
        bool ordered=cache.displays.size()==graph.nodes.size();
        for(std::size_t i=0;ordered&&i<graph.nodes.size();++i)ordered=cache.displays[i].id==graph.nodes[i].id;
        if(!ordered)
        {
            auto previous=std::move(cache.displays);cache.displays.resize(graph.nodes.size());
            for(std::size_t i=0;i<graph.nodes.size();++i)
            {
                const auto found=std::find_if(previous.begin(),previous.end(),[&](const auto& state){return state.id==graph.nodes[i].id;});
                if(found!=previous.end())cache.displays[i]=std::move(*found);cache.displays[i].id=graph.nodes[i].id;
            }
        }
        for(std::size_t i=0;i<graph.links.size();++i)
        {
            const auto& link=graph.links[i];auto& route=cache.routes[i];
            const auto* from=findNode(graph,link.fromNode);const auto* to=findNode(graph,link.toNode);
            if(!from||!to||link.fromPort<0||link.toPort<0||static_cast<std::size_t>(link.fromPort)>=definition(from->kind).outputs.size()||
                static_cast<std::size_t>(link.toPort)>=definition(to->kind).inputs.size()){route=RoutedWire{};continue;}
            const auto start=reroute(from->kind)?rerouteCenter(graph,origin,*from):portPoint(graph,origin,*from,true,link.fromPort);
            const auto end=reroute(to->kind)?rerouteCenter(graph,origin,*to):portPoint(graph,origin,*to,false,link.toPort);
            if(link.ties.empty()){route=RoutedWire{};route.path=wirePath(start,end,graph.zoom,link.bend);route.bendPoint=route.path[32];}
            else route=tiedWirePath(graph,link,origin,start,end);
            route.localBendPoint=multiply(ImVec2(route.bendPoint.x-translation.x,route.bendPoint.y-translation.y),1/graph.zoom);
            route.from=static_cast<std::size_t>(from-graph.nodes.data());route.to=static_cast<std::size_t>(to-graph.nodes.data());
            cache.connections[route.from][1]|=std::uint64_t{1}<<link.fromPort;cache.connections[route.to][0]|=std::uint64_t{1}<<link.toPort;
            route.localPath.reserve(route.path.size());route.cumulative.reserve(route.path.size());
            float walked=0;
            for(const auto point:route.path)
            {
                const ImVec2 local((point.x-translation.x)/graph.zoom,(point.y-translation.y)/graph.zoom);
                if(!route.localPath.empty())walked+=std::sqrt(distanceSquared(local,route.localPath.back()));
                else route.localMinimum=route.localMaximum=local;
                route.localMinimum.x=std::min(route.localMinimum.x,local.x);route.localMinimum.y=std::min(route.localMinimum.y,local.y);
                route.localMaximum.x=std::max(route.localMaximum.x,local.x);route.localMaximum.y=std::max(route.localMaximum.y,local.y);
                route.localPath.push_back(local);route.cumulative.push_back(walked);
            }
            for(const float at:route.tieDistances)route.localTieDistances.push_back(at/graph.zoom);
        }
        for(auto it=editor.wireAnimations.begin();it!=editor.wireAnimations.end();)
        {
            const auto& edge=it->first;
            if(edge[0]==index&&std::none_of(graph.links.begin(),graph.links.end(),[&](const Link& link){return link.fromNode==edge[1]&&link.fromPort==edge[2]&&link.toNode==edge[3]&&link.toPort==edge[4];}))it=editor.wireAnimations.erase(it);else++it;
        }
    }
    if(rebuild||cache.zoom!=graph.zoom||cache.translation.x!=translation.x||cache.translation.y!=translation.y)
    {
        for(auto& route:cache.routes)
        {
            for(std::size_t i=0;i<route.localPath.size();++i)route.path[i]=add(translation,multiply(route.localPath[i],graph.zoom));
            for(std::size_t i=0;i<route.localTieDistances.size();++i)route.tieDistances[i]=route.localTieDistances[i]*graph.zoom;
            route.length=route.cumulative.empty()?0:route.cumulative.back()*graph.zoom;
            route.minimum=add(translation,multiply(route.localMinimum,graph.zoom));route.maximum=add(translation,multiply(route.localMaximum,graph.zoom));
            route.bendPoint=add(translation,multiply(route.localBendPoint,graph.zoom));
        }
        cache.translation=translation;cache.zoom=graph.zoom;
    }
    return cache;
}
bool segmentIntersection(ImVec2 a,ImVec2 b,ImVec2 c,ImVec2 d,float& fraction)
{
    const ImVec2 u(b.x-a.x,b.y-a.y),v(d.x-c.x,d.y-c.y),delta(c.x-a.x,c.y-a.y);
    const float denominator=cross(u,v);if(std::abs(denominator)<0.00001f)return false;
    const float first=cross(delta,v)/denominator,second=cross(delta,u)/denominator;
    if(first<0||first>1||second<0||second>1)return false;fraction=second;return true;
}
void collectWireHits(const Graph& graph,const std::vector<RoutedWire>& routes,ImVec2 from,ImVec2 to,std::vector<TieHit>& hits)
{
    for(std::size_t wire=0;wire<routes.size();++wire)
    {
        const auto key=linkKey(graph.links[wire]);
        if(std::any_of(hits.begin(),hits.end(),[&](const TieHit& hit){return hit.link==key;}))continue;
        const auto& route=routes[wire];float walked=0;
        for(std::size_t segment=1;segment<route.path.size();++segment)
        {
            const auto a=route.path[segment-1],b=route.path[segment];float fraction=0;const float length=std::sqrt(distanceSquared(a,b));
            if(segmentIntersection(from,to,a,b,fraction))
            {
                const float distance=walked+fraction*length;
                const auto slot=static_cast<std::size_t>(std::upper_bound(route.tieDistances.begin(),route.tieDistances.end(),distance)-route.tieDistances.begin());
                hits.push_back({key,slot,ImVec2(a.x+(b.x-a.x)*fraction,a.y+(b.y-a.y)*fraction)});break;
            }
            walked+=length;
        }
    }
}
bool createWireTie(Graph& graph,int tab,const std::vector<TieHit>& hits,ImVec2 position,float angle,std::string& error)
{
    if(hits.size()<2){error="Cross at least two wires to create a tie.";return false;}
    if(graph.ties.size()>=MaxWireTies){error="Wire tie limit reached (64).";return false;}
    Graph candidate=graph;int id=1;while(findTie(candidate,id))++id;std::vector<std::array<int,4>> added;
    for(const auto& hit:hits)
    {
        const auto found=std::find_if(candidate.links.begin(),candidate.links.end(),[&](const Link& link){return linkKey(link)==hit.link;});
        if(found==candidate.links.end()||std::find(added.begin(),added.end(),hit.link)!=added.end()){error="A selected wire no longer exists or is repeated.";return false;}
        if(found->ties.size()>=MaxTiesPerLink){error="A wire can pass through at most 16 ties.";return false;}
        found->ties.insert(found->ties.begin()+std::min(hit.slot,found->ties.size()),id);added.push_back(hit.link);
    }
    candidate.ties.push_back({id,snap(position.x),snap(position.y),snapAngle(angle)});
    auto workspace=editor.draft;workspace.graphs[tab]=candidate;if(!validate(workspace,error))return false;
    remember();graph=std::move(candidate);selectOneTie(tab,id);editor.autoFit[tab]=false;edited(false);return true;
}
float nearestPathDistance(const std::vector<ImVec2>& path,ImVec2 point)
{
    float nearest=1e20f,walked=0,at=0;
    for(std::size_t i=1;i<path.size();++i)
    {
        const ImVec2 a=path[i-1],b=path[i],delta(b.x-a.x,b.y-a.y);const float squared=dot(delta,delta);
        const float t=squared>0?std::clamp(((point.x-a.x)*delta.x+(point.y-a.y)*delta.y)/squared,0.0f,1.0f):0;
        const float distance=distanceSquared(point,add(a,multiply(delta,t))),length=std::sqrt(squared);
        if(distance<nearest){nearest=distance;at=walked+t*length;}walked+=length;
    }
    return at;
}
void queueCutAnimation(const Graph& graph,int index,const Link& link,const RoutedWire& route,ImVec2 cut)
{
    if(route.path.size()<2)return;
    const auto* source=findNode(graph,link.fromNode);if(!source)return;
    CutAnimation animation;animation.graph=index;animation.born=ImGui::GetTime();animation.color=pinColor(definition(source->kind).outputs[link.fromPort].type);
    const float total=wireLength(route.path),at=nearestPathDistance(route.path,cut);
    const auto a=wirePosition(route.path,std::max(0.0f,at-1)),b=wirePosition(route.path,std::min(total,at+1));
    const float length=std::sqrt(distanceSquared(a,b));const ImVec2 normal=length>0.001f?ImVec2(-(b.y-a.y)/length,(b.x-a.x)/length):ImVec2(0,1);
    animation.minimum=ImVec2(1e9f,1e9f);animation.maximum=ImVec2(-1e9f,-1e9f);
    for(int side=0;side<2;++side)
    {
        auto& piece=animation.pieces[side];const float pixels=side?total-at:at;piece.length=pixels/graph.zoom;piece.recoil=multiply(normal,side?1.0f:-1.0f);
        const int samples=std::clamp(static_cast<int>(std::ceil(piece.length/12)),2,48);piece.points.reserve(samples+1);
        for(int i=0;i<=samples;++i)
        {
            const float distance=side?total-pixels*i/samples:pixels*i/samples;const auto point=wirePosition(route.path,distance);
            const ImVec2 world((point.x-editor.canvasOrigin[index].x-graph.panX)/graph.zoom,(point.y-editor.canvasOrigin[index].y-graph.panY)/graph.zoom);
            piece.points.push_back(world);animation.minimum.x=std::min(animation.minimum.x,world.x-16);animation.minimum.y=std::min(animation.minimum.y,world.y-16);
            animation.maximum.x=std::max(animation.maximum.x,world.x+16);animation.maximum.y=std::max(animation.maximum.y,world.y+16);
        }
    }
    if(editor.cutAnimations.size()>=MaxCutAnimations)editor.cutAnimations.erase(editor.cutAnimations.begin());
    editor.cutAnimations.push_back(std::move(animation));
}
void cutPiecePath(const CutPiece& piece,float progress,const Graph& graph,ImVec2 origin,std::vector<ImVec2>& path)
{
    path.clear();if(piece.points.size()<2||piece.length<0.001f||progress>=1)return;
    progress=std::clamp(progress,0.0f,1.0f);const float remaining=std::pow(1-progress,3.0f);
    const float end=remaining*(piece.points.size()-1),kick=std::sin(progress*Pi)*12;
    const auto point=[&](ImVec2 world,float fraction){const float weight=remaining>0.00001f?std::clamp(fraction/remaining,0.0f,1.0f):0;const auto recoil=multiply(piece.recoil,kick*weight*weight);return screenPoint(graph,origin,world.x+recoil.x,world.y+recoil.y);};
    const auto whole=static_cast<std::size_t>(end);path.reserve(piece.points.size());
    for(std::size_t i=0;i<=whole&&i<piece.points.size();++i)path.push_back(point(piece.points[i],static_cast<float>(i)/(piece.points.size()-1)));
    if(whole+1<piece.points.size())
    {
        const float fraction=end-whole;const auto a=piece.points[whole],b=piece.points[whole+1];
        path.push_back(point(ImVec2(a.x+(b.x-a.x)*fraction,a.y+(b.y-a.y)*fraction),remaining));
    }
}
void drawCutAnimations(ImDrawList* draw,const Graph& graph,int index,ImVec2 origin,ImVec2 end)
{
    const double now=ImGui::GetTime();
    editor.cutAnimations.erase(std::remove_if(editor.cutAnimations.begin(),editor.cutAnimations.end(),[&](const CutAnimation& animation){return now-animation.born>=CutDuration;}),editor.cutAnimations.end());
    for(const auto& animation:editor.cutAnimations)
    {
        if(animation.graph!=index)continue;
        const auto p=screenPoint(graph,origin,animation.minimum.x,animation.minimum.y),q=screenPoint(graph,origin,animation.maximum.x,animation.maximum.y);
        if(q.x<origin.x||q.y<origin.y||p.x>end.x||p.y>end.y)continue;
        const float t=std::clamp(static_cast<float>((now-animation.born)/CutDuration),0.0f,1.0f),alpha=(1-t)*(1-t);
        const ImU32 color=(animation.color&0x00ffffff)|IM_COL32(0,0,0,static_cast<int>(235*alpha));
        for(const auto& piece:animation.pieces)
        {
            cutPiecePath(piece,t,graph,origin,editor.cutScratch);
            if(editor.cutScratch.size()>1)draw->AddPolyline(editor.cutScratch.data(),static_cast<int>(editor.cutScratch.size()),color,0,std::max(0.8f,(2.2f-0.8f*t)*graph.zoom));
        }
    }
}
void insertReroute(Graph& graph,int index,ImVec2 position,int tab,std::size_t tieSplit=MaxTiesPerLink)
{
    if (index<0 || static_cast<size_t>(index)>=graph.links.size() || graph.nodes.size()>=MaxNodes || graph.links.size()>=MaxLinks) return;
    const Link previous=graph.links[index];
    const Node* source=findNode(graph,previous.fromNode);
    if (!source) return;
    const auto type=definition(source->kind).outputs[previous.fromPort].type;
    const int id=nextNodeId(graph);
    remember();
    const auto rerouteNode=defaultNode(rerouteKind(type),id,snap(position.x-32),snap(position.y-16));
    graph.nodes.push_back(rerouteNode);
    graph.links[index]={previous.fromNode,previous.fromPort,id,0};
    graph.links.push_back({id,0,previous.toNode,previous.toPort});
    graph.links[index].bend=graph.links.back().bend=previous.bend;
    tieSplit=std::min(tieSplit,previous.ties.size());
    graph.links[index].ties.assign(previous.ties.begin(),previous.ties.begin()+tieSplit);
    graph.links.back().ties.assign(previous.ties.begin()+tieSplit,previous.ties.end());
    for(auto& group:graph.groups)
        if(std::find(group.nodes.begin(),group.nodes.end(),previous.fromNode)!=group.nodes.end()&&
            std::find(group.nodes.begin(),group.nodes.end(),previous.toNode)!=group.nodes.end())
        {
            group.nodes.push_back(id);
            const float right=std::max(group.x+group.width,rerouteNode.x+64+32);
            const float bottom=std::max(group.y+group.height,rerouteNode.y+32+32);
            group.x=std::min(group.x,rerouteNode.x-32);group.y=std::min(group.y,rerouteNode.y-32);
            group.width=std::ceil((right-group.x)/GridSize)*GridSize;
            group.height=std::ceil((bottom-group.y)/GridSize)*GridSize;
            break;
        }
    selectOne(tab,id);
    edited();
}
void addLooseReroute(Graph& graph,int tab,ImVec2 position)
{
    if(graph.nodes.size()>=MaxNodes){editor.error=true;editor.message="Node limit reached (128).";return;}
    const auto node=defaultNode(NodeKind::RerouteFloat,nextNodeId(graph),snap(position.x-32),snap(position.y-16));
    int owner=0;float area=std::numeric_limits<float>::max();
    for(const auto& group:graph.groups)
        if(contains(position,ImVec2(group.x,group.y+32),ImVec2(group.x+group.width,group.y+group.height))&&group.width*group.height<area)
        {owner=group.id;area=group.width*group.height;}
    remember();graph.nodes.push_back(node);
    if(auto* group=findGroup(graph,owner)){group->nodes.push_back(node.id);fitContainingGroups(graph,node.id);}
    cancelGestures();selectOne(tab,node.id);editor.autoFit[tab]=false;edited();
}
bool modelPathInput(NodeKind kind,const char* key)
{
    return (kind==NodeKind::Inference||kind==NodeKind::DepthInference)&&key&&std::strcmp(key,"model_path")==0;
}
std::string valueText(const Value* value)
{
    if (!value || !value->valid) return value&&!value->error.empty()?"ERROR":"";
    if(value->preview)return std::string(typeName(value->preview->sourceType))+"  "+std::to_string(value->preview->sourceWidth)+" x "+std::to_string(value->preview->sourceHeight);
    if (value->type == ValueType::Boolean) return value->boolean ? "TRUE" : "FALSE";
    if (value->type == ValueType::Integer) return std::to_string(value->integer);
    if(value->type==ValueType::String)
    {if(value->text.empty())return "\"\"";std::size_t end=std::min<std::size_t>(160,value->text.size());while(end&&end<value->text.size()&&(static_cast<unsigned char>(value->text[end])&0xc0)==0x80)--end;return value->text.substr(0,end);}
    if(value->type==ValueType::Monitors)return value->monitors?std::to_string(value->monitors->size())+" monitors":"";
    if(value->type==ValueType::Monitor)return value->monitor?value->monitor->name+"  "+std::to_string(value->monitor->width)+" x "+std::to_string(value->monitor->height):"";
    if(value->type==ValueType::OverlayStyle)return value->settings?"STYLE":"";
    if(value->type==ValueType::LabelOptions)return value->settings?"LABEL OPTIONS":"";
    if (value->type == ValueType::Detections)
    {
        if (!value->detections) return "NO FRAME";
        return std::to_string(value->detections->objects.size()) + " detections";
    }
    if (value->type == ValueType::Target)
    {
        if (!value->target.valid) return "NO TARGET";
        char text[72];
        std::snprintf(text, sizeof(text), "%s  %.0f, %.0f", value->target.observed ? "OBSERVED" : "TRACKED", value->target.point.x, value->target.point.y);
        return text;
    }
    if (value->type == ValueType::Movement)
    {
        int dx = 0, dy = 0;
        for (const auto& step : value->movement.steps) { dx += step.dx; dy += step.dy; }
        char text[72];
        std::snprintf(text, sizeof(text), "%d, %d  |  %zu steps", dx, dy, value->movement.steps.size());
        return text;
    }
    if(value->type==ValueType::Frame||value->type==ValueType::Depth)
    {
        if(!value->frame)return "NO FRAME";
        return std::to_string(value->frame->width)+" x "+std::to_string(value->frame->height)+(value->frame->gpu?" GPU":" CPU");
    }
    if(value->type==ValueType::Mask)
        return value->mask?std::to_string(value->mask->width)+" x "+std::to_string(value->mask->height)+" MASK":"NO MASK";
    char text[72];
    if (value->type == ValueType::Point) std::snprintf(text, sizeof(text), "%.1f, %.1f", value->point.x, value->point.y);
    else std::snprintf(text, sizeof(text), "%.3f", value->number);
    return text;
}
const InputDefaultDisplay& inputDefault(NodeDisplay& display,const Node& node,std::size_t pin,std::uint64_t connectedInputs=0)
{
    const auto& def=definition(node.kind);
    if(display.defaultKind!=node.kind||display.defaults.size()!=def.inputs.size())
    {
        display.defaultKind=node.kind;display.defaults.clear();display.defaults.resize(def.inputs.size());
        for(std::size_t i=0;i<def.inputs.size();++i)if(const auto* key=def.inputs[i].defaultParameter)
        {
            for(const auto& spec:def.parameters)if(std::strcmp(key,spec.key)==0)display.defaults[i].parameter=&spec;
            for(const auto& spec:def.textParameters)if(std::strcmp(key,spec.key)==0)display.defaults[i].textParameter=&spec;
        }
    }
    auto& cached=display.defaults[pin];const auto& input=def.inputs[pin];
    InputDefaultKind kind=InputDefaultKind::Required;double number=0;std::string_view source;
    if(cached.textParameter)
    {
        kind=InputDefaultKind::Text;
        const auto found=node.textParameters.find(cached.textParameter->key);
        source=found==node.textParameters.end()?std::string_view(cached.textParameter->defaultValue):std::string_view(found->second);
    }
    else if(cached.parameter)
    {
        kind=input.type==ValueType::Boolean?InputDefaultKind::Boolean:InputDefaultKind::Number;
        const auto found=node.parameters.find(cached.parameter->key);
        number=found==node.parameters.end()?cached.parameter->defaultValue:found->second;
    }
    else if(const auto found=node.inputValues.find(static_cast<int>(pin));found!=node.inputValues.end())
    {kind=input.type==ValueType::Boolean?InputDefaultKind::Boolean:InputDefaultKind::Number;number=found->second;}
    else if(const auto found=node.inputTexts.find(static_cast<int>(pin));found!=node.inputTexts.end())
    {kind=InputDefaultKind::Text;source=found->second;}
    else if(node.kind==NodeKind::CaptureSource&&pin==0)
    {kind=InputDefaultKind::Local;source="Source settings";}
    else if(node.kind==NodeKind::FilterCircle&&pin==1)kind=InputDefaultKind::Center;
    else if(node.kind==NodeKind::MouseDelta&&pin==2)
    {
        bool wired=false;
        for(std::size_t i=0;i<def.inputs.size();++i)
            if(def.inputs[i].defaultParameter&&std::strcmp(def.inputs[i].defaultParameter,"legacy_center")==0)
                wired=(connectedInputs&(std::uint64_t{1}<<i))!=0;
        if(wired){kind=InputDefaultKind::Local;source="Center setting";}
        else if(parameter(node,"legacy_center")!=0)kind=InputDefaultKind::Center;
    }
    else if(((node.kind==NodeKind::CollectDataset||node.kind==NodeKind::RenderOverlay||node.kind==NodeKind::DatasetOutput)&&pin>=2&&pin<def.dataInputs)||
        (node.kind==NodeKind::OverlayOutput&&pin>=2&&pin<=4)||
        (node.kind>=NodeKind::OverlayBoxes&&node.kind<=NodeKind::OverlayCorrection&&pin==0)||
        (node.kind==NodeKind::ImagePreview&&pin>=1&&pin<=4))kind=InputDefaultKind::Empty;
    else if(!primaryOutput(node.kind)&&node.kind!=NodeKind::CollectDataset&&node.kind!=NodeKind::RenderOverlay&&node.kind!=NodeKind::ScreenshotOutput)
    {
        if(input.type==ValueType::Boolean)kind=InputDefaultKind::Boolean;
        else if(input.type==ValueType::Float||input.type==ValueType::Integer)kind=InputDefaultKind::Number;
        else if(input.type==ValueType::String)kind=InputDefaultKind::Text;
    }
    if(cached.kind==kind&&cached.number==number&&std::string_view(cached.source)==source)return cached;
    cached.kind=kind;cached.number=number;cached.source.assign(source.data()?source.data():"",source.size());
    cached.limit=-1;cached.naturalWidth=-1;cached.text.clear();cached.fullText.clear();
    switch(kind)
    {
    case InputDefaultKind::Boolean:cached.text=number!=0?"true":"false";break;
    case InputDefaultKind::Number:
    {
        char text[64],full[64];std::snprintf(text,sizeof(text),input.type==ValueType::Integer?"%.0f":"%.6g",number);
        std::snprintf(full,sizeof(full),"%.12g",number);cached.text=text;cached.fullText=full;
        if(cached.parameter&&cached.parameter->choices)
        {
            auto choices=std::string_view(cached.parameter->choices);int choice=static_cast<int>(number-cached.parameter->minValue);
            while(choice-->0){const auto split=choices.find('|');if(split==std::string_view::npos){choices={};break;}choices.remove_prefix(split+1);}
            choices=choices.substr(0,choices.find('|'));
            if(!choices.empty()){cached.text.assign(choices.data(),choices.size());cached.fullText=cached.text+" ("+cached.fullText+")";}
        }
        break;
    }
    case InputDefaultKind::Text:
        for(const char character:source)
            if(character=='\n')cached.text+="\\n";else if(character=='\r')cached.text+="\\r";else if(character=='\t')cached.text+="\\t";else cached.text+=character;
        cached.fullText='"'+cached.text+'"';
        if(modelPathInput(node.kind,input.defaultParameter))
        {
            if(source.empty())cached.text="Select model...";
            else try{const auto name=std::filesystem::u8path(source).filename().u8string();if(!name.empty())cached.text=name;}catch(const std::exception&){}
        }
        else if(cached.text.empty())cached.text="\"\"";
        break;
    case InputDefaultKind::Center:cached.text="Center";cached.fullText="Capture center";break;
    case InputDefaultKind::Local:cached.text="Local";cached.fullText=cached.source;break;
    case InputDefaultKind::Empty:cached.text="None";cached.fullText="Optional: no data";break;
    default:cached.text="-";cached.fullText="Required input; no default";break;
    }
    if(cached.fullText.empty())cached.fullText=cached.text;
    return cached;
}
const InputDefaultDisplay& fitInputDefault(NodeDisplay& display,const Node& node,std::size_t pin,std::uint64_t connectedInputs,ImFont* font,float fontSize,float limit)
{
    inputDefault(display,node,pin,connectedInputs);auto& cached=display.defaults[pin];
    if(cached.font==font&&cached.fontSize==fontSize&&cached.limit==limit)return cached;
    cached.font=font;cached.fontSize=fontSize;cached.limit=limit;cached.shown=cached.text;
    const auto width=[&](){return font->CalcTextSizeA(fontSize,std::numeric_limits<float>::max(),0,cached.shown.c_str()).x;};
    cached.width=width();
    if(cached.width>limit+0.01f)
    {
        const float suffix=font->CalcTextSizeA(fontSize,std::numeric_limits<float>::max(),0,"...").x;
        if(suffix>limit){cached.shown.clear();cached.width=0;}
        else
        {
            const char* remaining=cached.text.c_str();
            font->CalcTextSizeA(fontSize,std::max(0.001f,limit-suffix+0.001f),0,cached.text.c_str(),nullptr,&remaining);
            cached.shown=cached.text.substr(0,static_cast<std::size_t>(remaining-cached.text.c_str()))+"...";cached.width=width();
        }
    }
    return cached;
}
void inputDefaultTooltip(const Pin& pin,const InputDefaultDisplay& value,bool editable=false)
{
    ImGui::BeginTooltip();ImGui::Text("Input %s : %s",pin.name,typeName(pin.type));
    ImGui::PushTextWrapPos(420);
    if(value.kind==InputDefaultKind::Required||value.kind==InputDefaultKind::Empty)ImGui::TextDisabled("%s",value.fullText.c_str());
    else ImGui::Text("Default: %s",value.fullText.c_str());
    if(value.kind!=InputDefaultKind::Required)ImGui::TextDisabled("Used when this input is not connected.");
    if(editable)ImGui::TextUnformatted(pin.type==ValueType::Boolean?"Click the value to toggle.":value.parameter&&value.parameter->choices?"Click the value to choose an option.":pin.type==ValueType::String?"Click the value to edit text or choose an option.":"Drag the value, or Ctrl+click to type a precise value.");
    ImGui::TextUnformatted("Drag the socket to connect. Alt+click it to disconnect.");
    ImGui::PopTextWrapPos();ImGui::EndTooltip();
}
void dataTooltip(const Value& value)
{
    if(!value.error.empty()){ImGui::SetTooltip("%s",value.error.c_str());return;}
    if(!value.valid)return;
    if(value.type==ValueType::String){ImGui::BeginTooltip();ImGui::PushTextWrapPos(460);ImGui::TextUnformatted(value.text.c_str());ImGui::PopTextWrapPos();ImGui::EndTooltip();return;}
    if(value.type==ValueType::Monitor||value.type==ValueType::Monitors)
    {
        ImGui::BeginTooltip();const auto show=[](const MonitorInfo& m){ImGui::Text("%s%s",m.name.c_str(),m.primary?" (primary)":"");ImGui::Text("Index %d | %d x %d | %d, %d",m.index,m.width,m.height,m.x,m.y);};
        if(value.monitor)show(*value.monitor);if(value.monitors)for(std::size_t i=0;i<std::min<std::size_t>(16,value.monitors->size());++i){if(i)ImGui::Separator();show((*value.monitors)[i]);}
        ImGui::EndTooltip();return;
    }
    if(value.type==ValueType::Target)
    {
        const auto& target=value.target;
        ImGui::BeginTooltip();
        if(!target.valid)ImGui::TextUnformatted("No target");
        else
        {
            ImGui::Text("Class %d  |  Confidence %.1f%%",target.classId,target.confidence*100);
            ImGui::Text("Track %d  |  %s",target.trackId,target.observed?"Observed":"Predicted");
            ImGui::Text("Box %.0f, %.0f  |  %.0f x %.0f",target.x,target.y,target.width,target.height);
            ImGui::Text("Point %.2f, %.2f",target.point.x,target.point.y);
            ImGui::Text("Velocity %.1f, %.1f px/s",target.velocityX,target.velocityY);
            ImGui::Text("Missed %d frames  |  Age %.1f ms",target.missedFrames,target.ageSeconds*1000);
        }
        ImGui::EndTooltip();
    }
    else if(value.type==ValueType::Detections&&value.detections)
    {
        ImGui::BeginTooltip();
        ImGui::Text("%zu detections",value.detections->objects.size());
        const size_t shown=std::min<size_t>(8,value.detections->objects.size());
        for(size_t i=0;i<shown;++i)
        {
            const auto& detection=value.detections->objects[i];
            ImGui::Text("Class %d  |  %.1f%%  |  %.0f x %.0f",detection.classId,detection.confidence*100,detection.width,detection.height);
        }
        if(shown<value.detections->objects.size())ImGui::TextDisabled("%zu more",value.detections->objects.size()-shown);
        ImGui::EndTooltip();
    }
    else if((value.type==ValueType::Frame||value.type==ValueType::Depth)&&value.frame)
        ImGui::SetTooltip("%d x %d  |  %s  |  Frame %lld",value.frame->width,value.frame->height,
            value.frame->gpu?"GPU":"CPU",static_cast<long long>(value.frame->version));
    else if(value.type==ValueType::Mask&&value.mask)
        ImGui::SetTooltip("%d x %d  |  Mask frame %lld",value.mask->width,value.mask->height,static_cast<long long>(value.mask->version));
}
const NodeValue* nodeSample(int graph,int id,bool fresh)
{
    if(!fresh)return nullptr;
    const auto& index=editor.liveIndex[graph];
    const auto found=std::lower_bound(index.begin(),index.end(),id,[](const auto& entry,int key){return entry.first<key;});
    if(found!=index.end()&&found->first==id)return &editor.live.graphs[graph][found->second];
    return nullptr;
}
const Value* nodeValue(int graph,int id,bool fresh)
{
    const auto* sample=nodeSample(graph,id,fresh);return sample?&sample->value:nullptr;
}
bool telemetryFresh(int graph,std::chrono::steady_clock::time_point now)
{
    const auto sampled=editor.live.graphSampledAt[graph];
    return editor.live.graphRevisions[graph]==editor.observedRevision&&sampled>editor.appliedAt&&
        now-sampled<std::chrono::milliseconds(300);
}
void invalidateActivity(std::size_t graph)
{
    for(auto& display:editor.canvasRoutes[graph].displays)
    {
        display.sequence=display.textSequence=0;display.textAt=display.lastEvent=display.flashAt=display.lastFlash=display.rapidUntil=-1;
        display.quickEvents=0;display.strength=0;
    }
    for(auto it=editor.wireAnimations.begin();it!=editor.wireAnimations.end();)
        if(it->first[0]==static_cast<int>(graph))it=editor.wireAnimations.erase(it);else++it;
}
void refreshTelemetry()
{
    const auto revisions=editor.live.graphRevisions;
    editor.live=service().telemetry();editor.telemetryAt=ImGui::GetTime();
    for(std::size_t graph=0;graph<GraphCount;++graph)
    {
        auto& index=editor.liveIndex[graph];index.clear();index.reserve(editor.live.graphs[graph].size());
        for(std::size_t i=0;i<editor.live.graphs[graph].size();++i)index.emplace_back(editor.live.graphs[graph][i].nodeId,i);
        std::sort(index.begin(),index.end());
        if(revisions[graph]!=editor.live.graphRevisions[graph])
            invalidateActivity(graph);
    }
}
float signalGlow(const NodeValue* sample,std::chrono::steady_clock::time_point now)
{
    if(!sample||!sample->signalSequence)return 0;
    const float age=std::chrono::duration<float>(now-sample->signaledAt).count();
    if(age<0||age>=SignalDuration)return 0;
    const float fade=1-age/SignalDuration;return fade*fade*(sample->signalActive?1.0f:0.45f);
}
float updateNodeDisplay(NodeDisplay& display,const Node& node,const NodeValue* sample,bool needText,bool crowded,
    double now,std::chrono::steady_clock::time_point sampledNow)
{
    const Value* value=sample?&sample->value:nullptr;
    const bool available=value&&value->valid;
    const std::string empty;const auto& error=value?value->error:empty;
    const bool force=display.kind!=node.kind||display.disabled!=node.disabled||display.available!=available||
        display.error!=error||((node.kind==NodeKind::Number||node.kind==NodeKind::Integer||node.kind==NodeKind::Boolean)&&display.constant!=node.value);
    if(!sample||node.disabled)
    {display.sequence=0;display.rapidUntil=display.flashAt=display.lastFlash=display.lastEvent=-1;display.quickEvents=0;}
    else if(sample->signalSequence!=display.sequence)
    {
        const float age=std::chrono::duration<float>(sampledNow-sample->signaledAt).count();
        if(sample->signalSequence&&age>=0&&age<SignalDuration)
        {
            const double eventTime=now-age;
            const bool previous=display.sequence&&sample->signalSequence>display.sequence;
            if(previous&&(sample->signalSequence-display.sequence>1||
                (eventTime>display.lastEvent&&eventTime-display.lastEvent<MinSignalInterval)))++display.quickEvents;
            else display.quickEvents=0;
            if(previous&&sample->signalSequence-display.sequence>2)display.quickEvents=3;
            if(display.quickEvents>=2)
            {
                display.rapidUntil=now+SignalDuration;
                display.strength=std::max(display.strength,sample->signalActive?1.0f:0.45f);
            }
            else if(now-display.lastFlash>=MinSignalInterval)
            {display.flashAt=eventTime;display.lastFlash=now;display.strength=sample->signalActive?1.0f:0.45f;++display.flashes;}
            display.lastEvent=eventTime;
        }
        display.sequence=sample->signalSequence;
    }
    const bool rapid=now<display.rapidUntil;
    const auto sequence=sample?sample->signalSequence:0;
    const double textInterval=crowded||rapid?0.1:0.05;
    if(needText&&(force||display.textAt<0||(display.textSequence!=sequence&&now-display.textAt>=textInterval)))
    {
        display.text=valueText(value);
        if(!available&&!error.empty()&&(node.kind==NodeKind::Inference||node.kind==NodeKind::DepthInference))
            display.text=error.substr(0,error.find_first_of("\r\n"));
        if(node.kind==NodeKind::Number){Value number;number.type=ValueType::Number;number.number=node.value;display.text=valueText(&number);}
        if(node.kind==NodeKind::Integer){Value value;value.type=ValueType::Integer;value.integer=static_cast<std::int32_t>(node.value);display.text=valueText(&value);}
        if(node.kind==NodeKind::Boolean)display.text=node.value!=0?"TRUE":"FALSE";
        if(node.disabled)display.text="DISABLED";
        display.kind=node.kind;display.disabled=node.disabled;display.available=available;display.error=error;display.constant=node.value;
        display.textSequence=sequence;display.textAt=now;++display.formats;
    }
    if(rapid)return 0.16f*display.strength*std::clamp(static_cast<float>((display.rapidUntil-now)/0.1),0.0f,1.0f);
    if(display.flashAt<0||now-display.flashAt>=SignalDuration)return 0;
    const float fade=std::clamp(1-static_cast<float>((now-display.flashAt)/SignalDuration),0.0f,1.0f);
    return fade*fade*display.strength;
}
SignalEvent wireSignal(const NodeValue* source,const NodeValue* target,int pin,std::chrono::steady_clock::time_point now)
{
    if(!source)return {};
    const bool consumed=target&&(target->readInputs&(std::uint64_t{1}<<pin))!=0;
    return {source->signalSequence,std::chrono::duration<float>(now-source->signaledAt).count(),consumed?(source->signalActive?1.0f:0.45f):0.0f};
}
bool matches(const Definition& def)
{
    if (!editor.search[0]) return true;
    std::string text = std::string(def.title) + " " + compactTitle(def.kind) + " " + def.category + " " + def.description;
    std::string query = editor.search;
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
    for (auto& c : query) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
    return text.find(query) != std::string::npos;
}
bool catalogConnection(const Graph& graph,const Node& node,Link& connection)
{
    if(!editor.catalogWireNode)return true;
    const auto* anchor=findNode(graph,editor.catalogWireNode);
    if(!anchor)return false;
    const auto& anchorPins=editor.catalogWireOutput?definition(anchor->kind).outputs:definition(anchor->kind).inputs;
    if(editor.catalogWirePort<0||static_cast<size_t>(editor.catalogWirePort)>=anchorPins.size())return false;
    const auto type=anchorPins[editor.catalogWirePort].type;
    const auto& candidates=editor.catalogWireOutput?definition(node.kind).inputs:definition(node.kind).outputs;
    Graph draft=graph;draft.nodes.push_back(node);
    for(size_t port=0;port<candidates.size();++port)if(looseReroute(graph,*anchor)||looseReroute(draft,node)||
        (editor.catalogWireOutput?canConnectTypes(type,candidates[port].type):canConnectTypes(candidates[port].type,type)))
    {
        const Link link=editor.catalogWireOutput?Link{anchor->id,editor.catalogWirePort,node.id,static_cast<int>(port)}:
            Link{node.id,static_cast<int>(port),anchor->id,editor.catalogWirePort};
        std::string error;if(canConnectReroutes(draft,link,error)){connection=link;return true;}
    }
    return false;
}
bool addCatalogNode(Graph& graph,int index,NodeKind kind,std::string& error)
{
    if(graph.nodes.size()>=MaxNodes){error="Node limit reached (128).";return false;}
    if(!kindAllowed(graph,kind)){error="This node cannot be added to the current graph.";return false;}
    Node node=defaultNode(kind,nextNodeId(graph),snap(editor.catalogPosition.x),snap(editor.catalogPosition.y));
    if(variableSet(kind)){std::vector<Node> named{node};remapNamedCopies(graph,named);node=std::move(named[0]);}
    if(kind==NodeKind::Boolean)node.value=1;
    Link connection;
    if(!catalogConnection(graph,node,connection)){error="No compatible port is available.";return false;}
    Graph candidate=graph;candidate.nodes.push_back(node);
    if(editor.catalogWireNode)
    {
        adaptLooseReroutes(candidate,connection);
        candidate.links.erase(std::remove_if(candidate.links.begin(),candidate.links.end(),[&](const Link& link){
            return link.toNode==connection.toNode&&link.toPort==connection.toPort;}),candidate.links.end());
        candidate.links.push_back(connection);
    }
    auto workspace=editor.draft;workspace.graphs[index]=candidate;
    if(!validate(workspace,error))return false;
    remember();graph=std::move(candidate);selectOne(index,node.id);editor.catalogWireNode=0;edited();return true;
}
struct CatalogPath {const char* group;const char* subgroup;};
CatalogPath catalogPath(const Definition& def)
{
    if(variableGet(def.kind)||variableSet(def.kind))return {"Get / Set",typeName(def.outputs[0].type)};
    if(def.kind>=NodeKind::SelectBoolean&&def.kind<=NodeKind::SelectMonitors)return {"Values","Selection"};
    if(!std::strcmp(def.category,"Monitors"))return {"System","Monitors"};
    using K=NodeKind;
    switch(def.kind)
    {
    case K::AimHeld:case K::ShootHeld:case K::ZoomHeld:return {"Inputs","Control buttons"};
    case K::KeyHeld:case K::KeyPressed:case K::KeyReleased:return {"Inputs","Keyboard"};
    case K::Center:case K::CaptureFps:case K::Detections:case K::ControlActive:return {"Inputs","Runtime data"};
    case K::Boolean:case K::Number:case K::Integer:case K::String:return {"Values",nullptr};
    case K::ToFloat:case K::ToInteger:case K::IntegerToString:case K::FloatToString:return {"Math","Conversion"};
    case K::IntegerAdd:case K::IntegerSubtract:case K::IntegerMultiply:case K::IntegerDivide:case K::IntegerClamp:return {"Math","Integers"};
    case K::Greater:case K::Less:return {"Logic","Comparisons"};
    case K::Pulse:case K::Toggle:return {"Logic","Timing and state"};
    case K::Target:case K::TargetNearest:case K::TargetTracker:return {"Targets","Selection"};
    case K::TargetValid:case K::TargetObserved:case K::ScopeCheck:return {"Targets","Conditions"};
    case K::AimPoint:case K::TargetDistance:return {"Targets","Properties"};
    case K::Prediction:case K::CameraCompensation:return {"Motion","Prediction"};
    case K::MouseDelta:case K::NormalizeFps:return {"Motion","Mouse conversion"};
    case K::SpeedCurve:case K::WindMotion:case K::ScaleMovement:case K::LimitMovement:return {"Motion","Response"};
    case K::AutoAimEnabled:return {"Settings","Aim"};
    case K::AutoShootEnabled:return {"Settings","Auto shoot"};
    case K::RecoilEnabled:case K::RecoilStrength:return {"Settings","Recoil"};
    case K::CaptureSource:return {"Vision","Capture"};
    case K::Inference:case K::DepthInference:return {"Vision","Models"};
    case K::DepthToMask:case K::ApplyMask:case K::FilterMask:return {"Vision","Masks"};
    case K::AimOutput:case K::AutoShootOutput:case K::RecoilOutput:return {"Outputs","Controls"};
    case K::VisionOutput:return {"Outputs","Observations"};
    case K::CollectDataset:case K::DatasetOutput:case K::DatasetLabels:case K::ScreenshotOutput:return {"Outputs","Recording"};
    case K::RenderOverlay:case K::OverlayOutput:case K::ImagePreview:return {"Outputs","Rendering"};
    case K::OverlayBoxes:case K::OverlayFrame:case K::OverlayMotion:case K::OverlayIcon:case K::OverlayDepth:case K::OverlayCorrection:return {"Outputs","Overlay layers"};
    case K::RerouteOverlay:case K::RerouteLabels:return {"Routing","Settings"};
    case K::RerouteBoolean:case K::RerouteNumber:case K::RerouteInteger:case K::ReroutePoint:return {"Routing","Signals"};
    case K::RerouteString:case K::RerouteMonitor:case K::RerouteMonitors:return {"Routing","Values"};
    case K::RerouteDetections:case K::RerouteTarget:return {"Routing","Targets"};
    case K::RerouteMovement:return {"Routing","Movement"};
    case K::RerouteFrame:case K::RerouteDepth:case K::RerouteMask:return {"Routing","Images"};
    default:break;
    }
    if(!std::strcmp(def.category,"Logic"))return {"Logic","Boolean"};
    if(!std::strcmp(def.category,"Math"))return {"Math","Numbers"};
    if(!std::strcmp(def.category,"Points"))return {"Math","Points"};
    if(!std::strcmp(def.category,"Targets"))return {"Targets","Properties"};
    if(!std::strcmp(def.category,"Detections"))return {"Targets","Detection filters"};
    return {def.category,nullptr};
}
struct CatalogMenu
{
    std::string label;
    std::vector<CatalogMenu> children;
    std::vector<NodeKind> nodes;
    CatalogMenu& branch(const char* name)
    {
        for(auto& child:children)if(child.label==name)return child;
        children.push_back({name});return children.back();
    }
};
void requestCatalogPreview(NodeKind kind,bool retain=false)
{
    auto& preview=editor.catalogPreview;preview.requested=kind;preview.requestFrame=ImGui::GetFrameCount();
    preview.anchor=ImGui::GetCurrentWindow()->Rect();preview.owner=ImGui::GetCurrentWindow()->ID;
    if(!retain)preview.hoveredAt=ImGui::GetTime();
}
bool retainCatalogPreview(NodeKind kind)
{
    const auto& preview=editor.catalogPreview;const auto& context=*ImGui::GetCurrentContext();
    if(preview.kind!=kind||preview.owner!=ImGui::GetCurrentWindow()->ID||preview.drawnFrame<ImGui::GetFrameCount()-1)return false;
    auto bounds=preview.bounds;bounds.Expand(12);
    const auto* window=ImGui::FindWindowByName("Node preview###catalog_node_preview");
    return bounds.Contains(ImGui::GetIO().MousePos)||(window&&context.ActiveIdWindow==window&&context.ActiveId!=0)||ImGui::GetTime()-preview.hoveredAt<0.25;
}
const std::string& catalogDefaultValue(const InputDefaultDisplay& value)
{
    return value.kind==InputDefaultKind::Number||value.kind==InputDefaultKind::Text?value.fullText:value.text;
}
void drawNodeShell(ImDrawList* draw,ImVec2 p,ImVec2 q,float header,float zoom,ImU32 accent,bool bypassed,bool selected,float hoverFade,bool active,float signal)
{
    draw->AddRectFilled(add(p,ImVec2(3,4)),add(q,ImVec2(3,4)),IM_COL32(0,0,0,50),6*zoom);
    draw->AddRectFilled(p,q,bypassed?IM_COL32(33,34,38,255):IM_COL32(37,39,44,255),6*zoom);
    draw->AddRectFilled(p,ImVec2(q.x,p.y+header),IM_COL32(46,49,55,255),6*zoom);
    draw->AddRect(p,q,selected?Cyan:IM_COL32(74+static_cast<int>(26*hoverFade),79+static_cast<int>(35*hoverFade),88+static_cast<int>(43*hoverFade),255),6*zoom,0,selected?2.0f:1.0f);
    draw->AddRectFilled(add(p,ImVec2(0,8*zoom)),add(p,ImVec2(3*zoom,24*zoom)),(accent&0x00ffffff)|IM_COL32(0,0,0,active?230:85),2);
    if(signal>0.01f)draw->AddRect(p,q,(accent&0x00ffffff)|IM_COL32(0,0,0,static_cast<int>(210*signal)),6*zoom,0,1.0f+signal);
}
void layoutCatalogCard(CatalogPreviewState& preview,float width)
{
    if(preview.layoutWidth==width)return;
    preview.layoutWidth=width;preview.rows.clear();++preview.layouts;
    const auto& def=definition(preview.kind);const float font=ImGui::GetFontSize();
    const auto measure=[&](const char* text,float wrap){return ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,std::max(1.0f,wrap),text);};
    preview.headerHeight=std::max(HeaderHeight,measure(def.title,width-30).y+12);
    float y=preview.headerHeight+8;
    for(std::size_t pin=0;pin<std::max(def.inputs.size(),def.outputs.size());++pin)
    {
        CatalogPreviewState::Row row;row.y=y;float height=font;
        if(pin<def.outputs.size())
        {
            row.outputWidth=std::min(measure(def.outputs[pin].name,FLT_MAX).x,pin<def.inputs.size()?width*0.36f:width-24);
            height=std::max(height,measure(def.outputs[pin].name,row.outputWidth).y);
        }
        if(pin<def.inputs.size())
        {
            const float available=std::max(24.0f,width-24-(pin<def.outputs.size()?row.outputWidth+16:0));
            const float padding=modelPathInput(preview.kind,def.inputs[pin].defaultParameter)?26.0f:8.0f;
            const auto& value=catalogDefaultValue(preview.display.defaults[pin]);
            const auto labelSize=measure(def.inputs[pin].name,FLT_MAX),valueSize=measure(value.c_str(),FLT_MAX);
            row.labelWidth=std::min(labelSize.x,available);row.valueWidth=std::min(valueSize.x+padding,available);
            const float labelHeight=measure(def.inputs[pin].name,row.labelWidth).y;
            row.valueX=12;row.valueY=labelHeight+4;
            if(labelSize.x+6+row.valueWidth<=available){row.valueX+=labelSize.x+6;row.valueY=0;}
            row.valueHeight=measure(value.c_str(),row.valueWidth-padding).y+2;
            height=std::max(height,row.valueY+row.valueHeight);
        }
        row.height=std::max(PinSpacing,height+8);y+=row.height;preview.rows.push_back(row);
    }
    preview.settingsY=y+4;
    if(hasNodeSettings(preview.node))y+=preview.node.kind==NodeKind::Boolean?56:font*2+36;
    if(preview.kind==NodeKind::ImagePreview)y+=std::clamp((width-20)*9/16,64.0f,240.0f)+8;
    preview.cardHeight=reroute(preview.kind)?60:y+16;
}
void drawCatalogCard(CatalogPreviewState& preview)
{
    auto* draw=ImGui::GetWindowDrawList();const auto& def=definition(preview.kind);const float font=ImGui::GetFontSize();
    const auto p=add(ImGui::GetCursorScreenPos(),ImVec2(5,4));const float width=std::max(32.0f,ImGui::GetContentRegionAvail().x-10);
    layoutCatalogCard(preview,width);const auto q=add(p,ImVec2(width,preview.cardHeight));
    const auto accent=def.outputs.empty()?Cyan:pinColor(def.outputs[0].type);
    preview.inputRows=static_cast<int>(def.inputs.size());preview.outputRows=static_cast<int>(def.outputs.size());
    if(reroute(preview.kind))
    {
        const ImVec2 center(p.x+width*0.5f,p.y+30);drawReroutePoint(draw,center,5,accent,false,0);
        draw->AddLine(add(center,ImVec2(-38,0)),add(center,ImVec2(-6,0)),accent,1.4f);
        draw->AddLine(add(center,ImVec2(6,0)),add(center,ImVec2(38,0)),accent,1.4f);
    }
    else
    {
        drawNodeShell(draw,p,q,preview.headerHeight,1,accent,false,false,0,false,0);
        const ImVec2 c(p.x+12,p.y+14);draw->AddTriangleFilled(add(c,ImVec2(-4,-2)),add(c,ImVec2(4,-2)),add(c,ImVec2(0,4)),IM_COL32(175,186,200,255));
        draw->AddText(ImGui::GetFont(),font,add(p,ImVec2(24,6)),IM_COL32(238,241,245,255),def.title,nullptr,width-30);
        for(std::size_t pin=0;pin<preview.rows.size();++pin)
        {
            const auto& row=preview.rows[pin];const float y=p.y+row.y;
            if(y+row.height<ImGui::GetCurrentWindow()->ClipRect.Min.y||y>ImGui::GetCurrentWindow()->ClipRect.Max.y)continue;
            if(pin<def.inputs.size())
            {
                const auto color=pinColor(def.inputs[pin].type);const ImVec2 port(p.x,y+font*0.5f);
                draw->AddCircleFilled(port,5,IM_COL32(27,29,33,255));draw->AddCircle(port,4,color,0,1.3f);
                draw->AddText(ImGui::GetFont(),font,ImVec2(p.x+12,y),IM_COL32(184,192,204,255),def.inputs[pin].name,nullptr,row.labelWidth);
                const ImVec2 value(p.x+row.valueX,y+row.valueY);
                const bool model=modelPathInput(preview.kind,def.inputs[pin].defaultParameter);
                draw->AddRectFilled(add(value,ImVec2(0,-1)),add(value,ImVec2(row.valueWidth,row.valueHeight)),(color&0x00ffffff)|IM_COL32(0,0,0,22),3);
                draw->AddText(ImGui::GetFont(),font,add(value,ImVec2(4,0)),(color&0x00ffffff)|IM_COL32(0,0,0,220),catalogDefaultValue(preview.display.defaults[pin]).c_str(),nullptr,std::max(1.0f,row.valueWidth-(model?26:8)));
                if(model&&row.valueWidth>=26)
                {
                    const ImVec2 c(value.x+row.valueWidth-9,value.y+font*0.5f);
                    draw->AddLine(add(c,ImVec2(-3,-1.5f)),add(c,ImVec2(0,1.5f)),color,1);draw->AddLine(add(c,ImVec2(0,1.5f)),add(c,ImVec2(3,-1.5f)),color,1);
                }
            }
            if(pin<def.outputs.size())
            {
                const auto color=pinColor(def.outputs[pin].type);const ImVec2 port(q.x,y+font*0.5f);
                draw->AddCircleFilled(port,5,IM_COL32(27,29,33,255));draw->AddCircle(port,4,color,0,1.3f);
                draw->AddText(ImGui::GetFont(),font,ImVec2(q.x-12-row.outputWidth,y),IM_COL32(184,192,204,255),def.outputs[pin].name,nullptr,std::max(1.0f,row.outputWidth));
            }
        }
        if(hasNodeSettings(preview.node))
        {
            const ImVec2 a(p.x+6,p.y+preview.settingsY),b(q.x-6,q.y-12);
            draw->AddRectFilled(a,b,IM_COL32(23,25,29,105),4);
            const auto cursor=ImGui::GetCursorScreenPos();ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha,1);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(5,4));ImGui::BeginDisabled();
            draw->AddText(add(a,ImVec2(5,6)),ImGui::GetColorU32(ImGuiCol_Text),variableGet(preview.kind)||variableSet(preview.kind)?"Variable name":"Value");
            if(preview.kind==NodeKind::Boolean)
            {
                const float toggleWidth=std::max(18.0f,ImGui::GetFrameHeight()*0.72f)*1.85f;
                ImGui::SetCursorScreenPos(ImVec2(b.x-toggleWidth-10,a.y+2));ImGui::SetNextItemWidth(toggleWidth);
                bool value=preview.node.value!=0;ImGui::Checkbox("##literal",&value);
            }
            else
            {
                ImGui::SetCursorScreenPos(add(a,ImVec2(5,font+12)));ImGui::SetNextItemWidth(std::max(16.0f,b.x-a.x-20));
                if(preview.kind==NodeKind::Number){double value=preview.node.value;ImGui::InputDouble("##literal",&value,0,0,"%.5f");}
                else if(preview.kind==NodeKind::Integer){int value=static_cast<int>(preview.node.value);ImGui::InputInt("##literal",&value,0,0);}
                else{char value[129]{};const auto name=textParameter(preview.node,"name");std::memcpy(value,name.data(),std::min(name.size(),sizeof(value)-1));ImGui::InputText("##literal",value,sizeof(value));}
            }
            ImGui::EndDisabled();ImGui::PopStyleVar(2);ImGui::SetCursorScreenPos(cursor);
        }
        if(preview.kind==NodeKind::ImagePreview)
        {
            const ImVec2 a(p.x+10,p.y+preview.settingsY),b(q.x-10,q.y-12);
            draw->AddRectFilled(a,b,IM_COL32(17,19,23,255),4);draw->AddRect(a,b,IM_COL32(66,73,83,150),4);
            const char* label="Connect Frame, Depth or Mask";const float wrap=std::max(1.0f,b.x-a.x-16);
            const auto size=ImGui::GetFont()->CalcTextSizeA(font,FLT_MAX,wrap,label);
            draw->AddText(ImGui::GetFont(),font,add(ImVec2((a.x+b.x)*0.5f,(a.y+b.y)*0.5f),multiply(size,-0.5f)),IM_COL32(137,146,159,255),label,nullptr,wrap);
        }
    }
    ImGui::Dummy(ImVec2(0,preview.cardHeight+12));
}
ImRect catalogPreviewPlacement(ImRect menus,ImRect viewport,ImVec2 desired)
{
    constexpr float gap=8,minWidth=180;
    const float right=viewport.Max.x-menus.Max.x-gap,left=menus.Min.x-viewport.Min.x-gap;
    bool useRight=right>=desired.x||(right>=left&&left<desired.x);
    const float side=useRight?right:left;
    if(side>=minWidth)
    {
        const ImVec2 size(std::min(desired.x,side),std::min(desired.y,viewport.GetHeight()));
        const ImVec2 p(useRight?menus.Max.x+gap:menus.Min.x-gap-size.x,
            std::clamp(menus.Min.y,viewport.Min.y,viewport.Max.y-size.y));
        return ImRect(p,add(p,size));
    }
    const float above=menus.Min.y-viewport.Min.y-gap,below=viewport.Max.y-menus.Max.y-gap;
    if(std::max(above,below)<140)return {};
    const ImVec2 size(std::min(desired.x,viewport.GetWidth()),std::min(desired.y,std::max(above,below)));
    const ImVec2 p(std::clamp(menus.Min.x,viewport.Min.x,viewport.Max.x-size.x),below>=above?menus.Max.y+gap:menus.Min.y-gap-size.y);
    return ImRect(p,add(p,size));
}
void drawCatalogPreview()
{
    auto& preview=editor.catalogPreview;
    if(preview.requestFrame!=ImGui::GetFrameCount()||preview.drawnFrame==ImGui::GetFrameCount()||ImGui::GetCurrentContext()->BeginPopupStack.empty()||ImGui::GetCurrentContext()->OpenPopupStack.empty())return;
    const bool changed=preview.kind!=preview.requested;
    if(changed)
    {
        preview.kind=preview.requested;preview.node=defaultNode(preview.kind,1);preview.display=NodeDisplay{};
        if(preview.kind==NodeKind::Boolean)preview.node.value=1;
        for(std::size_t pin=0;pin<definition(preview.kind).inputs.size();++pin)inputDefault(preview.display,preview.node,pin);
        preview.measuredFont=nullptr;preview.contentHeight=0;preview.layoutWidth=-1;++preview.builds;
    }
    const auto& def=definition(preview.kind);const float font=ImGui::GetFontSize();
    if(preview.measuredFont!=ImGui::GetFont()||preview.measuredSize!=font)
    {
        float width=ImGui::CalcTextSize(def.title).x+48;
        for(std::size_t pin=0;pin<def.inputs.size();++pin)
            width=std::max(width,ImGui::CalcTextSize(def.inputs[pin].name).x+ImGui::CalcTextSize(catalogDefaultValue(preview.display.defaults[pin]).c_str()).x+80);
        for(const auto& pin:def.outputs)width=std::max(width,ImGui::CalcTextSize(pin.name).x+48);
        preview.desiredWidth=std::clamp(width,240.0f,560.0f);preview.measuredFont=ImGui::GetFont();preview.measuredSize=font;preview.contentHeight=0;preview.layoutWidth=-1;
    }
    const auto* viewport=ImGui::GetWindowViewport();ImRect menus=preview.anchor;
    for(const auto& popup:ImGui::GetCurrentContext()->OpenPopupStack)if(popup.Window&&popup.Window->Active)menus.Add(popup.Window->Rect());
    const ImRect work(add(viewport->WorkPos,ImVec2(8,8)),add(viewport->WorkPos,ImVec2(viewport->WorkSize.x-8,viewport->WorkSize.y-8)));
    const float desiredHeight=preview.contentHeight>0?preview.contentHeight+20:font*10+(def.inputs.size()+def.outputs.size())*(font+12);
    const auto bounds=catalogPreviewPlacement(menus,work,ImVec2(preview.desiredWidth,std::clamp(desiredHeight,160.0f,600.0f)));
    if(bounds.GetWidth()<1||bounds.GetHeight()<140)return;
    preview.bounds=bounds;preview.drawnFrame=ImGui::GetFrameCount();
    ImGui::SetNextWindowPos(bounds.Min);ImGui::SetNextWindowSize(bounds.GetSize());
    if(changed)ImGui::SetNextWindowScroll(ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(12,10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,7);
    ImGui::PushStyleColor(ImGuiCol_PopupBg,ImVec4(0.11f,0.12f,0.14f,0.98f));
    const auto flags=ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|
        ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_Tooltip;
    if(ImGui::Begin("Node preview###catalog_node_preview",nullptr,flags))
    {
        auto* window=ImGui::GetCurrentWindow();
        // A read-only surface consumes background clicks without focusing the
        // tooltip or dismissing its parent menus. The native scrollbar remains interactive.
        const auto bodyId=window->GetID("##read_only_surface");bool hovered=false,held=false;
        ImGui::ItemAdd(window->InnerRect,bodyId,nullptr,ImGuiItemFlags_NoNav);
        ImGui::ButtonBehavior(window->InnerRect,bodyId,&hovered,&held,ImGuiButtonFlags_NoNavFocus);
        ImGui::TextDisabled("Node preview");
        ImGui::PushTextWrapPos(0);ImGui::TextUnformatted(def.description);ImGui::PopTextWrapPos();
        ImGui::Separator();
        drawCatalogCard(preview);
        preview.contentHeight=ImGui::GetCursorPosY()+ImGui::GetScrollY()-window->WindowPadding.y-ImGui::GetStyle().ItemSpacing.y;
        if(ImGui::GetCurrentContext()->ActiveIdWindow==window&&ImGui::GetActiveID())preview.interacting=true;
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)&&preview.interacting)
        {if(editor.search[0]||editor.catalogSearch.browse)editor.catalogSearch.refocus=true;preview.interacting=false;}
    }
    ImGui::End();ImGui::PopStyleColor();ImGui::PopStyleVar(2);
}
void catalogItem(Graph& graph,int index,NodeKind kind,bool showCategory=false)
{
    const auto& def=definition(kind);const auto path=catalogPath(def);
    if(ImGui::MenuItem(def.title,showCategory?path.group:nullptr))
    {
        std::string error;
        if(!addCatalogNode(graph,index,kind,error)){editor.message=error;editor.error=true;}
        ImGui::CloseCurrentPopup();
    }
    const bool hovered=ImGui::IsItemHovered()||(editor.popupUsesKeyboard&&ImGui::GetCurrentContext()->NavId==ImGui::GetItemID());
    if(hovered)requestCatalogPreview(kind);
}
void syncPopupNavigation()
{
    auto& context=*ImGui::GetCurrentContext();const auto& io=ImGui::GetIO();
    if(editor.popupInputFrame!=ImGui::GetFrameCount())
    {
        editor.popupInputFrame=ImGui::GetFrameCount();
        if(io.MouseDelta.x!=0||io.MouseDelta.y!=0||io.MouseWheel!=0||io.MouseWheelH!=0||
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)||ImGui::IsMouseClicked(ImGuiMouseButton_Right)||ImGui::IsMouseClicked(ImGuiMouseButton_Middle))editor.popupUsesKeyboard=false;
        for(const auto key:{ImGuiKey_Tab,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_UpArrow,ImGuiKey_DownArrow,
            ImGuiKey_Home,ImGuiKey_End,ImGuiKey_PageUp,ImGuiKey_PageDown,ImGuiKey_Enter,ImGuiKey_KeypadEnter,ImGuiKey_Space,
            ImGuiKey_GamepadDpadLeft,ImGuiKey_GamepadDpadRight,ImGuiKey_GamepadDpadUp,ImGuiKey_GamepadDpadDown,ImGuiKey_GamepadFaceDown})
            if(ImGui::IsKeyPressed(key))editor.popupUsesKeyboard=true;
    }
    if(!editor.popupUsesKeyboard)
    {
        // Opening/recycling a submenu initializes its first navigation item.
        // Keep that cursor for keyboard navigation, but don't paint it as a
        // second hover target while the user is moving through mouse menus.
        ImGui::SetNavCursorVisible(false);context.NavHighlightItemUnderNav=false;
    }
}
bool beginEditorMenu(const char* label,bool enabled=true)
{
    if(!ImGui::BeginMenu(label,enabled))return false;
    syncPopupNavigation();return true;
}
bool beginEditorPopup(const char* label)
{
    if(!ImGui::BeginPopup(label))return false;
    syncPopupNavigation();return true;
}
void catalogBranches(Graph& graph,int index,const CatalogMenu& menu)
{
    for(const auto& branch:menu.children)if(beginEditorMenu(branch.label.c_str()))
    {catalogBranches(graph,index,branch);ImGui::EndMenu();}
    for(const auto kind:menu.nodes)catalogItem(graph,index,kind);
    if(!menu.nodes.empty())
    {
        auto& preview=editor.catalogPreview;
        // Resolve every row before retaining or drawing a preview. Otherwise
        // an earlier row's retained card can consume the frame before the hovered row.
        if(preview.requestFrame!=ImGui::GetFrameCount()&&
            std::find(menu.nodes.begin(),menu.nodes.end(),preview.kind)!=menu.nodes.end()&&retainCatalogPreview(preview.kind))
            requestCatalogPreview(preview.kind,true);
        if(preview.owner==ImGui::GetCurrentWindow()->ID)drawCatalogPreview();
    }
}
void refreshCatalogCandidates(const Graph& graph,int index)
{
    auto& state=editor.catalogSearch;
    if(state.candidatesReady&&state.graph==index&&state.revision==editor.observedRevision&&
        state.anchor==editor.catalogWireNode&&state.port==editor.catalogWirePort&&state.output==editor.catalogWireOutput)return;
    state.graph=index;state.revision=editor.observedRevision;state.anchor=editor.catalogWireNode;state.port=editor.catalogWirePort;state.output=editor.catalogWireOutput;
    state.available.clear();
    for(const auto& def:definitions())
    {
        if(def.kind==NodeKind::RenderOverlay||def.kind==NodeKind::CollectDataset||!kindAllowed(graph,def.kind))continue;
        Link connection;
        if(editor.catalogWireNode&&!catalogConnection(graph,defaultNode(def.kind,nextNodeId(graph)),connection))continue;
        state.available.push_back(def.kind);
    }
    state.candidatesReady=true;state.filteredReady=false;++state.candidateBuilds;
}
CatalogMenu catalogTree(const std::vector<NodeKind>& kinds)
{
    CatalogMenu tree;
    for(auto kind:kinds)
    {const auto path=catalogPath(definition(kind));auto& group=tree.branch(path.group);(path.subgroup?group.branch(path.subgroup):group).nodes.push_back(kind);}
    return tree;
}
int catalogSearchInput(ImGuiInputTextCallbackData* data)
{
    auto& state=*static_cast<CatalogSearchState*>(data->UserData);
    if(data->EventFlag==ImGuiInputTextFlags_CallbackCompletion)state.accept=true;
    else if(data->EventFlag==ImGuiInputTextFlags_CallbackHistory)
    {
        const int direction=data->EventKey==ImGuiKey_UpArrow?-1:1;
        // The first Down on an empty field opens the list at its first result.
        state.step+=data->BufTextLen==0&&!state.browse&&direction>0?0:direction;
        state.browse=true;state.navigate=true;state.reveal=true;
    }
    if(!state.pendingText.empty())
    {
        if(data->SelectionStart!=data->SelectionEnd)data->DeleteChars(data->SelectionStart,data->SelectionEnd-data->SelectionStart);
        const char* begin=state.pendingText.c_str();const auto length=std::min<std::size_t>(state.pendingText.size(),data->BufSize-data->BufTextLen-1);
        const char* end=begin+length;
        while(end>begin&&end<begin+state.pendingText.size()&&(static_cast<unsigned char>(*end)&0xc0)==0x80)--end;
        data->InsertChars(data->CursorPos,begin,end);state.pendingText.clear();
    }
    return 0;
}
int catalogSearchRank(NodeKind kind,const std::string& query)
{
    const auto lower=[](std::string text){for(auto& c:text)if(c>='A'&&c<='Z')c+='a'-'A';return text;};
    const auto title=lower(definition(kind).title),shortTitle=lower(compactTitle(kind));
    if(title==query||shortTitle==query)return 0;
    if(title.rfind(query,0)==0||shortTitle.rfind(query,0)==0)return 1;
    if(title.find(query)!=std::string::npos||shortTitle.find(query)!=std::string::npos)return 2;
    return 3;
}
void refreshCatalogSearch(const Graph& graph,int index)
{
    auto& state=editor.catalogSearch;
    refreshCatalogCandidates(graph,index);
    if(state.filteredReady&&state.query==editor.search)return;
    state.query=editor.search;state.nodes.clear();
    for(auto kind:state.available)if(matches(definition(kind)))state.nodes.push_back(kind);
    std::string query=state.query;for(auto& c:query)if(c>='A'&&c<='Z')c+='a'-'A';
    std::stable_sort(state.nodes.begin(),state.nodes.end(),[&](NodeKind a,NodeKind b){return catalogSearchRank(a,query)<catalogSearchRank(b,query);});
    state.selected=0;state.reveal=true;state.filteredReady=true;++state.builds;
}
bool catalogSearchResults()
{
    auto& state=editor.catalogSearch;const int count=static_cast<int>(state.nodes.size());
    if(!count){ImGui::TextDisabled("No matching nodes.");state.step=0;return false;}
    state.selected=((state.selected+state.step)%count+count)%count;state.step=0;
    const float font=ImGui::GetFontSize(),rowHeight=std::ceil(font*1.8f+10);
    const auto* viewport=ImGui::GetWindowViewport();
    const float chrome=ImGui::GetFrameHeightWithSpacing()+ImGui::GetTextLineHeightWithSpacing()*(editor.catalogWireNode?5:9)+2*ImGui::GetStyle().WindowPadding.y;
    const float height=std::min(std::min(count,7)*rowHeight+4,std::max(rowHeight+4,viewport->WorkSize.y-chrome-16));
    const float width=std::min(360.0f,std::max(180.0f,viewport->WorkSize.x-4*ImGui::GetStyle().WindowPadding.x));
    ImGui::BeginChild("catalog_results",ImVec2(width,height),ImGuiChildFlags_None,ImGuiWindowFlags_NoNavInputs);
    bool chosen=false;ImGuiListClipper clipper;clipper.Begin(count,rowHeight);
    if(state.reveal)clipper.IncludeItemByIndex(state.selected);
    while(clipper.Step())for(int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row)
    {
        const auto kind=state.nodes[row];const auto& def=definition(kind);const auto path=catalogPath(def);
        const auto p=ImGui::GetCursorScreenPos();ImGui::PushID(static_cast<int>(kind));
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav,true);
        const bool pressed=ImGui::Selectable("##result",row==state.selected,ImGuiSelectableFlags_NoAutoClosePopups,ImVec2(0,rowHeight-ImGui::GetStyle().ItemSpacing.y));
        ImGui::PopItemFlag();
        const bool over=ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if(over&&!editor.popupUsesKeyboard&&(ImGui::GetIO().MouseDelta.x!=0||ImGui::GetIO().MouseDelta.y!=0)){state.selected=row;state.reveal=false;}
        if(pressed){state.selected=row;chosen=true;}
        if(state.reveal&&row==state.selected)ImGui::SetScrollHereY(0.5f);
        const auto q=ImGui::GetItemRectMax();const ImVec4 clip(p.x+8,p.y,q.x-6,q.y);
        auto* draw=ImGui::GetWindowDrawList();
        if(row==state.selected)draw->AddRectFilled(add(p,ImVec2(1,4)),ImVec2(p.x+3,q.y-4),Cyan,1);
        draw->AddText(ImGui::GetFont(),font,add(p,ImVec2(10,2)),IM_COL32(223,230,238,255),def.title,nullptr,0,&clip);
        std::string breadcrumb=path.group;if(path.subgroup){breadcrumb+=" / ";breadcrumb+=path.subgroup;}
        draw->AddText(ImGui::GetFont(),font*0.8f,add(p,ImVec2(10,font+5)),IM_COL32(142,157,175,255),breadcrumb.c_str(),nullptr,0,&clip);
        ImGui::PopID();
    }
    state.reveal=false;ImGui::EndChild();return chosen;
}
void catalog(Graph& graph, int index)
{
    if(!ImGui::IsPopupOpen("node_catalog")){editor.catalogWireNode=0;return;}
    auto& state=editor.catalogSearch;const auto* viewport=ImGui::GetWindowViewport();
    const ImVec2 minimum=add(viewport->WorkPos,ImVec2(8,8)),available(std::max(1.0f,viewport->WorkSize.x-16),std::max(1.0f,viewport->WorkSize.y-16));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0,0),available);
    if(!editor.catalogFocus&&state.popupSize.x>0)
    {
        const ImVec2 maximum(minimum.x+available.x-std::min(available.x,state.popupSize.x),minimum.y+available.y-std::min(available.y,state.popupSize.y));
        ImGui::SetNextWindowPos(ImVec2(std::clamp(state.popupAnchor.x,minimum.x,maximum.x),std::clamp(state.popupAnchor.y,minimum.y,maximum.y)),ImGuiCond_Always);
    }
    if (!beginEditorPopup("node_catalog"))
    {if(!ImGui::IsPopupOpen("node_catalog"))editor.catalogWireNode=0;return;}
    if(editor.catalogFocus)
    {
        editor.catalogPreview=CatalogPreviewState{};
        state=CatalogSearchState{};const auto anchor=ImGui::GetMousePosOnOpeningCurrentPopup();
        state.popupAnchor=ImGui::IsMousePosValid(&anchor)?anchor:ImGui::GetWindowPos();
    }
    if((editor.search[0]||state.browse)&&ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        ImGui::ClearActiveID();editor.catalogWireNode=0;ImGui::CloseCurrentPopup();ImGui::EndPopup();return;
    }
    ImGui::TextUnformatted(editor.catalogWireNode?"Add and connect":"Graph");
    if(editor.catalogWireNode)ImGui::TextDisabled("Only compatible nodes are shown");
    ImGui::SetNextItemWidth(272);
    const bool focusSearch=editor.catalogFocus||state.refocus;
    bool openingShortcut=false;
    if(focusSearch)
    {
        // Activate in the opening frame. A deferred focus request makes
        // InputText discard characters arriving in its activation frame.
        ImGui::FocusWindow(ImGui::GetCurrentWindow());
        auto& context=*ImGui::GetCurrentContext();const auto id=ImGui::GetID("##search");
        const auto& io=ImGui::GetIO();
        openingShortcut=!state.refocus&&((io.KeyShift&&ImGui::IsKeyPressed(ImGuiKey_A))||(io.KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_Space)));
        if(state.refocus)context.InputTextReactivateId=id;
        else
        {
            // Navigation activation suppresses its incoming character queue.
            // Replay real search text via InputText's callback, while keeping
            // the shortcut that opened the library out of the query.
            if(!openingShortcut&&context.ActiveId!=id&&!io.KeyCtrl)
                for(auto c:io.InputQueueCharacters)if(c>=32&&c!=127)
                {char utf8[5];const int bytes=ImTextCharToUtf8(utf8,c);if(state.pendingText.size()+bytes>=sizeof(editor.search))break;state.pendingText.append(utf8,bytes);}
            context.NavActivateId=id;context.NavActivateFlags=ImGuiActivateFlags_PreferInput;
        }
        editor.catalogFocus=false;state.refocus=false;
    }
    auto flags=ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_CallbackHistory;
    if(!state.pendingText.empty())flags|=ImGuiInputTextFlags_CallbackAlways;
    if(editor.search[0]||state.browse||(!openingShortcut&&!ImGui::GetIO().InputQueueCharacters.empty()))flags|=ImGuiInputTextFlags_CallbackCompletion;
    const bool enter=ImGui::InputTextWithHint("##search", "Search nodes...", editor.search, sizeof(editor.search),flags,catalogSearchInput,&state);
    if(ImGui::IsItemEdited())
    {state.selected=0;state.reveal=true;state.error.clear();if(!state.navigate)state.browse=false;editor.popupUsesKeyboard=true;}
    ImGui::Separator();
    bool choose=false;
    if(editor.search[0]||state.browse)
    {
        refreshCatalogSearch(graph,index);choose=catalogSearchResults();
        choose|=(enter||state.accept)&&!state.nodes.empty();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+std::max(180.0f,std::min(360.0f,available.x-16)));
        ImGui::TextDisabled("Up/Down: select   Tab/Enter: add   Esc: close");ImGui::PopTextWrapPos();
    }
    else
    {
        refreshCatalogCandidates(graph,index);const auto tree=catalogTree(state.available);
        if(tree.children.empty())ImGui::TextDisabled("No matching nodes.");
        else if(beginEditorMenu("Add node")){catalogBranches(graph,index,tree);ImGui::EndMenu();}
    }
    if(choose)
    {
        std::string error;
        if(addCatalogNode(graph,index,state.nodes[state.selected],error))
        {ImGui::ClearActiveID();ImGui::CloseCurrentPopup();ImGui::EndPopup();return;}
        state.error=error;editor.message=error;editor.error=true;state.refocus=true;
    }
    else if(enter)state.refocus=true;
    if((editor.search[0]||state.browse)&&!state.nodes.empty())requestCatalogPreview(state.nodes[state.selected]);
    state.accept=false;state.navigate=false;state.step=0;
    if(!state.error.empty())ImGui::TextColored(ImVec4(1,0.45f,0.45f,1),"%s",state.error.c_str());
    if(!editor.catalogWireNode)
    {
        ImGui::Separator();
        if(ImGui::MenuItem("Tie wires","Z + drag")){editor.tieMode=true;ImGui::CloseCurrentPopup();}
        if(ImGui::MenuItem("Group selected nodes","Ctrl+G",false,!editor.selection[index].empty()))groupSelection(graph,index);
        if(ImGui::MenuItem("Arrange graph","Alt+L",false,!graph.nodes.empty()))arrangeGraph(graph,index);
        if(ImGui::MenuItem("Paste","Ctrl+V"))pasteSelection(graph,index);
    }
    const auto* popup=ImGui::GetCurrentWindow();
    state.popupSize=ImVec2(popup->DC.CursorMaxPos.x-popup->Pos.x+popup->WindowPadding.x+popup->Scroll.x+2*popup->WindowBorderSize,
        popup->DC.CursorMaxPos.y-popup->Pos.y+popup->WindowPadding.y+popup->Scroll.y+2*popup->WindowBorderSize);
    if(editor.search[0]||state.browse)drawCatalogPreview();
    ImGui::EndPopup();
}
void rememberParameterChange()
{
    const ImGuiID item=ImGui::GetItemID();
    if(ImGui::IsItemActivated()||!ImGui::IsItemActive()||editor.parameterUndoItem!=item)remember();
    editor.parameterUndoItem=item;
}
void ownInputPopup(int node,int pin)
{
    editor.inputPopupNode=node;editor.inputPopupPin=pin;
    editor.inputPopup=ImGui::GetCurrentContext()->BeginPopupStack.back().PopupId;
}
bool keyParameterControl(const Node& node,const ParameterSpec& spec,double& value,int pin)
{
    const bool keyNode=node.kind==NodeKind::KeyHeld||node.kind==NodeKind::KeyPressed||node.kind==NodeKind::KeyReleased;
    if(!keyNode||spec.kind!=ParameterKind::Integer||spec.minValue<0||spec.maxValue>255)return false;
    const int code=static_cast<int>(value);
    const char* current="Unassigned";
    for(const auto& name:key_names)if(KeyCodes::getKeyCode(name)==code){current=name.c_str();break;}
    bool changed=false;
    if(ImGui::BeginCombo("##value",current,ImGuiComboFlags_NoArrowButton))
    {
        ownInputPopup(node.id,pin);
        for(const auto& name:key_names)
        {
            const int key=KeyCodes::getKeyCode(name);
            if(key<spec.minValue||key>spec.maxValue)continue;
            if(ImGui::Selectable(name.c_str(),key==code)){value=key;changed=true;}
            if(key==code)ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}
bool textChoiceParameter(NodeKind kind,const char* key)
{
    return !std::strcmp(key,"capture_method")||!std::strcmp(key,"capture_target")||!std::strcmp(key,"backend")||
        !std::strcmp(key,"game_overlay_icon_anchor")||(kind==NodeKind::ScreenshotOutput&&!std::strcmp(key,"format"));
}
bool textChoiceControl(const Node& node,const char* key,std::string& value,int pin)
{
    std::vector<const char*> choices;
    const std::string name=key;
    if(name=="capture_method")choices={"duplication_api","winrt","virtual_camera","udp_capture"};
    else if(name=="capture_target")choices={"monitor","window"};
    else if(name=="backend")choices={"DML","CPU","TRT"};
    else if(node.kind==NodeKind::ScreenshotOutput&&name=="format")choices={"png","jpg"};
    else if(name=="game_overlay_icon_anchor")choices={"center","top","bottom"};
    if(choices.empty())return false;
    bool changed=false;
    if(ImGui::BeginCombo("##value",value.c_str(),ImGuiComboFlags_NoArrowButton))
    {
        ownInputPopup(node.id,pin);
        for(const char* choice:choices)
        {
            if(ImGui::Selectable(choice,value==choice)){value=choice;changed=true;}
            if(value==choice)ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}
bool numberChoiceControl(const Node& node,const ParameterSpec& spec,double& value,int pin)
{
    std::string_view choices=spec.choices;const int selected=static_cast<int>(value-spec.minValue);
    auto current=choices;
    for(int i=0;i<selected;++i){const auto end=current.find('|');if(end==std::string_view::npos)break;current.remove_prefix(end+1);}
    const auto label=std::string(current.substr(0,current.find('|')));bool changed=false;
    if(ImGui::BeginCombo("##value",label.c_str(),ImGuiComboFlags_NoArrowButton))
    {
        ownInputPopup(node.id,pin);
        for(int i=0;!choices.empty();++i)
        {
            const auto end=choices.find('|');const auto text=std::string(choices.substr(0,end));
            if(ImGui::Selectable(text.c_str(),i==selected)){value=spec.minValue+i;changed=true;}
            if(i==selected)ImGui::SetItemDefaultFocus();
            if(end==std::string_view::npos)break;choices.remove_prefix(end+1);
        }
        ImGui::EndCombo();
    }
    return changed;
}
void ownInlineItem(int id)
{
    if(ImGui::IsItemActive()){editor.inlineActiveNode=id;editor.inlineActiveItem=ImGui::GetItemID();}
}
struct ModelChoice{std::string name,label,path;bool legacy=false;};
struct ModelMenu
{
    std::vector<ModelChoice> choices;
    std::string value,error;
    int selected=-1;
    float rowHeight=0;
    std::uint64_t scans=0;
} modelMenu;
std::string lowerModelKey(std::string value)
{
    for(auto& c:value)if(c>='A'&&c<='Z')c+='a'-'A';return value;
}
void refreshModelChoices(bool depth,bool engines)
{
    namespace fs=std::filesystem;
    modelMenu.choices.clear();modelMenu.error.clear();modelMenu.value.clear();modelMenu.selected=-1;++modelMenu.scans;
    for(const char* folder:depth?std::vector<const char*>{"models","depth_models"}:std::vector<const char*>{"models"})
    {
        std::error_code error;
        if(!fs::is_directory(folder,error))continue;
        try
        {
            for(fs::directory_iterator it(folder,error),end;!error&&it!=end;it.increment(error))
            {
                std::error_code status;if(!it->is_regular_file(status))continue;
                const auto extension=lowerModelKey(it->path().extension().u8string());
                if(extension!=".onnx"&&!(engines&&extension==".engine")&&!(depth&&(extension==".trt"||extension==".plan")))continue;
                const auto name=it->path().filename().u8string(),path=(fs::path(folder)/it->path().filename()).generic_u8string();
                if(path.size()>1024)continue;
                const bool legacy=std::strcmp(folder,"depth_models")==0;
                modelMenu.choices.push_back({name,legacy?path:name,path,legacy});
            }
        }
        catch(const std::exception&){modelMenu.error="Cannot read the model folder.";}
        if(error)modelMenu.error="Cannot read the model folder.";
    }
    std::sort(modelMenu.choices.begin(),modelMenu.choices.end(),[](const ModelChoice& a,const ModelChoice& b){return a.legacy!=b.legacy?!a.legacy:a.name<b.name;});
}
std::string modelChoiceKey(const Node& node,const std::string& value)
{
    namespace fs=std::filesystem;
    try
    {
        auto path=fs::u8path(value);
        if(!path.has_parent_path())
        {
            const bool legacy=node.kind==NodeKind::DepthInference&&std::any_of(modelMenu.choices.begin(),modelMenu.choices.end(),[&](const ModelChoice& item){return item.legacy&&item.name==value;});
            path=fs::path(legacy?"depth_models":"models")/path;
        }
        if(path.is_absolute())
        {
            std::error_code error;const auto current=fs::current_path(error);
            if(!error){const auto relative=path.lexically_normal().lexically_relative(current);if(!relative.empty())path=relative;}
        }
        return lowerModelKey(path.lexically_normal().generic_u8string());
    }
    catch(const std::exception&){return value;}
}
bool modelChoiceControl(const Node& node,std::string& value,int graph,int pin,InputDefaultDisplay& state,float width,float scale,bool engines)
{
    const bool depth=node.kind==NodeKind::DepthInference;
    std::string preview="Select model...";
    if(!value.empty())try{preview=std::filesystem::u8path(value).filename().u8string();if(preview.empty())preview=value;}catch(const std::exception&){preview=value;}
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(width,280*scale),0),ImVec2(std::max(width,600*scale),std::numeric_limits<float>::max()));
    if(!ImGui::BeginCombo("##value",preview.c_str()))return false;
    auto owned=ImGui::GetCurrentContext()->LastItemData;ownInputPopup(node.id,pin);
    const bool appearing=ImGui::IsWindowAppearing();
    if(appearing)refreshModelChoices(depth,engines);
    if(appearing||modelMenu.value!=value)
    {
        modelMenu.value=value;modelMenu.selected=-1;const auto key=modelChoiceKey(node,value);
        for(std::size_t i=0;i<modelMenu.choices.size();++i)if(key==lowerModelKey(modelMenu.choices[i].path)){modelMenu.selected=static_cast<int>(i);break;}
    }
    bool changed=false;
    ImGui::TextDisabled("%s",depth?"models / depth_models":"models");
    const float rows=static_cast<float>(std::clamp<std::size_t>(modelMenu.choices.size(),1,10));
    modelMenu.rowHeight=ImGui::GetTextLineHeightWithSpacing();
    if(appearing)ImGui::SetNextWindowScroll(ImVec2(0,0));
    if(ImGui::BeginChild("##model_list",ImVec2(0,rows*modelMenu.rowHeight+4*scale),ImGuiChildFlags_None))
    {
        if(modelMenu.choices.empty())ImGui::TextDisabled("%s",modelMenu.error.empty()?"No compatible models found.":modelMenu.error.c_str());
        ImGuiListClipper clipper;clipper.Begin(static_cast<int>(modelMenu.choices.size()),modelMenu.rowHeight);
        if(appearing&&modelMenu.selected>=0)clipper.IncludeItemByIndex(modelMenu.selected);
        while(clipper.Step())for(int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i)
        {
            const auto& item=modelMenu.choices[i];ImGui::PushID(i);
            if(ImGui::Selectable("##model",modelMenu.selected==i))
            {value=item.path;changed=true;owned=ImGui::GetCurrentContext()->LastItemData;ImGui::CloseCurrentPopup();}
            const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
            const ImVec4 clip(a.x,a.y,b.x,b.y);ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),ImGui::GetFontSize(),a,ImGui::GetColorU32(ImGuiCol_Text),item.label.c_str(),nullptr,0,&clip);
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",item.path.c_str());
            if(modelMenu.selected==i)ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();ImGui::Separator();
    if(ImGui::Selectable("Choose file..."))
    {editor.fileAction=3;editor.fileGraph=graph;editor.fileNode=node.id;editor.fileParameter="model_path";owned=ImGui::GetCurrentContext()->LastItemData;}
    const std::size_t length=state.textParameter?std::min<std::size_t>(1024,state.textParameter->maxLength):1024;
    state.editBuffer.resize(length+1,0);
    if(value!=state.editBuffer.data())
    {std::fill(state.editBuffer.begin(),state.editBuffer.end(),0);std::memcpy(state.editBuffer.data(),value.data(),std::min(value.size(),length));}
    ImGui::SetNextItemWidth(-1);
    if(ImGui::InputTextWithHint("##custom_path","Custom path...",state.editBuffer.data(),state.editBuffer.size()))
    {value=state.editBuffer.data();changed=true;}
    if(ImGui::IsItemActive()||ImGui::IsItemEdited()){owned=ImGui::GetCurrentContext()->LastItemData;ownInlineItem(node.id);}
    ImGui::EndCombo();ImGui::GetCurrentContext()->LastItemData=owned;return changed;
}
bool inlineCheckbox(const char* label,bool& value,int id,float scale)
{
    // The menu's labeled checkbox reserves a wide 58 px row. Use its compact
    // switch and a wrapped label so both stay inside the node at every zoom.
    const auto position=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
    const float toggleWidth=std::max(18.0f,ImGui::GetFrameHeight()*0.72f)*1.85f;
    const float labelWidth=std::max(10.0f,width-toggleWidth-6*scale);
    ImGui::BeginGroup();ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+labelWidth);
    ImGui::TextWrapped("%s",label);const bool labelPressed=ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImGui::PopTextWrapPos();ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(position.x+width-toggleWidth,position.y));ImGui::SetNextItemWidth(toggleWidth);
    bool changed=ImGui::Checkbox("##value",&value);ownInlineItem(id);
    if(labelPressed){value=!value;changed=true;}
    ImGui::EndGroup();return changed;
}
int drivenInput(const Graph& graph,const Node& node,const char* key)
{
    const auto& inputs=definition(node.kind).inputs;
    for(std::size_t pin=0;pin<inputs.size();++pin)
        if((inputs[pin].defaultParameter&&std::strcmp(inputs[pin].defaultParameter,key)==0)||
            (node.kind==NodeKind::CaptureSource&&pin==0&&(!std::strcmp(key,"monitor_idx")||!std::strcmp(key,"monitor_device")||!std::strcmp(key,"capture_target"))))
            if(std::any_of(graph.links.begin(),graph.links.end(),[&](const Link& link){return link.toNode==node.id&&link.toPort==static_cast<int>(pin);}))return static_cast<int>(pin);
    return -1;
}
void variableNameControl(Graph& graph,int id,NodeDisplay& state)
{
    auto* node=findNode(graph,id);if(!node||(!variableGet(node->kind)&&!variableSet(node->kind)))return;
    auto& buffer=state.settingsBuffers["name"];buffer.resize(129,0);
    const auto value=textParameter(*node,"name");
    if(value!=buffer.data())
    {std::fill(buffer.begin(),buffer.end(),0);std::memcpy(buffer.data(),value.data(),std::min(value.size(),buffer.size()-1));}
    ImGui::TextUnformatted("Variable name");ImGui::SetNextItemWidth(-1);
    if(ImGui::InputText("##name",buffer.data(),buffer.size()))
    {rememberParameterChange();node->textParameters["name"]=buffer.data();edited();}
    ownInlineItem(id);
    if(ImGui::IsItemHovered())ImGui::SetTooltip("Static identifier shared by a matching Get and Set pair.");
}
ImRect settingsRect(const Graph& graph,ImVec2 origin,const Node& node)
{
    const auto p=screenPoint(graph,origin,node.x+6,node.y+nodeSettingsTop(node)+4);
    return {p,add(p,ImVec2((cardWidth(node)-12)*graph.zoom,nodeSettingsHeight(node)*graph.zoom))};
}
ImRect previewRect(const Graph& graph,ImVec2 origin,const Node& node)
{
    const auto p=screenPoint(graph,origin,node.x,node.y);
    return {add(p,ImVec2(10*graph.zoom,(Layout::settingsTop(node)+4)*graph.zoom)),
        add(p,ImVec2((cardWidth(node)-10)*graph.zoom,(renderHeight(graph,node)-30)*graph.zoom))};
}
void inlineSettings(Graph& graph,int index,int id,NodeDisplay& state,ImVec2 origin,bool interactive)
{
    auto* node=findNode(graph,id);if(!node||compactNode(graph,*node)||!hasNodeSettings(*node))return;
    const auto panel=settingsRect(graph,origin,*node);const float scale=graph.zoom;
    if(!panel.Overlaps(ImGui::GetCurrentWindow()->ClipRect)&&editor.inlineActiveNode!=id)return;
    auto* window=ImGui::GetCurrentWindow();auto& dc=window->DC;
    const auto cursor=dc.CursorPos,previous=dc.CursorPosPrevLine,maximumPos=dc.CursorMaxPos,ideal=dc.IdealMaxPos;
    const auto currentLine=dc.CurrLineSize,previousLine=dc.PrevLineSize;
    const float currentBase=dc.CurrLineTextBaseOffset,previousBase=dc.PrevLineTextBaseOffset;
    const bool sameLine=dc.IsSameLine,setPos=dc.IsSetPos;
    const auto work=window->WorkRect,content=window->ContentRegionRect;
    auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(panel.Min,panel.Max,IM_COL32(23,25,29,105),4*scale);
    ImGui::PushID(id);ImGui::PushID("inline_settings");
    // Dormant fields must not take Tab from canvas port navigation. Once a
    // field owns focus, ordinary Tab navigation between settings is available.
    ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop,!editor.inlineActiveItem||editor.portFocus[index].node!=0);
    ImGui::PushFont(nullptr,ImGui::GetStyle().FontSizeBase*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(5*scale,4*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(4*scale,4*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,3*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,10*scale);
    ImGui::PushClipRect(panel.Min,panel.Max,true);
    window->WorkRect=window->ContentRegionRect=ImRect(add(panel.Min,ImVec2(5*scale,6*scale)),add(panel.Max,ImVec2(-16*scale,-6*scale)));
    const float top=panel.Min.y+6*scale;
    ImGui::SetCursorScreenPos(ImVec2(window->WorkRect.Min.x,top));ImGui::BeginGroup();
    ImGui::BeginDisabled(!interactive);
    if(node->kind==NodeKind::Boolean)
    {
        bool value=node->value!=0;if(inlineCheckbox("Value",value,id,scale)){remember();node->value=value?1:0;edited();}
        ownInlineItem(id);
    }
    else if(node->kind==NodeKind::Number)
    {
        double value=node->value;ImGui::TextUnformatted("Value");ImGui::SetNextItemWidth(-1);
        if(ImGui::InputDouble("##constant",&value,0,0,"%.5f")&&std::isfinite(value)){rememberParameterChange();node->value=value;edited();}
        ownInlineItem(id);
    }
    else if(node->kind==NodeKind::Integer)
    {
        int value=static_cast<int>(node->value);ImGui::TextUnformatted("Value");ImGui::SetNextItemWidth(-1);
        if(ImGui::InputInt("##constant",&value,0,0)){rememberParameterChange();node->value=value;edited();}
        ownInlineItem(id);
    }
    variableNameControl(graph,id,state);
    state.settingsContent=std::max(0.0f,(ImGui::GetCursorScreenPos().y-top)/scale+12);
    ImGui::EndDisabled();ImGui::EndGroup();
    ImGui::PopClipRect();ImGui::PopStyleVar(4);ImGui::PopFont();ImGui::PopItemFlag();ImGui::PopID();ImGui::PopID();
    window->WorkRect=work;window->ContentRegionRect=content;
    dc.CursorPos=cursor;dc.CursorPosPrevLine=previous;dc.CursorMaxPos=maximumPos;dc.IdealMaxPos=ideal;
    dc.CurrLineSize=currentLine;dc.PrevLineSize=previousLine;dc.CurrLineTextBaseOffset=currentBase;dc.PrevLineTextBaseOffset=previousBase;
    dc.IsSameLine=sameLine;dc.IsSetPos=setPos;
}
bool scalarInput(const Pin& pin)
{
    return pin.type==ValueType::Boolean||pin.type==ValueType::Float||pin.type==ValueType::Integer||pin.type==ValueType::String;
}
bool editableInput(const Node& node,std::size_t pin,std::uint64_t connectedInputs)
{
    const auto& input=definition(node.kind).inputs[pin];
    if(!scalarInput(input)||(connectedInputs&(std::uint64_t{1}<<pin)))return false;
    if(node.kind==NodeKind::CaptureSource&&(connectedInputs&1)&&input.defaultParameter)
        for(const char* key:{"monitor_idx","monitor_device","capture_target"})if(std::strcmp(input.defaultParameter,key)==0)return false;
    return true;
}
float inputLabelRight(const Graph& graph,const Node& node,std::size_t pin,ImVec2 origin,float font)
{
    const auto p=screenPoint(graph,origin,node.x,node.y);const float right=p.x+(cardWidth(node)-12)*graph.zoom;
    const auto& outputs=definition(node.kind).outputs;
    if(pin>=outputs.size())return right;
    const float outputWidth=ImGui::GetFont()->CalcTextSizeA(font,std::numeric_limits<float>::max(),0,outputs[pin].name).x;
    return std::max(p.x+cardWidth(node)*graph.zoom*0.55f,right-outputWidth)-8*graph.zoom;
}
ImRect inputValueRect(const Graph& graph,const Node& node,std::size_t pin,ImVec2 origin,float font,float desiredWidth,float labelWidth=-1)
{
    const auto& def=definition(node.kind);const auto port=portPoint(graph,origin,node,false,static_cast<int>(pin));
    const float left=port.x+12*graph.zoom;
    const float right=inputLabelRight(graph,node,pin,origin,font);
    if(labelWidth<0)labelWidth=ImGui::GetFont()->CalcTextSizeA(font,std::numeric_limits<float>::max(),0,def.inputs[pin].name).x;
    const float gap=6*graph.zoom;
    const float space=std::max(0.0f,right-left-gap);
    const float label=std::min(labelWidth,std::max(0.0f,space-std::min(desiredWidth,44*graph.zoom)));
    const float width=std::min(desiredWidth,std::max(0.0f,space-label));
    const float x=left+std::max(0.0f,std::min(labelWidth,right-left-width-gap))+gap;
    return {ImVec2(x,port.y-9*graph.zoom),ImVec2(x+width,port.y+9*graph.zoom)};
}
float inputEditorWidth(const Pin& pin,float zoom)
{
    return (pin.type==ValueType::String?112.0f:pin.type==ValueType::Boolean?44.0f:56.0f)*zoom;
}
float inputValueWidth(NodeDisplay& display,const Node& node,std::size_t pin,std::uint64_t connected,ImFont* font,float size,float zoom)
{
    inputDefault(display,node,pin,connected);auto& cached=display.defaults[pin];
    if(cached.naturalWidth<0||cached.measureFont!=font||cached.measureSize!=size)
    {
        cached.measureFont=font;cached.measureSize=size;
        cached.naturalWidth=font->CalcTextSizeA(size,std::numeric_limits<float>::max(),0,cached.text.c_str()).x;
    }
    const float minimum=editableInput(node,pin,connected)?inputEditorWidth(definition(node.kind).inputs[pin],zoom):0;
    const float arrow=modelPathInput(node.kind,definition(node.kind).inputs[pin].defaultParameter)?18*zoom:0;
    return std::max(minimum,cached.naturalWidth+6*zoom+arrow);
}
bool inlineInputDefault(Graph& graph,int index,int id,std::size_t pin,NodeDisplay& display,const ImRect& bounds,float scale)
{
    auto* node=findNode(graph,id);if(!node||bounds.GetWidth()<12*scale)return false;
    const auto& input=definition(node->kind).inputs[pin];
    inputDefault(display,*node,pin);auto& state=display.defaults[pin];
    const auto* parameterSpec=state.parameter;const auto* textSpec=state.textParameter;
    const bool required=state.kind==InputDefaultKind::Required;
    double number=state.number;bool changed=false;std::string text=state.source;
    auto* window=ImGui::GetCurrentWindow();auto& dc=window->DC;
    const auto cursor=dc.CursorPos,previous=dc.CursorPosPrevLine,maximum=dc.CursorMaxPos,ideal=dc.IdealMaxPos;
    const auto currentLine=dc.CurrLineSize,previousLine=dc.PrevLineSize;
    const float currentBase=dc.CurrLineTextBaseOffset,previousBase=dc.PrevLineTextBaseOffset;
    const bool sameLine=dc.IsSameLine,setPos=dc.IsSetPos;
    ImGui::PushID(id);ImGui::PushID("input_defaults");ImGui::PushID(static_cast<int>(pin));
    ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop,!editor.inlineActiveItem||editor.portFocus[index].node!=0);
    ImGui::PushFont(nullptr,ImGui::GetStyle().FontSizeBase*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(3*scale,std::max(0.0f,(bounds.GetHeight()-ImGui::GetFontSize())*0.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,3*scale);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,IM_COL32(29,33,39,245));
    ImGui::PushStyleColor(ImGuiCol_Text,pinColor(input.type));
    ImGui::SetCursorScreenPos(bounds.Min);ImGui::SetNextItemWidth(bounds.GetWidth());
    if(input.type==ValueType::Boolean)
    {
        if(ImGui::Button(number!=0?"true":"false",bounds.GetSize())){number=number==0?1:0;changed=true;}
    }
    else if(input.type==ValueType::String)
    {
        if(textSpec&&modelPathInput(node->kind,textSpec->key))
            changed=modelChoiceControl(*node,text,index,static_cast<int>(pin),state,bounds.GetWidth(),scale,
                node->kind==NodeKind::DepthInference||textParameter(*node,"backend")=="TRT"||drivenInput(graph,*node,"backend")>=0);
        else if(textSpec&&textChoiceParameter(node->kind,textSpec->key))changed=textChoiceControl(*node,textSpec->key,text,static_cast<int>(pin));
        else
        {
            const auto length=textSpec?std::min<std::size_t>(4096,textSpec->maxLength):4096;
            state.editBuffer.resize(length+1,0);
            if(text!=state.editBuffer.data())
            {std::fill(state.editBuffer.begin(),state.editBuffer.end(),0);std::memcpy(state.editBuffer.data(),text.data(),std::min(text.size(),length));}
            const bool file=textSpec&&(!std::strcmp(textSpec->key,"model_path")||!std::strcmp(textSpec->key,"game_overlay_icon_path"));
            if(file)ImGui::SetNextItemWidth(std::max(12*scale,bounds.GetWidth()-21*scale));
            if(ImGui::InputTextWithHint("##value","\"\"",state.editBuffer.data(),state.editBuffer.size())){text=state.editBuffer.data();changed=true;}
            if(file)
            {
                const auto field=ImGui::GetCurrentContext()->LastItemData;
                ImGui::SetCursorScreenPos(ImVec2(bounds.Max.x-18*scale,bounds.Min.y));
                if(ImGui::Button("...",ImVec2(18*scale,bounds.GetHeight())))
                {editor.fileAction=3;editor.fileGraph=index;editor.fileNode=id;editor.fileParameter=textSpec->key;}
                if(ImGui::IsItemHovered())ImGui::SetTooltip("Choose a file");
                ImGui::GetCurrentContext()->LastItemData=field;
            }
        }
    }
    else if(parameterSpec&&parameterSpec->choices)changed=numberChoiceControl(*node,*parameterSpec,number,static_cast<int>(pin));
    else if(parameterSpec&&(node->kind==NodeKind::KeyHeld||node->kind==NodeKind::KeyPressed||node->kind==NodeKind::KeyReleased))
        changed=keyParameterControl(*node,*parameterSpec,number,static_cast<int>(pin));
    else if(input.type==ValueType::Integer||(parameterSpec&&parameterSpec->kind==ParameterKind::Integer))
    {
        int value=static_cast<int>(number);
        const int minimum=parameterSpec?static_cast<int>(parameterSpec->minValue):MinInteger;
        const int maximum=parameterSpec?static_cast<int>(parameterSpec->maxValue):MaxInteger;
        changed=ImGui::DragInt("##value",&value,1,minimum,maximum,"%d",ImGuiSliderFlags_AlwaysClamp);number=value;
    }
    else
    {
        const double minimum=parameterSpec?parameterSpec->minValue:-std::numeric_limits<double>::max();
        const double maximum=parameterSpec?parameterSpec->maxValue:std::numeric_limits<double>::max();
        const float speed=static_cast<float>(std::clamp(std::abs(number)*0.01,parameterSpec?0.001:0.1,1000000.0));
        changed=ImGui::DragScalar("##value",ImGuiDataType_Double,&number,speed,&minimum,&maximum,"%.6g",ImGuiSliderFlags_AlwaysClamp);
    }
    ownInlineItem(id);
    if(ImGui::IsItemActive()){editor.inputEditNode=id;editor.inputEditPin=static_cast<int>(pin);}
    if(required&&input.type!=ValueType::Boolean&&ImGui::IsItemActivated())changed=true;
    if(changed&&std::isfinite(number))
    {
        rememberParameterChange();Node updated=*node;
        if(input.type==ValueType::String)
        {if(textSpec)updated.textParameters[textSpec->key]=text;else updated.inputTexts[static_cast<int>(pin)]=text;}
        else if(parameterSpec)updated.parameters[parameterSpec->key]=std::clamp(number,parameterSpec->minValue,parameterSpec->maxValue);
        else updated.inputValues[static_cast<int>(pin)]=number;
        *node=std::move(updated);edited();
    }
    ImGui::PopStyleColor(2);ImGui::PopStyleVar(2);ImGui::PopFont();ImGui::PopItemFlag();ImGui::PopID();ImGui::PopID();ImGui::PopID();
    dc.CursorPos=cursor;dc.CursorPosPrevLine=previous;dc.CursorMaxPos=maximum;dc.IdealMaxPos=ideal;
    dc.CurrLineSize=currentLine;dc.PrevLineSize=previousLine;dc.CurrLineTextBaseOffset=currentBase;dc.PrevLineTextBaseOffset=previousBase;
    dc.IsSameLine=sameLine;dc.IsSetPos=setPos;
    return true;
}
void inspector(Graph& graph,int index)
{
    if(!beginEditorPopup("node_inspector"))return;
    const auto* node=findNode(graph,editor.selected[index]);
    if(!node){ImGui::EndPopup();return;}
    const Node snapshot=*node;const int id=node->id;
    ImGui::TextDisabled("%s",definition(snapshot.kind).title);
    if(ImGui::MenuItem("Node information","Enter"))openProperties(index);
    if(ImGui::MenuItem("Copy","Ctrl+C"))copySelection(graph,index);
    if(ImGui::MenuItem("Duplicate","Ctrl+D"))duplicateSelection(graph,index);
    if(ImGui::MenuItem("Bypass","Ctrl+B",snapshot.bypassed,canBypass(snapshot.kind)))bypassSelection(graph,index);
    if(ImGui::MenuItem("Disable","Ctrl+M",snapshot.disabled))disableSelection(graph,index);
    if(ImGui::MenuItem("Collapse / expand","Space",snapshot.collapsed,!reroute(snapshot.kind)))collapseSelection(graph,index);
    if(beginEditorMenu("Size",!reroute(snapshot.kind)))
    {
        if(ImGui::MenuItem("Automatic / reset"))resizeSelection(graph,index,NodeSizePreset::Automatic);
        if(ImGui::MenuItem("Fit height to visible settings"))resizeSelection(graph,index,NodeSizePreset::FitContent);
        if(ImGui::MenuItem("Compact width"))resizeSelection(graph,index,NodeSizePreset::Compact);
        if(ImGui::MenuItem("Wide"))resizeSelection(graph,index,NodeSizePreset::Wide);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if(ImGui::MenuItem("Disconnect"))
    {
        remember();graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[id](const Link& link){return link.fromNode==id||link.toNode==id;}),graph.links.end());edited();
    }
    if(ImGui::MenuItem("Delete","Delete"))deleteSelection(graph,index);
    ImGui::EndPopup();
}
int drawWireBendHandle(ImDrawList* draw,const Graph& graph,int index,const std::vector<RoutedWire>& routes,
    ImVec2 origin,ImVec2 end,bool hovered,bool leftGesture,int hoverLink)
{
    const auto& io=ImGui::GetIO();int candidate=-1;float nearest=28*28;
    const bool dragging=editor.bendGraph==index;
    const bool available=hovered&&(!leftGesture||dragging)&&!editor.rightGesture&&editor.panButton<0&&
        !editor.tieMode&&!editor.boxSelectMode&&!(ImGui::IsKeyDown(ImGuiKey_Z)&&!io.KeyCtrl&&!io.WantTextInput);
    if(dragging)
    {
        for(std::size_t i=0;i<graph.links.size();++i)if(linkKey(graph.links[i])==editor.bendLink){candidate=static_cast<int>(i);break;}
    }
    else if(available)
    {
        for(std::size_t i=0;i<routes.size();++i)
        {
            if(routes[i].path.empty()||!contains(routes[i].bendPoint,origin,end))continue;
            const float distance=distanceSquared(io.MousePos,routes[i].bendPoint);
            if(distance<nearest){nearest=distance;candidate=static_cast<int>(i);}
        }
        if(candidate<0)candidate=hoverLink;
    }
    const bool showing=candidate>=0;
    if(showing&&(editor.bendHandleGraph!=index||editor.bendHandleLink!=linkKey(graph.links[candidate])))
    {editor.bendHandleGraph=index;editor.bendHandleLink=linkKey(graph.links[candidate]);editor.bendHandleFade=0;}
    editor.bendHandleFade=std::clamp(editor.bendHandleFade+(showing?1:-1)*io.DeltaTime/0.14f,0.0f,1.0f);
    if(candidate<0&&editor.bendHandleFade>0&&editor.bendHandleGraph==index)
        for(std::size_t i=0;i<graph.links.size();++i)if(linkKey(graph.links[i])==editor.bendHandleLink){candidate=static_cast<int>(i);break;}
    if(candidate<0||routes[candidate].path.empty()||!contains(routes[candidate].bendPoint,origin,end))return -1;
    const auto point=routes[candidate].bendPoint;
    const bool hit=available&&showing&&distanceSquared(io.MousePos,point)<=12*12;
    const auto& route=routes[candidate];const auto& link=graph.links[candidate];
    const auto type=definition(graph.nodes[route.from].kind).outputs[link.fromPort].type;
    const float fade=editor.bendHandleFade;const float radius=dragging?6.5f:5.5f;
    draw->AddCircleFilled(point,radius+2,IM_COL32(12,15,19,static_cast<int>(160*fade)),12);
    draw->AddCircleFilled(point,radius,IM_COL32(30,35,41,static_cast<int>(255*fade)),12);
    draw->AddCircle(point,radius,(pinColor(type)&0x00ffffff)|(static_cast<ImU32>(255*fade)<<24),12,hit||dragging?1.8f:1.2f);
    draw->AddCircleFilled(point,2,IM_COL32(220,226,234,static_cast<int>(255*fade)),8);
    return hit?candidate:-1;
}
void wireInspector(Graph& graph,int index,ImVec2 origin)
{
    if(!beginEditorPopup("wire_inspector"))return;
    const auto found=std::find_if(graph.links.begin(),graph.links.end(),[&](const Link& link){return linkKey(link)==editor.contextLink;});
    if(found==graph.links.end()){ImGui::CloseCurrentPopup();ImGui::EndPopup();return;}
    const int position=static_cast<int>(found-graph.links.begin());
    const auto& routes=editor.canvasRoutes[index].routes;
    if(static_cast<std::size_t>(position)<routes.size())
    {
        const auto& route=routes[position];const auto pointer=screenPoint(graph,origin,editor.catalogPosition.x,editor.catalogPosition.y);
        const float distance=nearestPathDistance(route.path,pointer);
        ImGui::TextDisabled("Connection");
        if(ImGui::MenuItem("Cut connection","Double-click"))
        {
            queueCutAnimation(graph,index,*found,route,wirePosition(route.path,distance));
            remember();graph.links.erase(graph.links.begin()+position);edited();ImGui::CloseCurrentPopup();
        }
        else if(ImGui::MenuItem("Insert reroute"))
        {
            const auto split=static_cast<std::size_t>(std::upper_bound(route.tieDistances.begin(),route.tieDistances.end(),distance)-route.tieDistances.begin());
            insertReroute(graph,position,editor.catalogPosition,index,split);ImGui::CloseCurrentPopup();
        }
        else if(ImGui::MenuItem("Reset bend","Alt+click center",false,!sameBend(found->bend,WireBend{})))
        {resetWireBend(*found);ImGui::CloseCurrentPopup();}
    }
    ImGui::EndPopup();
}
void propertiesPanel(Graph& graph,int index,ImVec2 position,ImVec2 size)
{
    ImGui::SetNextWindowPos(position,ImGuiCond_Always);ImGui::SetNextWindowSize(size,ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,7);
    ImGui::Begin("Node information###node_properties",nullptr,ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushID(index);
    ImGui::TextDisabled("NODE INFORMATION");ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth()-ImGui::GetStyle().WindowPadding.x-24);
    if(ImGui::SmallButton("x##close"))editor.inspectorOpen=false;
    const int id=editor.selected[index];Node* node=findNode(graph,id);
    if(node)
    {
        if(editor.inspectedGraph!=index||editor.inspectedNode!=id)
        {editor.inspectedGraph=index;editor.inspectedNode=id;}
        const auto& def=definition(node->kind);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Cyan),"%s",def.title);ImGui::SameLine();ImGui::TextDisabled("#%d",id);
        const bool fresh=telemetryFresh(index,std::chrono::steady_clock::now());
        const auto* value=nodeValue(index,id,fresh);
        const auto liveText=valueText(value);
        if(!liveText.empty())ImGui::TextColored(ImVec4(0.58f,0.78f,0.9f,1),"%s",liveText.c_str());
        if(value&&!value->error.empty())ImGui::TextWrapped("%s",value->error.c_str());
        ImGui::PushTextWrapPos(0);ImGui::TextDisabled("%s",def.description);ImGui::PopTextWrapPos();
        ImGui::Separator();
        ImGui::TextDisabled("%s",node->disabled?"Disabled":node->bypassed?"Bypassed":"Enabled");
        ImGui::Text("Inputs: %zu  |  Outputs: %zu",def.inputs.size(),def.outputs.size());
        ImGui::TextWrapped("Edit settings directly inside the expanded node.");
    }
    else ImGui::TextWrapped("Select a node to inspect its live data and description.");
    ImGui::PopID();ImGui::End();ImGui::PopStyleVar();
}
void groupInspector(Graph& graph,int tab)
{
    if (!beginEditorPopup("group_inspector")) return;
    Group* group=findGroup(graph,editor.selectedGroup[tab]);
    if (!group) {ImGui::EndPopup();return;}
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(group->color),"Group");
    ImGui::SetNextItemWidth(280);
    if (ImGui::InputText("Title",editor.groupTitle,sizeof(editor.groupTitle)))
    {remember();group->title=editor.groupTitle;edited(false);}
    group=findGroup(graph,editor.selectedGroup[tab]);
    if(!group){ImGui::EndPopup();return;}
    ImVec4 color=ImGui::ColorConvertU32ToFloat4(group->color);
    if (ImGui::ColorEdit3("Color",&color.x))
    {remember();color.w=1;group->color=ImGui::ColorConvertFloat4ToU32(color);edited(false);}
    ImGui::TextDisabled("%zu nodes | Drag the header to move the group",group->nodes.size());
    if (ImGui::Button("Select members"))
    {
        clearSelection(tab);editor.selection[tab]=group->nodes;
        for(const auto& tie:graph.ties)if(tieInGroup(graph,tie.id,*group))editor.tieSelection[tab].push_back(tie.id);
        if(!editor.tieSelection[tab].empty())editor.selectedTie[tab]=editor.tieSelection[tab].back();
        if(!group->nodes.empty())editor.selected[tab]=group->nodes.back();ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Ungroup"))
    {
        const int id=group->id;remember();
        graph.groups.erase(std::remove_if(graph.groups.begin(),graph.groups.end(),[id](const Group& g) {return g.id==id;}),graph.groups.end());
        editor.selectedGroup[tab]=0;edited(false);ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
void tieInspector(Graph& graph,int tab)
{
    if(!beginEditorPopup("wire_tie_inspector"))return;
    auto* tie=findTie(graph,editor.selectedTie[tab]);
    if(tie)
    {
        ImGui::TextDisabled("Wire tie | %d wires",tieWireCount(graph,tie->id));
        float angle=tie->angle;
        if(ImGui::SliderFloat("Angle",&angle,-180,180,"%.0f deg")){rememberParameterChange();tie->angle=snapAngle(angle);edited(false);}
        if(ImGui::MenuItem("Rotate 15 degrees")){remember();tie->angle=normalizedAngle(tie->angle+15);edited(false);}
        if(ImGui::MenuItem("Reset rotation")){remember();tie->angle=0;edited(false);}
        if(ImGui::MenuItem("Frame selection","F"))frameSelection(graph,tab,editor.canvasSize[tab],true);
        ImGui::Separator();
        if(ImGui::MenuItem("Remove tie","Delete")){const int id=tie->id;remember();detachTie(graph,id);edited(false);}
    }
    ImGui::EndPopup();
}
enum class ToolIcon {Add,Inspect,Group,Tie,Bypass,Collapse,Undo,Redo,Arrange,Fit,Expand,Files,Restore,Tools,Help};
bool toolButton(const char* id,const char* tooltip,ToolIcon icon,bool enabled=true,bool accent=false)
{
    ImGui::BeginDisabled(!enabled);
    const ImVec2 p=ImGui::GetCursorScreenPos();
    const bool clicked=ImGui::InvisibleButton(id,ImVec2(32,32));
    if(editor.toolbarItemCount<editor.toolbarItems.size())
        editor.toolbarItems[editor.toolbarItemCount++]=ImVec4(p.x,p.y,p.x+32,p.y+32);
    const bool hover=ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImGui::PushID(id);const float hoverFade=visualEase("##tool_hover",hover&&enabled?1.0f:0.0f);ImGui::PopID();
    ImDrawList* draw=ImGui::GetWindowDrawList();
    const ImVec2 c(p.x+16,p.y+16);
    draw->AddRectFilled(p,add(p,ImVec2(32,32)),accent?IM_COL32(38,108,145,enabled?215:85):IM_COL32(39+static_cast<int>(23*hoverFade),42+static_cast<int>(25*hoverFade),48+static_cast<int>(28*hoverFade),255),6);
    if (accent) draw->AddRect(p,add(p,ImVec2(32,32)),IM_COL32(96,205,255,enabled?210:70),6);
    const ImU32 color=enabled?IM_COL32(222,231,241,255):IM_COL32(104,113,125,255);
    const auto line=[&](float x,float y,float u,float v) {draw->AddLine(add(c,ImVec2(x,y)),add(c,ImVec2(u,v)),color,1.7f);};
    switch(icon)
    {
    case ToolIcon::Add:line(-7,0,7,0);line(0,-7,0,7);break;
    case ToolIcon::Inspect:line(-7,-6,7,-6);line(-7,0,7,0);line(-7,6,7,6);draw->AddCircleFilled(add(c,ImVec2(-3,-6)),2.5f,color);draw->AddCircleFilled(add(c,ImVec2(4,0)),2.5f,color);draw->AddCircleFilled(add(c,ImVec2(-1,6)),2.5f,color);break;
    case ToolIcon::Group:draw->AddRect(add(c,ImVec2(-8,-7)),add(c,ImVec2(8,8)),color,2,0,1.6f);line(-8,-3,8,-3);draw->AddRectFilled(add(c,ImVec2(-4,0)),add(c,ImVec2(0,4)),color,1);draw->AddRectFilled(add(c,ImVec2(2,0)),add(c,ImVec2(6,4)),color,1);break;
    case ToolIcon::Tie:line(-9,-7,0,-2);line(-9,7,0,2);line(0,-2,9,-7);line(0,2,9,7);line(0,-7,0,8);draw->AddRectFilled(add(c,ImVec2(-2,-8)),add(c,ImVec2(2,-5)),color,1);break;
    case ToolIcon::Bypass:line(-9,5,-5,5);line(-5,5,-5,-5);line(-5,-5,7,-5);line(7,-5,3,-8);line(7,-5,3,-2);draw->AddRect(add(c,ImVec2(-2,0)),add(c,ImVec2(5,7)),color,1,0,1.5f);break;
    case ToolIcon::Collapse:line(-7,-6,0,0);line(0,0,7,-6);line(-7,6,7,6);break;
    case ToolIcon::Undo:case ToolIcon::Redo:
    {const float sign=icon==ToolIcon::Undo?1.0f:-1.0f;draw->AddBezierCubic(add(c,ImVec2(6*sign,6)),add(c,ImVec2(12*sign,-7)),add(c,ImVec2(-4*sign,-10)),add(c,ImVec2(-7*sign,-3)),color,1.7f);line(-7*sign,-3,-7*sign,-9);line(-7*sign,-3,-1*sign,-3);break;}
    case ToolIcon::Arrange:
        draw->AddRect(add(c,ImVec2(-9,-7)),add(c,ImVec2(-3,-1)),color,1,0,1.4f);
        draw->AddRect(add(c,ImVec2(3,-7)),add(c,ImVec2(9,-1)),color,1,0,1.4f);
        draw->AddRect(add(c,ImVec2(3,3)),add(c,ImVec2(9,9)),color,1,0,1.4f);
        line(-3,-4,3,-4);line(0,-4,0,6);line(0,6,3,6);break;
    case ToolIcon::Fit:for(float x:{-1.0f,1.0f})for(float y:{-1.0f,1.0f}){line(8*x,8*y,3*x,8*y);line(8*x,8*y,8*x,3*y);}break;
    case ToolIcon::Expand:line(-7,7,7,-7);line(-7,7,-7,1);line(-7,7,-1,7);line(7,-7,1,-7);line(7,-7,7,-1);break;
    case ToolIcon::Files:line(-8,3,-8,8);line(-8,8,8,8);line(8,8,8,3);line(-3,-8,-3,2);line(-6,-1,-3,2);line(-3,2,0,-1);line(5,2,5,-8);line(2,-5,5,-8);line(5,-8,8,-5);break;
    case ToolIcon::Restore:draw->PathArcTo(c,8,0.5f,5.5f,18);draw->PathStroke(color,0,1.7f);line(6,-6,0,-6);line(6,-6,6,-1);break;
    case ToolIcon::Tools:for(int x:{-6,0,6})draw->AddCircleFilled(add(c,ImVec2(static_cast<float>(x),0)),1.7f,color);break;
    case ToolIcon::Help:
        draw->AddCircle(c,9,color,0,1.5f);
        draw->AddBezierCubic(add(c,ImVec2(-3.3f,-3.5f)),add(c,ImVec2(-3.3f,-7)),add(c,ImVec2(3.5f,-7)),add(c,ImVec2(3.5f,-3.5f)),color,1.6f,10);
        draw->AddBezierCubic(add(c,ImVec2(3.5f,-3.5f)),add(c,ImVec2(3.5f,-0.5f)),add(c,ImVec2(0,-1)),add(c,ImVec2(0,1.6f)),color,1.6f,10);
        draw->AddCircleFilled(add(c,ImVec2(0,5)),1.05f,color,8);break;
    }
    ImGui::EndDisabled();
    if (hover) ImGui::SetTooltip("%s",tooltip);
    return clicked&&enabled;
}
void toolDivider()
{
    ImGui::SameLine(0,5);
    const ImVec2 p=ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(add(p,ImVec2(2,5)),add(p,ImVec2(2,27)),IM_COL32(91,101,116,105));
    ImGui::Dummy(ImVec2(5,32));ImGui::SameLine(0,5);
}
void collapseSelection(Graph& graph,int index)
{
    bool any=false,all=true;
    for(const auto& node:graph.nodes) if(isSelected(index,node.id)&&!reroute(node.kind)){any=true;all&=node.collapsed;}
    if(!any)return;
    remember();for(auto& node:graph.nodes)if(isSelected(index,node.id)&&!reroute(node.kind))node.collapsed=!all;
    edited(false);
}
void selectMarquee(const Graph& graph,int index,ImVec2 origin,ImDrawList* draw)
{
    const auto mouse=ImGui::GetIO().MousePos;
    const ImVec2 p(std::min(editor.marqueeStart.x,mouse.x),std::min(editor.marqueeStart.y,mouse.y));
    const ImVec2 q(std::max(editor.marqueeStart.x,mouse.x),std::max(editor.marqueeStart.y,mouse.y));
    editor.selectedGroup[index]=0;editor.selection[index]=editor.marqueeBase;editor.tieSelection[index]=editor.marqueeBaseTies;
    for(const auto& node:graph.nodes)
    {
        if(reroute(node.kind))
        {
            const auto center=rerouteCenter(graph,origin,node);
            const ImVec2 closest(std::clamp(center.x,p.x,q.x),std::clamp(center.y,p.y,q.y));
            const float radius=std::max(6.0f,10*graph.zoom);
            if(distanceSquared(center,closest)<=radius*radius&&!isSelected(index,node.id))editor.selection[index].push_back(node.id);
            continue;
        }
        const auto a=screenPoint(graph,origin,node.x,node.y),b=screenPoint(graph,origin,node.x+cardWidth(node),node.y+renderHeight(graph,node));
        if(a.x<=q.x&&b.x>=p.x&&a.y<=q.y&&b.y>=p.y&&!isSelected(index,node.id))editor.selection[index].push_back(node.id);
    }
    for(const auto& tie:graph.ties)
    {
        const auto center=screenPoint(graph,origin,tie.x,tie.y);
        const ImVec2 closest(std::clamp(center.x,p.x,q.x),std::clamp(center.y,p.y,q.y));
        if(distanceSquared(center,closest)<=49&&!isTieSelected(index,tie.id))editor.tieSelection[index].push_back(tie.id);
    }
    editor.selectedTie[index]=editor.tieSelection[index].empty()?0:editor.tieSelection[index].back();
    editor.selected[index]=editor.selection[index].empty()?0:editor.selection[index].back();
    draw->AddRectFilled(p,q,IM_COL32(96,205,255,22));draw->AddRect(p,q,IM_COL32(96,205,255,165));
}
void openCatalogAtCursor(const Graph& graph,int index)
{
    editor.catalogPosition=editor.canvasHovered[index]?editor.pastePosition[index]:ImVec2((editor.canvasSize[index].x*0.5f-graph.panX)/graph.zoom,(editor.canvasSize[index].y*0.5f-graph.panY)/graph.zoom);
    editor.catalogWireNode=0;editor.catalogFocus=true;editor.search[0]=0;ImGui::OpenPopup("node_catalog");
}
void nudgeSelection(Graph& graph,int index,float x,float y)
{
    if(editor.selection[index].empty()&&editor.tieSelection[index].empty())return;
    remember();for(auto& node:graph.nodes)if(isSelected(index,node.id)){node.x=snap(node.x+x);node.y=snap(node.y+y);}
    for(auto& tie:graph.ties)if(isTieSelected(index,tie.id)){tie.x=snap(tie.x+x);tie.y=snap(tie.y+y);}editor.autoFit[index]=false;edited(false);
}
void shortcutsPopup()
{
    ImGui::SetNextWindowSize(ImVec2(570,560),ImGuiCond_Appearing);
    if(!beginEditorPopup("graph_shortcuts"))return;
    ImGui::TextUnformatted("Keyboard and mouse");ImGui::Separator();
    ImGui::BeginChild("shortcut_rows",ImVec2(0,-40));
    ImGui::TextUnformatted("Signal colors");
    if(ImGui::BeginTable("signal_colors",3))
    {
        for(const auto type:{ValueType::Boolean,ValueType::Float,ValueType::Integer,ValueType::Point,ValueType::Detections,
            ValueType::Target,ValueType::Movement,ValueType::Frame,ValueType::Depth,ValueType::Mask,ValueType::String,ValueType::Monitor,ValueType::Monitors,ValueType::OverlayStyle,ValueType::LabelOptions})
        {
            ImGui::TableNextColumn();ImGui::PushStyleColor(ImGuiCol_Text,pinColor(type));
            ImGui::TextWrapped("%s",typeName(type));ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("Int connects to Float without precision loss. Use Float to Int to choose rounding.");ImGui::Separator();
    const std::pair<const char*,const char*> rows[]={
        {"LMB drag","Move freely; outline previews grid placement on release"},{"Empty canvas + LMB / MMB","Pan the graph"},{"Wire center + LMB drag","Horizontal: roundness; vertical: twist; Shift: fine"},{"Alt + click wire center","Reset bend; RMB / Esc cancels a bend drag"},{"Selected card edge / corner","Resize on the grid; RMB / Esc cancels"},{"RMB click / drag","Context menu / box selection"},{"Shift + RMB drag","Add to selection"},{"MMB drag / Wheel","Pan / zoom around cursor"},
        {"Shift+A / Ctrl+Space","Open node library; type to search"},{"Up / Down in search","Select a result; Down with no query browses all"},{"Tab / Enter in search","Add selected node; Esc closes the search"},{"?","Show this reference"},{"G","Move selected items with the cursor"},{"Shift+D","Duplicate and move selected nodes"},
        {"Enter / LMB","Confirm keyboard move"},{"Esc / RMB","Cancel move or unfinished connection"},{"A / Ctrl+A","Select all nodes and ties"},{"Alt+A","Deselect all"},{"Ctrl+I","Invert selection"},
        {"B","Box select with the next LMB drag"},{"Alt+L","Arrange graph by connections"},{"Home","Frame the whole graph"},{"F / Numpad .","Frame selection"},{"Arrow keys","Move selection by one grid cell"},
        {"Tab / Shift+Tab","Focus ports; move to the next / previous node"},{"Up / Down; Home / End","Choose a focused node's port"},{"Left / Right","Follow upstream / downstream from a focused port"},
        {"Enter / Space on port","Start or complete a connection"},{"Delete / Backspace on port","Disconnect port; preserve the node"},
        {"Ctrl+C / X / V","Copy / cut / paste selection"},{"Ctrl+D","Duplicate selection one grid cell away"},{"Ctrl+Z / Ctrl+Y","Undo / redo (Ctrl+Shift+Z also redoes)"},
        {"Ctrl+B","Bypass selected processing nodes"},{"Ctrl+M","Disable / enable selected nodes"},{"Ctrl+G","Group selected nodes"},{"Space","Collapse / expand selection"},
        {"Enter / Double-click node","Open node information"},{"X / Delete","Delete selection"},{"Z + LMB drag","Tie crossed wires"},{"Ctrl+RMB drag","Cut crossed connections"},
        {"Shift + tie drag","Rotate tie in 15-degree steps"},{"Double-click empty canvas","Add a reroute; first wire chooses its type"},{"Double-click wire","Cut connection"},{"Alt+click port","Disconnect port"},{"Ctrl+S","Flush automatic saving now"}};
    if(ImGui::BeginTable("shortcuts",2,ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("Shortcut",ImGuiTableColumnFlags_WidthFixed,178);
        for(const auto& row:rows){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Cyan),"%s",row.first);ImGui::TableNextColumn();ImGui::TextWrapped("%s",row.second);}
        ImGui::EndTable();
    }
    ImGui::EndChild();if(ImGui::Button("Close"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
}
constexpr ImU32 SnapPreviewFill=IM_COL32(72,190,228,30);
void drawLayoutSnapPreview(ImDrawList* draw,const Graph& graph,ImVec2 origin,ImVec2 end)
{
    if(!(editor.pointerMove||editor.keyboardMove)||!editor.moved||editor.resizeGroup||(editor.dragTie&&editor.rotateTie))return;
    const auto rectangle=[&](float x,float y,float width,float height,bool group=false)
    {
        const auto p=screenPoint(graph,origin,snap(x),snap(y));
        const auto q=add(p,ImVec2(width*graph.zoom,height*graph.zoom));
        if(q.x<origin.x||q.y<origin.y||p.x>end.x||p.y>end.y)return;
        draw->AddRectFilled(p,q,group?IM_COL32(72,190,228,10):SnapPreviewFill,4*graph.zoom);
        draw->AddRect(p,q,IM_COL32(72,190,228,140),4*graph.zoom,0,1.0f);
        const float length=std::clamp(8*graph.zoom,4.0f,12.0f);
        for(float xSide:{0.0f,1.0f})for(float ySide:{0.0f,1.0f})
        {
            const ImVec2 at(p.x+(q.x-p.x)*xSide,p.y+(q.y-p.y)*ySide);
            const auto outside=add(at,ImVec2(xSide?3.0f:-3.0f,ySide?3.0f:-3.0f));
            draw->AddLine(outside,add(outside,ImVec2(xSide?-length:length,0)),IM_COL32(97,209,242,180),1.0f);
            draw->AddLine(outside,add(outside,ImVec2(0,ySide?-length:length)),IM_COL32(97,209,242,180),1.0f);
        }
    };
    for(const auto& start:editor.dragGroupPositions)if(const auto* group=findGroup(graph,start.id))rectangle(group->x,group->y,group->width,group->height,true);
    if(const auto* group=findGroup(graph,editor.dragGroup))rectangle(group->x,group->y,group->width,group->height,true);
    for(const auto& start:editor.dragPositions)if(const auto* node=findNode(graph,start.id))
    {
        if(reroute(node->kind))
        {
            const auto point=screenPoint(graph,origin,snap(node->x)+32,snap(node->y)+16);
            draw->AddCircleFilled(point,7,SnapPreviewFill,12);draw->AddCircle(point,7,IM_COL32(97,209,242,180),12,1.2f);
        }
        else rectangle(node->x,node->y,cardWidth(*node),renderHeight(graph,*node));
    }
    for(const auto& start:editor.dragTiePositions)if(const auto* tie=findTie(graph,start.id))
    {
        const auto point=screenPoint(graph,origin,snap(tie->x),snap(tie->y));
        draw->AddCircleFilled(point,8,SnapPreviewFill,12);draw->AddCircle(point,8,IM_COL32(97,209,242,180),12,1.2f);
    }
}
void canvas(Graph& graph, int index)
{
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(1.0f, size.x); size.y = std::max(100.0f, size.y);
    const bool showProperties=editor.inspectorOpen;
    const bool dockProperties=showProperties&&size.x>=620;
    const float propertyWidth=showProperties?std::min(std::clamp(size.x*0.38f,300.0f,370.0f),std::max(100.0f,size.x-16)):0;
    if(dockProperties)size.x-=propertyWidth+8;
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##node_canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const auto canvasId=ImGui::GetItemID();
    if(ImGui::GetActiveID()==canvasId)ImGui::GetCurrentContext()->ActiveIdAllowOverlap=true;
    bool hovered=ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem|ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImVec2 origin = ImGui::GetItemRectMin();
    editor.canvasOrigin[index]=origin;editor.canvasHovered[index]=hovered;
    const ImVec2 end = add(origin,size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    auto& io = ImGui::GetIO();
    const bool emptyDoubleClick=editor.emptyClickCandidate&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)||ImGui::IsMouseClicked(ImGuiMouseButton_Right)||ImGui::IsMouseClicked(ImGuiMouseButton_Middle))editor.emptyClickCandidate=false;
    const bool editCancelled=(editor.resizeNode||editor.bendGraph>=0||editor.pointerMove||editor.keyboardMove)&&
        (ImGui::IsKeyPressed(ImGuiKey_Escape)||ImGui::IsMouseClicked(ImGuiMouseButton_Right));
    if(editCancelled)
    {
        finishNodeResize(false);finishWireBend(false);finishPointerMove(false);
        if(editor.keyboardMove)finishKeyboardMove(index,false);
        cancelGestures();hovered=false;if(ImGui::GetActiveID()==canvasId)ImGui::ClearActiveID();
    }
    // Update active layout gestures before hit testing and drawing so cards,
    // their wires and the snap preview all follow this frame's pointer.
    if(editor.pointerMove)
    {
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))finishPointerMove(true);
        else if(ImGui::IsMouseDown(ImGuiMouseButton_Left))updateLayoutMove(graph,io.MousePos);
        else{finishPointerMove(false);cancelLeftGesture();}
    }
    if(editor.keyboardMove)
    {
        updateLayoutMove(graph,io.MousePos);
        if(editor.confirmKeyboardMove)finishKeyboardMove(index,true);
    }
    if(editor.resizeNode)
    {
        const bool released=ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        if(ImGui::IsMouseDragging(ImGuiMouseButton_Left)||editor.resizeChanged||
            (released&&distanceSquared(io.MousePos,editor.dragStartMouse)>=io.MouseDragThreshold*io.MouseDragThreshold))updateNodeResize(graph,index,io.MousePos);
        if(released)finishNodeResize(true);
    }
    if(editor.inlineActiveItem!=ImGui::GetActiveID()){editor.inlineActiveItem=0;editor.inlineActiveNode=0;editor.inputEditNode=0;editor.inputEditPin=-1;}
    if(editor.inputPopup&&!ImGui::IsPopupOpen(editor.inputPopup,ImGuiPopupFlags_AnyPopupLevel))
    {editor.inputPopup=0;editor.inputPopupNode=0;editor.inputPopupPin=-1;}
    const bool controlActive=editor.inlineActiveItem!=0;
    int settingsHovered=0,inputHovered=0,inputHoveredPin=-1;bool settingsPortPriority=false;
    if(hovered)
        for(auto it=graph.nodes.rbegin();it!=graph.nodes.rend();++it)
        {
            const auto p=screenPoint(graph,origin,it->x,it->y),q=add(p,ImVec2(cardWidth(*it)*graph.zoom,renderHeight(graph,*it)*graph.zoom));
            const auto center=rerouteCenter(graph,origin,*it);
            if(reroute(it->kind)?distanceSquared(center,io.MousePos)>std::pow(std::max(6.0f,10*graph.zoom),2):!contains(io.MousePos,p,q))continue;
            if(!compactNode(graph,*it)&&hasNodeSettings(*it)&&settingsRect(graph,origin,*it).Contains(io.MousePos))settingsHovered=it->id;
            if(!compactNode(graph,*it)&&!reroute(it->kind)&&graph.zoom>=0.55f)
            {
                const auto& inputs=definition(it->kind).inputs;std::uint64_t connected=0;
                for(const auto& link:graph.links)if(link.toNode==it->id)connected|=std::uint64_t{1}<<link.toPort;
                const float font=ImGui::GetFontSize()*graph.zoom;
                for(std::size_t pin=0;pin<inputs.size();++pin)
                {
                    if(!scalarInput(inputs[pin])||(connected&(std::uint64_t{1}<<pin))!=0)continue;
                    const auto pos=portPoint(graph,origin,*it,false,static_cast<int>(pin));
                    if(std::abs(io.MousePos.y-pos.y)>9*graph.zoom||!editableInput(*it,pin,connected))continue;
                    const auto nodeIndex=static_cast<std::size_t>(it.base()-graph.nodes.begin()-1);
                    auto& displays=editor.canvasRoutes[index].displays;NodeDisplay temporary;
                    auto& display=nodeIndex<displays.size()&&displays[nodeIndex].id==it->id?displays[nodeIndex]:temporary;
                    const float desired=inputValueWidth(display,*it,pin,connected,ImGui::GetFont(),font,graph.zoom);
                    if(inputValueRect(graph,*it,pin,origin,font,desired).Contains(io.MousePos))
                    {inputHovered=it->id;inputHoveredPin=static_cast<int>(pin);break;}
                }
            }
            break;
        }
    if((settingsHovered||inputHovered)&&!controlActive)
        for(const auto& node:graph.nodes)
            for(int side=0;side<2;++side)
            {
                const auto& pins=side?definition(node.kind).outputs:definition(node.kind).inputs;
                for(std::size_t pin=0;pin<pins.size();++pin)
                    if((side||Layout::inputVisible(node,pin))&&distanceSquared(portPoint(graph,origin,node,side!=0,static_cast<int>(pin)),io.MousePos)<std::pow(std::max(7.0f,8*graph.zoom),2))settingsPortPriority=true;
            }
    if(settingsPortPriority){settingsHovered=inputHovered=0;inputHoveredPin=-1;}
    if(editor.keyboardMove&&ImGui::IsMouseClicked(ImGuiMouseButton_Left))editor.confirmKeyboardMove=true;
    if(editor.autoFit[index]&&(std::abs(editor.canvasSize[index].x-size.x)>1||std::abs(editor.canvasSize[index].y-size.y)>1))editor.fit[index]=true;
    editor.canvasSize[index]=size;
    if (editor.fit[index]) {fitGraph(graph,size);editor.fit[index]=false;edited(false);}
    if(editor.focusSelection[index])
    {revealSelection(graph,index,ImVec2(showProperties&&!dockProperties?std::max(1.0f,size.x-propertyWidth-16):size.x,size.y));editor.focusSelection[index]=false;}
    const bool leftGesture=editor.wireNode||editor.dragNode||editor.resizeNode||editor.dragGroup||editor.resizeGroup||editor.dragTie||editor.tieStroke||editor.keyboardMove||editor.cutStroke||editor.bendGraph>=0;
    if((settingsHovered||inputHovered)&&!leftGesture&&editor.panButton<0&&!editor.rightGesture)
    {
        // Win32 can deliver mouse entry and press together. Hand that press
        // from the background item to the actual switch/input this frame.
        if(ImGui::GetActiveID()==canvasId)ImGui::ClearActiveID();
        if(ImGui::GetKeyOwner(ImGuiKey_MouseLeft)==canvasId)ImGui::SetKeyOwner(ImGuiKey_MouseLeft,ImGuiKeyOwner_NoOwner);
    }
    if(hovered&&!settingsHovered&&!inputHovered&&!controlActive&&!leftGesture&&!editor.rightGesture&&!ImGui::IsMouseDown(ImGuiMouseButton_Left)&&!ImGui::IsMouseDown(ImGuiMouseButton_Right)&&ImGui::IsMouseClicked(ImGuiMouseButton_Middle))beginPan(graph,ImGuiMouseButton_Middle);
    bool openContext=false;
    if(editor.panButton>=0)
    {
        const int button=editor.panButton;
        const bool released=ImGui::IsMouseReleased(button);
        const ImVec2 delta(io.MousePos.x-editor.panStartMouse.x,io.MousePos.y-editor.panStartMouse.y);
        if(distanceSquared(io.MousePos,editor.panStartMouse)>=io.MouseDragThreshold*io.MouseDragThreshold){editor.panned=true;editor.emptyClickCandidate=false;}
        if(editor.panned&&(ImGui::IsMouseDown(button)||released))
        {
            const float x=editor.panStart.x+delta.x,y=editor.panStart.y+delta.y;
            if(graph.panX!=x||graph.panY!=y){graph.panX=x;graph.panY=y;editor.autoFit[index]=false;edited(false);}
        }
        if(released)editor.panButton=-1;
        else if(!ImGui::IsMouseDown(button))editor.panButton=-1;
    }
    if (hovered && !settingsHovered && !inputHovered && !controlActive && !leftGesture && !editor.rightGesture && editor.panButton<0 && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && io.MouseWheel != 0)
    {
        const float zoom = std::clamp(graph.zoom*std::pow(1.15f,io.MouseWheel), 0.25f, 1.75f);
        const float ratio = zoom/graph.zoom;
        graph.panX = io.MousePos.x-origin.x-(io.MousePos.x-origin.x-graph.panX)*ratio;
        graph.panY = io.MousePos.y-origin.y-(io.MousePos.y-origin.y-graph.panY)*ratio;
        graph.zoom = zoom;
        editor.autoFit[index]=false;
        edited(false);
    }
    if(hovered)editor.pastePosition[index]=ImVec2((io.MousePos.x-origin.x-graph.panX)/graph.zoom,(io.MousePos.y-origin.y-graph.panY)/graph.zoom);
    ImGui::PushClipRect(origin,end,true);
    draw->AddRectFilled(origin,end,IM_COL32(27,29,33,255),7);
    for (int major=0;major<2;++major)
    {
        const float spacing=GridSize*(major?4:1)*graph.zoom;
        if (!major && spacing<12) continue;
        const ImU32 color=major?IM_COL32(95,110,128,65):IM_COL32(70,77,87,static_cast<int>(45*std::clamp(graph.zoom,0.25f,1.0f)));
        for (float x=origin.x+std::fmod(graph.panX,spacing);x<end.x;x+=spacing) draw->AddLine(ImVec2(x,origin.y),ImVec2(x,end.y),color);
        for (float y=origin.y+std::fmod(graph.panY,spacing);y<end.y;y+=spacing) draw->AddLine(ImVec2(origin.x,y),ImVec2(end.x,y),color);
    }
    drawLayoutSnapPreview(draw,graph,origin,end);
    int hoverGroup=0,hoverResizeGroup=0;
    for (const auto& group:graph.groups)
    {
        const ImVec2 p=screenPoint(graph,origin,group.x,group.y);
        const ImVec2 q=screenPoint(graph,origin,group.x+group.width,group.y+group.height);
        const ImU32 rgb=group.color&0x00ffffff;
        const bool selected=editor.selectedGroup[index]==group.id;
        draw->AddRectFilled(p,q,rgb|IM_COL32(0,0,0,13),7*graph.zoom);
        draw->AddRectFilled(p,ImVec2(q.x,p.y+28*graph.zoom),rgb|IM_COL32(0,0,0,selected?65:38),7*graph.zoom);
        draw->AddRect(p,q,rgb|IM_COL32(0,0,0,selected?220:100),7*graph.zoom,0,selected?2.0f:1.0f);
        const ImVec4 clip(p.x+10*graph.zoom,p.y,q.x-12*graph.zoom,p.y+28*graph.zoom);
        draw->AddText(ImGui::GetFont(),ImGui::GetFontSize()*graph.zoom,add(p,ImVec2(10*graph.zoom,5*graph.zoom)),rgb|IM_COL32(0,0,0,235),group.title.c_str(),nullptr,0,&clip);
        draw->AddLine(ImVec2(q.x-12*graph.zoom,q.y-4*graph.zoom),ImVec2(q.x-4*graph.zoom,q.y-12*graph.zoom),rgb|IM_COL32(0,0,0,190),1.5f);
        const bool overHeader=hovered&&contains(io.MousePos,p,ImVec2(q.x,p.y+28*graph.zoom));
        const bool overResize=hovered&&contains(io.MousePos,add(q,ImVec2(-14*graph.zoom,-14*graph.zoom)),add(q,ImVec2(3,3)));
        // The topmost group control owns both the target and the action.
        if(overHeader||overResize){hoverGroup=group.id;hoverResizeGroup=overResize?group.id:0;}
    }
    drawCutAnimations(draw,graph,index,origin,end);
    const auto signalNow=std::chrono::steady_clock::now();
    const bool fresh=telemetryFresh(index,signalNow);
    auto& geometry=prepareRoutes(graph,index,origin);
    const auto& routes=geometry.routes;
    int hoverLink=-1,hoverTie=0;
    float nearestLink=36;
    std::size_t signalPulses=0;
    const std::size_t pulseBudget=graph.zoom<0.4f?16:graph.nodes.size()>=96?32:MaxSignalPulses;
    for (size_t linkIndex=0;linkIndex<graph.links.size();++linkIndex)
    {
        const auto& link=graph.links[linkIndex];
        const auto& route=routes[linkIndex];if(route.path.empty())continue;
        if(route.maximum.x<origin.x-8||route.maximum.y<origin.y-8||route.minimum.x>end.x+8||route.minimum.y>end.y+8)continue;
        const Node* from=&graph.nodes[route.from];const Node* to=&graph.nodes[route.to];
        const auto& outputs = definition(from->kind).outputs;
        if (link.fromPort < 0 || static_cast<size_t>(link.fromPort) >= outputs.size()) continue;
        const auto* sourceSample=nodeSample(index,from->id,fresh);
        const Value* value = sourceSample?&sourceSample->value:nullptr;
        const auto& path=route.path;
        if(hovered)
            if(const float distance=wireDistance(path,io.MousePos);distance<nearestLink)
            {nearestLink=distance;hoverLink=static_cast<int>(linkIndex);}
        const bool active=value&&value->valid&&!(value->type==ValueType::Boolean&&!value->boolean);
        const ImU32 color=wireColor(outputs[link.fromPort].type,active,from->disabled||to->disabled);
        const auto event=wireSignal(sourceSample,nodeSample(index,to->id,fresh),link.toPort,signalNow);
        auto& animation=editor.wireAnimations[{index,link.fromNode,link.fromPort,link.toNode,link.toPort}];
        advanceWireAnimation(animation,route.length,graph.zoom,event,ImGui::GetTime(),ImGui::GetFrameCount(),signalPulses<pulseBudget);
        if(!fresh)animation.activity=0;
        if(animation.activity>0.001f)
        {
            if(signalPulses>=pulseBudget)animation.activity=0;
            else ++signalPulses;
        }
        wire(draw,route,color,2*graph.zoom,animation,graph.zoom);
    }
    editor.drawnSignalPulses=signalPulses;
    int hoverBend=drawWireBendHandle(draw,graph,index,routes,origin,end,hovered,leftGesture,hoverLink);
    for(const auto& tie:graph.ties)
    {
        const auto center=screenPoint(graph,origin,tie.x,tie.y),direction=tieDirection(tie);const ImVec2 axis(-direction.y,direction.x);
        const auto extent=multiply(axis,tieHalfSpan(graph,tie.id)*graph.zoom);
        const bool over=hovered&&wireDistance({add(center,multiply(extent,-1)),add(center,extent)},io.MousePos)<=std::pow(std::max(7.0f,7*graph.zoom),2);
        if(over)hoverTie=tie.id;
        ImGui::PushID(tie.id);const float fade=visualEase("##tie_hover",over?1.0f:0.0f);ImGui::PopID();
        drawWireTie(draw,graph,tie,origin,isTieSelected(index,tie.id),fade);
    }
    int hoverNode = 0, hoverPort = -1, hoverCollapse = 0,hoverResizeNode=0,hoverResizeEdges=0;
    std::vector<int> previewNodes;
    if(index==3)for(const auto& node:graph.nodes)
    {
        if(node.kind!=NodeKind::ImagePreview||node.disabled||node.bypassed||compactNode(graph,node))continue;
        if(parameter(node,"enabled")==0&&std::none_of(graph.links.begin(),graph.links.end(),[&](const Link& link){return link.toNode==node.id&&link.toPort==0;}))continue;
        if(previewRect(graph,origin,node).Overlaps(ImRect(origin,end)))
        {if(isSelected(index,node.id))previewNodes.insert(previewNodes.begin(),node.id);else previewNodes.push_back(node.id);}
    }
    if(previewNodes.size()>MaxLivePreviews)previewNodes.resize(MaxLivePreviews);
    service().setPreviewNodes(previewNodes);NodePreview::retain(previewNodes);
    bool hoverOutput = false;
    int nearestPortNode=0,nearestPort=-1;
    int hoverDefaultNode=-1,hoverDefaultPin=-1;
    bool nearestPortOutput=false;
    float nearestPortDistance=std::numeric_limits<float>::max();
    for (std::size_t nodeIndex=0;nodeIndex<graph.nodes.size();++nodeIndex)
    {
        const auto& node=graph.nodes[nodeIndex];
        const auto& def = definition(node.kind);
        const ImVec2 p = screenPoint(graph,origin,node.x,node.y);
        const ImVec2 q = add(p,ImVec2(cardWidth(node)*graph.zoom,renderHeight(graph,node)*graph.zoom));
        if (q.x+8 < origin.x || q.y+8 < origin.y || p.x-8 > end.x || p.y-8 > end.y) continue;
        const bool isReroute=reroute(node.kind);
        const bool overview=!isReroute&&overviewNode(graph);
        const auto point=rerouteCenter(graph,origin,node);
        const float pointHitRadius=std::max(6.0f,10*graph.zoom);
        const bool overBody=hovered&&(isReroute?distanceSquared(io.MousePos,point)<=pointHitRadius*pointHitRadius:contains(io.MousePos,p,q));
        if (overBody) {hoverNode=node.id;hoverPort=-1;hoverCollapse=0;hoverResizeNode=hoverResizeEdges=0;}
        ImGui::PushID(node.id);
        const float hoverFade=visualEase("##node_hover",overBody?1.0f:0.0f);
        const float handleReach=std::max(14.0f,44*graph.zoom);
        const float handleFade=isReroute?visualEase("##reroute_handles",hovered&&distanceSquared(io.MousePos,point)<=handleReach*handleReach?1.0f:0.0f):1.0f;
        ImGui::PopID();
        const bool selected = isSelected(index,node.id);
        const auto* sample=nodeSample(index,node.id,fresh);
        const Value* value=sample?&sample->value:nullptr;
        auto& display=geometry.displays[nodeIndex];
        const float signal=updateNodeDisplay(display,node,sample,isReroute?overBody:(!overview&&graph.zoom>=0.4f)||overBody,graph.nodes.size()>=96,
            ImGui::GetTime(),signalNow);
        const ImU32 accent = node.disabled?IM_COL32(133,141,154,255):node.bypassed?IM_COL32(211,176,114,255):def.outputs.empty() ? Cyan : pinColor(def.outputs.front().type);
        if(!isReroute)
        {
            const bool active=sample&&(sample->active||(value->valid&&ImGui::GetTime()<display.rapidUntil));
            drawNodeShell(draw,p,q,HeaderHeight*graph.zoom,graph.zoom,accent,node.bypassed,selected,hoverFade,active,signal);
        }
        const float font = std::max(graph.zoom<0.4f?8.0f:0.0f,ImGui::GetFontSize()*graph.zoom);
        if (!reroute(node.kind))
        {
            const ImVec2 c=add(p,ImVec2(12*graph.zoom,14*graph.zoom));
            if (node.collapsed) draw->AddTriangleFilled(add(c,ImVec2(-3*graph.zoom,-4*graph.zoom)),add(c,ImVec2(-3*graph.zoom,4*graph.zoom)),add(c,ImVec2(3*graph.zoom,0)),IM_COL32(175,186,200,255));
            else draw->AddTriangleFilled(add(c,ImVec2(-4*graph.zoom,-2*graph.zoom)),add(c,ImVec2(4*graph.zoom,-2*graph.zoom)),add(c,ImVec2(0,4*graph.zoom)),IM_COL32(175,186,200,255));
            if (hovered&&contains(io.MousePos,add(p,ImVec2(3*graph.zoom,3*graph.zoom)),add(p,ImVec2(23*graph.zoom,25*graph.zoom)))) hoverCollapse=node.id;
            const bool badge=(node.bypassed||node.disabled)&&graph.zoom>=0.55f;
            if(overview)
            {
                const auto& caption=overviewCaption(display,node,graph.zoom,ImGui::GetFont(),ImGui::GetFontSize());
                float y=p.y+caption.top+(q.y-p.y-caption.top-caption.extent.y)*0.5f;
                const ImVec4 clip(p.x+2,p.y+2,q.x-2,q.y-2);
                const ImU32 color=value&&!value->error.empty()?IM_COL32(241,111,119,255):node.disabled?IM_COL32(150,159,173,255):
                    node.bypassed?IM_COL32(223,190,132,255):IM_COL32(238,241,245,255);
                for(const auto& line:caption.lines)
                {
                    const ImVec2 position(p.x+(q.x-p.x-line.width)*0.5f,y);
                    draw->AddText(ImGui::GetFont(),caption.fontSize,position,color,caption.text.c_str()+line.begin,caption.text.c_str()+line.end,0,&clip);
                    y+=caption.fontSize;
                }
            }
            else
            {
                const ImVec4 clip(p.x+24*graph.zoom,p.y,q.x-(badge?57:6)*graph.zoom,p.y+HeaderHeight*graph.zoom);
                draw->AddText(ImGui::GetFont(),font,add(p,ImVec2(24*graph.zoom,6*graph.zoom)),node.bypassed?IM_COL32(155,160,169,255):IM_COL32(238,241,245,255),def.title,nullptr,0,&clip);
            }
            if (badge)
            {
                const ImVec2 badge(q.x-55*graph.zoom,p.y+7*graph.zoom);
                draw->AddRectFilled(add(badge,ImVec2(-3*graph.zoom,-2*graph.zoom)),ImVec2(q.x-5*graph.zoom,p.y+23*graph.zoom),IM_COL32(123,95,42,95),3*graph.zoom);
                draw->AddText(ImGui::GetFont(),font*0.65f,badge,node.disabled?IM_COL32(165,174,188,255):IM_COL32(223,190,132,255),node.disabled?"DISABLED":"BYPASS");
            }
        }
        for (int side = 0; side < 2; ++side)
        {
            const auto& pins = side ? def.outputs : def.inputs;
            for (size_t pin = 0; pin < pins.size(); ++pin)
            {
                if(!side&&!Layout::inputVisible(node,pin))continue;
                const ImVec2 pos = portPoint(graph,origin,node,side!=0,static_cast<int>(pin));
                const float portDistance=distanceSquared(pos,io.MousePos);
                const float hitRadius=reroute(node.kind)?std::min(std::max(7.0f,8*graph.zoom),cardWidth(node)*graph.zoom*0.25f):std::max(7.0f,8*graph.zoom);
                const bool over = hovered && portDistance < hitRadius*hitRadius;
                if (over&&portDistance<=nearestPortDistance)
                {
                    nearestPortDistance=portDistance;nearestPortNode=node.id;
                    nearestPort=static_cast<int>(pin);nearestPortOutput=side!=0;
                }
                const bool connected=(geometry.connections[nodeIndex][side]&(std::uint64_t{1}<<pin))!=0;
                if(!isReroute||handleFade>0.004f)
                {
                    const float radius=isReroute?std::clamp(3.0f*graph.zoom,1.5f,4.0f):4*graph.zoom;
                    const int alpha=static_cast<int>(255*handleFade);
                    const auto color=(pinColor(pins[pin].type)&0x00ffffff)|IM_COL32(0,0,0,alpha);
                    draw->AddCircleFilled(pos,radius+1,IM_COL32(27,29,33,alpha));
                    if (connected&&!isReroute) draw->AddCircleFilled(pos,radius,pinColor(pins[pin].type));
                    else draw->AddCircle(pos,radius,color,0,1.3f);
                }
                if(!isReroute&&!compactNode(graph,node)&&pos.y+font>=origin.y&&pos.y-font<=end.y)
                {
                    const float textWidth=side?ImGui::GetFont()->CalcTextSizeA(font,10000,0,pins[pin].name).x:0;
                    const float detail=editor.viewMode==1?1.0f:std::clamp((graph.zoom-0.5f)/0.18f,0.0f,1.0f);
                    const auto row=side?pin:Layout::inputRow(node,pin);
                    const bool shared=side?row<Layout::inputCount(node):row<def.outputs.size();
                    const float inputRight=inputLabelRight(graph,node,pin,origin,font);
                    ImVec4 clip(side&&shared?inputRight+8*graph.zoom:p.x+10*graph.zoom,p.y, !side&&shared?inputRight:q.x-10*graph.zoom,q.y);
                    if(!side&&!connected&&detail>0.01f)
                    {
                        inputDefault(display,node,pin,geometry.connections[nodeIndex][0]);auto& cached=display.defaults[pin];
                        if(cached.labelFont!=ImGui::GetFont()||cached.labelFontSize!=font)
                        {
                            cached.labelFont=ImGui::GetFont();cached.labelFontSize=font;
                            cached.labelWidth=ImGui::GetFont()->CalcTextSizeA(font,std::numeric_limits<float>::max(),0,pins[pin].name).x;
                        }
                        const bool editable=editableInput(node,pin,geometry.connections[nodeIndex][0]);
                        const float desired=inputValueWidth(display,node,pin,geometry.connections[nodeIndex][0],ImGui::GetFont(),font,graph.zoom);
                        const auto bounds=inputValueRect(graph,node,pin,origin,font,desired,cached.labelWidth);
                        const bool model=modelPathInput(node.kind,pins[pin].defaultParameter);
                        const auto& fallback=fitInputDefault(display,node,pin,geometry.connections[nodeIndex][0],ImGui::GetFont(),font,std::max(0.0f,bounds.GetWidth()-(model?24:6)*graph.zoom));
                        const bool editNow=editable&&graph.zoom>=0.55f&&!leftGesture&&!editor.rightGesture&&editor.panButton<0&&!settingsPortPriority&&
                            ((inputHovered==node.id&&inputHoveredPin==static_cast<int>(pin))||(editor.inputEditNode==node.id&&editor.inputEditPin==static_cast<int>(pin))||
                             (editor.inputPopupNode==node.id&&editor.inputPopupPin==static_cast<int>(pin)));
                        const bool drawn=editNow&&inlineInputDefault(graph,index,node.id,pin,display,bounds,graph.zoom);
                        if(!drawn&&!fallback.shown.empty())
                        {
                            const auto tint=pinColor(pins[pin].type)&0x00ffffff;
                            draw->AddRectFilled(bounds.Min,bounds.Max,tint|IM_COL32(0,0,0,static_cast<int>(22*detail)),3*graph.zoom);
                            draw->AddText(ImGui::GetFont(),font,ImVec2(bounds.Min.x+3*graph.zoom,pos.y-font*0.5f),tint|IM_COL32(0,0,0,static_cast<int>(220*detail)),fallback.shown.c_str());
                            if(model&&bounds.GetWidth()>=24*graph.zoom)
                            {
                                const auto center=ImVec2(bounds.Max.x-9*graph.zoom,pos.y);const auto color=tint|IM_COL32(0,0,0,static_cast<int>(220*detail));
                                draw->AddLine(add(center,ImVec2(-3*graph.zoom,-1.5f*graph.zoom)),add(center,ImVec2(0,1.5f*graph.zoom)),color,graph.zoom);
                                draw->AddLine(add(center,ImVec2(0,1.5f*graph.zoom)),add(center,ImVec2(3*graph.zoom,-1.5f*graph.zoom)),color,graph.zoom);
                            }
                        }
                        if(bounds.GetWidth()>0)
                        {
                            clip.z=bounds.Min.x-6*graph.zoom;
                            if(hovered&&bounds.Contains(io.MousePos)){hoverDefaultNode=static_cast<int>(nodeIndex);hoverDefaultPin=static_cast<int>(pin);}
                        }
                    }
                    draw->AddText(ImGui::GetFont(),font,ImVec2(side ? pos.x-12*graph.zoom-textWidth : pos.x+12*graph.zoom,pos.y-font*0.5f),IM_COL32(184,192,204,static_cast<int>(255*detail)),pins[pin].name,nullptr,0,&clip);
                }
            }
        }
        if(isReroute)drawReroutePoint(draw,point,rerouteRadius(graph.zoom),accent,selected,std::max(hoverFade,signal));
        if(node.kind==NodeKind::ImagePreview&&!compactNode(graph,node))
        {
            const auto panel=previewRect(graph,origin,node);draw->AddRectFilled(panel.Min,panel.Max,IM_COL32(17,19,23,255),4*graph.zoom);
            draw->PushClipRect(panel.Min,panel.Max,true);
            const bool requested=std::find(previewNodes.begin(),previewNodes.end(),node.id)!=previewNodes.end();
            const auto image=requested&&value?value->preview:Preview{};
            const auto texture=NodePreview::texture(node.id,image,g_pd3dDevice,g_pd3dDeviceContext);
            if(texture)
            {
                const float scale=std::min(panel.GetWidth()/image->width,panel.GetHeight()/image->height);
                const ImVec2 size(image->width*scale,image->height*scale),a=add(panel.GetCenter(),multiply(size,-0.5f));
                draw->AddImage(texture,a,add(a,size));
            }
            else
            {
                const char* label=node.disabled?"Disabled":node.bypassed?"Bypassed":!requested?"Preview paused":
                    value&&!value->error.empty()?"Preview unavailable":(geometry.connections[nodeIndex][0]&14)==0?"Connect Frame, Depth or Mask":"Waiting for image";
                const auto size=ImGui::GetFont()->CalcTextSizeA(font,10000,0,label);
                draw->AddText(ImGui::GetFont(),font,add(panel.GetCenter(),multiply(size,-0.5f)),IM_COL32(137,146,159,255),label);
            }
            draw->PopClipRect();draw->AddRect(panel.Min,panel.Max,IM_COL32(66,73,83,150),4*graph.zoom);
        }
        const auto& text=display.text;
        if (!reroute(node.kind)&&!overview&&graph.zoom>=0.4f)
        {
            const ImVec4 clip(p.x+8*graph.zoom,p.y+HeaderHeight*graph.zoom,q.x-8*graph.zoom,q.y-2*graph.zoom);
            const auto valueColor=value&&!value->error.empty()?IM_COL32(241,111,119,255):value&&value->valid?accent:IM_COL32(137,146,159,255);
            if(signal>0.01f&&!text.empty()&&!compactNode(graph,node))draw->AddRectFilled(ImVec2(p.x+6*graph.zoom,q.y-27*graph.zoom),ImVec2(q.x-6*graph.zoom,q.y-5*graph.zoom),(accent&0x00ffffff)|IM_COL32(0,0,0,static_cast<int>(32*signal)),3*graph.zoom);
            if(!text.empty())draw->AddText(ImGui::GetFont(),font,ImVec2(p.x+12*graph.zoom,q.y-24*graph.zoom),valueColor,text.c_str(),nullptr,0,&clip);
        }
        else if (overBody)
        {
            if(isReroute)ImGui::SetTooltip("%s%s%s\nDrag the point to move. Drag a side handle to connect.\nDrop a wire on the point to connect.",looseReroute(graph,node)?"Reroute | First wire chooses the type":def.title,text.empty()?"":"  |  ",text.c_str());
            else ImGui::SetTooltip("%s%s%s%s",def.title,node.disabled?"  [DISABLED]":node.bypassed?"  [BYPASS]":"",text.empty()?"":"\n",text.c_str());
        }
        if(hovered&&settingsHovered!=node.id&&hoverNode==node.id&&hoverPort<0&&contains(io.MousePos,p,q)&&value&&
            (!value->error.empty()||value->type==ValueType::Target||value->type==ValueType::Detections||
             value->type==ValueType::Frame||value->type==ValueType::Depth||value->type==ValueType::Mask||value->type==ValueType::String||value->type==ValueType::Monitor||value->type==ValueType::Monitors))dataTooltip(*value);
        if(!isReroute&&!compactNode(graph,node)&&hasNodeSettings(node))
        {
            const auto panel=settingsRect(graph,origin,node);
            const bool canEdit=!leftGesture&&!editor.rightGesture&&editor.panButton<0&&!settingsPortPriority&&
                (!panel.Contains(io.MousePos)||settingsHovered==node.id||editor.inlineActiveNode==node.id);
            inlineSettings(graph,index,node.id,display,origin,canEdit);
        }
        if(selected&&!isReroute&&!node.collapsed)
        {
            const float half=std::clamp(3*graph.zoom,2.0f,4.0f);
            const ImVec2 handles[]={p,ImVec2((p.x+q.x)*0.5f,p.y),ImVec2(q.x,p.y),ImVec2(q.x,(p.y+q.y)*0.5f),
                q,ImVec2((p.x+q.x)*0.5f,q.y),ImVec2(p.x,q.y),ImVec2(p.x,(p.y+q.y)*0.5f)};
            for(const auto handle:handles)
            {
                bool socket=false;
                for(int side=0;side<2;++side)for(std::size_t pin=0;pin<(side?def.outputs:def.inputs).size();++pin)
                    if(distanceSquared(handle,portPoint(graph,origin,node,side!=0,static_cast<int>(pin)))<std::pow(4*graph.zoom+half+1,2))socket=true;
                if(socket)continue;
                draw->AddRectFilled(add(handle,ImVec2(-half,-half)),add(handle,ImVec2(half,half)),IM_COL32(27,36,43,255),1);
                draw->AddRect(add(handle,ImVec2(-half,-half)),add(handle,ImVec2(half,half)),Cyan,1);
            }
            if(hovered&&!leftGesture&&editor.panButton<0&&!editor.rightGesture)
                if(const int edges=nodeResizeHit(graph,node,origin,io.MousePos)){hoverResizeNode=node.id;hoverResizeEdges=edges;}
        }
    }
    const bool inlineActive=editor.inlineActiveItem&&editor.inlineActiveItem==ImGui::GetActiveID();
    if(settingsHovered||inputHovered||inlineActive)
    {
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const int id=inlineActive?editor.inlineActiveNode:inputHovered?inputHovered:settingsHovered;
            if(id&&!isSelected(index,id))selectOne(index,id);
            if(ImGui::GetActiveID()==canvasId)ImGui::ClearActiveID();
        }
        hovered=false;
    }
    // Pick once after all cards: compact pin hit circles may overlap, and a
    // later card body must not overwrite the nearest visible port.
    // The visible chevron wins over a neighboring pin's expanded hit halo.
    // Exact port centers lie outside the chevron rectangle and stay connectable.
    if(nearestPortNode&&!hoverCollapse)
    {
        hoverNode=nearestPortNode;hoverPort=nearestPort;hoverOutput=nearestPortOutput;hoverCollapse=0;
        const auto* node=findNode(graph,hoverNode);
        const auto& pin=hoverOutput?definition(node->kind).outputs[hoverPort]:definition(node->kind).inputs[hoverPort];
        const auto pos=portPoint(graph,origin,*node,hoverOutput,hoverPort);
        draw->AddCircle(pos,std::max(6.0f,6*graph.zoom),pinColor(pin.type),0,1.5f);
        if(!editor.wireNode)
        {
            const auto nodeIndex=static_cast<std::size_t>(node-graph.nodes.data());
            if(!hoverOutput&&(geometry.connections[nodeIndex][0]&(std::uint64_t{1}<<hoverPort))==0)
                inputDefaultTooltip(pin,inputDefault(geometry.displays[nodeIndex],*node,hoverPort,geometry.connections[nodeIndex][0]),!compactNode(graph,*node)&&graph.zoom>=0.55f&&editableInput(*node,hoverPort,geometry.connections[nodeIndex][0]));
            else ImGui::SetTooltip("%s %s : %s\nDrag to connect. Alt+click to disconnect.",hoverOutput?"Output":"Input",pin.name,typeName(pin.type));
        }
    }
    if(hoverResizeNode&&!hoverCollapse&&hoverPort<0)
    {hoverNode=hoverResizeNode;hoverGroup=hoverResizeGroup=hoverTie=0;ImGui::SetTooltip("Resize node | Drag an edge or corner\nRMB / Esc: cancel | Size menu: automatic or presets");}
    else hoverResizeNode=hoverResizeEdges=0;
    if(hoverDefaultNode>=0&&hoverPort<0&&!hoverResizeNode&&!hoverCollapse&&!editor.wireNode&&!leftGesture&&!inlineActive&&hoverNode==graph.nodes[hoverDefaultNode].id)
    {
        const auto& node=graph.nodes[hoverDefaultNode];
        inputDefaultTooltip(definition(node.kind).inputs[hoverDefaultPin],inputDefault(geometry.displays[hoverDefaultNode],node,hoverDefaultPin,geometry.connections[hoverDefaultNode][0]),graph.zoom>=0.55f&&editableInput(node,hoverDefaultPin,geometry.connections[hoverDefaultNode][0]));
    }
    if(editor.wireNode&&hoverNode!=editor.wireNode&&hoverNode&&hoverPort<0)
        if(const auto* node=findNode(graph,hoverNode);node&&reroute(node->kind))
        {hoverPort=0;hoverOutput=!editor.wireOutput;}
    if(const auto* focused=findNode(graph,editor.portFocus[index].node))
    {
        const auto port=editor.portFocus[index];const auto& pins=port.output?definition(focused->kind).outputs:definition(focused->kind).inputs;
        if(port.port>=0&&static_cast<std::size_t>(port.port)<pins.size())
        {
            const auto point=reroute(focused->kind)?rerouteCenter(graph,origin,*focused):portPoint(graph,origin,*focused,port.output,port.port);
            draw->AddCircle(point,8,Cyan,0,1.7f);
            const std::string label=std::string(port.output?"Out: ":"In: ")+pins[port.port].name;
            const auto text=add(point,ImVec2(12,-26)),extent=ImGui::CalcTextSize(label.c_str());
            draw->AddRectFilled(add(text,ImVec2(-4,-2)),add(text,add(extent,ImVec2(4,2))),IM_COL32(24,28,34,240),3);
            draw->AddText(text,Cyan,label.c_str());
        }
    }
    if(hoverNode||hoverGroup||hoverResizeGroup)hoverTie=0;
    if(hoverNode||hoverPort>=0||hoverTie||hoverGroup||hoverResizeGroup||hoverResizeNode||hoverCollapse)hoverBend=-1;
    if(hoverBend>=0)hoverLink=hoverBend;
    const bool tieTool=editor.tieMode||(!io.KeyCtrl&&!io.WantTextInput&&ImGui::IsKeyDown(ImGuiKey_Z));
    if(hoverTie&&!leftGesture&&!editor.rightGesture)
        ImGui::SetTooltip("Wire tie | %d wires\nLMB: move | Shift+LMB: rotate | RMB: actions\nDouble-click or Delete: remove the tie",tieWireCount(graph,hoverTie));
    bool cutByDoubleClick=false;
    if (hovered && !leftGesture && !editor.rightGesture && !tieTool && editor.panButton<0 && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !hoverNode && !hoverTie && !hoverGroup && !hoverResizeGroup && hoverLink>=0)
    {
        if(hoverBend>=0)ImGui::SetTooltip("Drag center: horizontal roundness / vertical twist\nShift+drag: fine | Alt+click: reset | RMB: actions\nDouble-click: cut connection");
        else ImGui::SetTooltip("Drag the center handle to bend | Double-click to cut");
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            const auto& route=routes[hoverLink];const auto at=nearestPathDistance(route.path,io.MousePos);
            queueCutAnimation(graph,index,graph.links[hoverLink],route,wirePosition(route.path,at));
            remember();graph.links.erase(graph.links.begin()+hoverLink);cancelGestures();edited();cutByDoubleClick=true;
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        editor.portFocus[index]={};
        if(editor.keyboardMove)finishKeyboardMove(index,false);
        else if(leftGesture||editor.panButton>=0||ImGui::IsMouseDown(ImGuiMouseButton_Left))cancelGestures();
        else if(io.KeyCtrl&&!ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {editor.cutStroke=true;editor.cutStart=editor.cutLast=io.MousePos;editor.cutHits.clear();}
        else if(!ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            editor.marqueeButton=ImGuiMouseButton_Right;
            editor.rightGesture=true;editor.marquee=false;editor.marqueeStart=io.MousePos;
            editor.marqueeBase=io.KeyShift||io.KeyCtrl?editor.selection[index]:std::vector<int>{};
            editor.marqueeBaseTies=io.KeyShift||io.KeyCtrl?editor.tieSelection[index]:std::vector<int>{};
            editor.contextNode=hoverNode;editor.contextGroup=hoverGroup?hoverGroup:hoverResizeGroup;editor.contextTie=hoverTie;
            editor.hasContextLink=!hoverNode&&!hoverGroup&&!hoverResizeGroup&&!hoverTie&&hoverLink>=0;
            if(editor.hasContextLink)editor.contextLink=linkKey(graph.links[hoverLink]);
            editor.catalogPosition=ImVec2((io.MousePos.x-origin.x-graph.panX)/graph.zoom,(io.MousePos.y-origin.y-graph.panY)/graph.zoom);
        }
    }
    if(editor.rightGesture&&(ImGui::IsMouseDown(editor.marqueeButton)||ImGui::IsMouseReleased(editor.marqueeButton)))
    {
        if(distanceSquared(io.MousePos,editor.marqueeStart)>=io.MouseDragThreshold*io.MouseDragThreshold)editor.marquee=true;
        if(editor.marquee)selectMarquee(graph,index,origin,draw);
        if(ImGui::IsMouseReleased(editor.marqueeButton))
        {openContext=!editor.marquee&&hovered;editor.rightGesture=editor.marquee=false;}
    }
    if(openContext)
    {
        if(editor.contextTie&&findTie(graph,editor.contextTie))
        {if(!isTieSelected(index,editor.contextTie))selectOneTie(index,editor.contextTie);editor.selectedTie[index]=editor.contextTie;ImGui::OpenPopup("wire_tie_inspector");}
        else if(editor.contextNode&&findNode(graph,editor.contextNode))
        {if(!isSelected(index,editor.contextNode))selectOne(index,editor.contextNode);editor.selected[index]=editor.contextNode;ImGui::OpenPopup("node_inspector");}
        else if (editor.contextGroup&&findGroup(graph,editor.contextGroup))
        {
            clearSelection(index);editor.selectedGroup[index]=editor.contextGroup;
            if (const auto* group=findGroup(graph,editor.selectedGroup[index])) std::snprintf(editor.groupTitle,sizeof(editor.groupTitle),"%s",group->title.c_str());
            ImGui::OpenPopup("group_inspector");
        }
        else if(editor.hasContextLink)ImGui::OpenPopup("wire_inspector");
        else
        {
            editor.catalogWireNode=0;editor.catalogFocus=true;editor.search[0]=0; ImGui::OpenPopup("node_catalog");
        }
    }
    if (hovered && !cutByDoubleClick && editor.panButton<0 && !editor.rightGesture && !openContext && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        editor.toggleSelectionOnRelease=0;
        editor.portFocus[index]={};
        if(editor.keyboardWire)editor.keyboardWire=false;
        else if(editor.keyboardMove)editor.confirmKeyboardMove=true;
        else if(editor.boxSelectMode)
        {
            editor.boxSelectMode=false;editor.rightGesture=editor.marquee=true;editor.marqueeButton=ImGuiMouseButton_Left;editor.marqueeStart=io.MousePos;
            editor.marqueeBase=io.KeyShift||io.KeyCtrl?editor.selection[index]:std::vector<int>{};editor.marqueeBaseTies=io.KeyShift||io.KeyCtrl?editor.tieSelection[index]:std::vector<int>{};
        }
        else if(tieTool&&!hoverNode&&!hoverGroup&&!hoverResizeGroup)
        {editor.tieStroke=true;editor.tieStart=editor.tieLast=io.MousePos;editor.tieHits.clear();editor.crossedTies.clear();}
        else if(hoverTie)
        {
            if(!isTieSelected(index,hoverTie))selectOneTie(index,hoverTie);editor.selectedTie[index]=hoverTie;
            if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)&&!io.KeyShift)
            {remember();detachTie(graph,hoverTie);edited(false);}
            else
            {
                editor.dragTie=hoverTie;editor.rotateTie=io.KeyShift;editor.moved=false;editor.dragStartMouse=io.MousePos;
                captureDragSelection(graph,index);
                beginPointerMove(index);
            }
        }
        else if (hoverCollapse && hoverPort<0)
        {
            if (Node* node=findNode(graph,hoverCollapse)) {remember();node->collapsed=!node->collapsed;edited(false);}
        }
        else if (hoverNode && hoverPort >= 0)
        {
            if (!isSelected(index,hoverNode)) selectOne(index,hoverNode);
            editor.selected[index]=hoverNode;
            if (io.KeyAlt)
            {
                remember();
                graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[&](const Link& link) {
                    return hoverOutput ? link.fromNode==hoverNode && link.fromPort==hoverPort : link.toNode==hoverNode && link.toPort==hoverPort;
                }),graph.links.end());
                edited();
            }
            else { editor.wireNode=hoverNode; editor.wirePort=hoverPort; editor.wireOutput=hoverOutput; }
        }
        else if(hoverResizeNode)
        {
            if(!isSelected(index,hoverResizeNode))selectOne(index,hoverResizeNode);
            editor.selected[index]=hoverResizeNode;editor.selectedGroup[index]=0;beginNodeResize(index,hoverResizeNode,hoverResizeEdges);
        }
        else if (hoverNode)
        {
            editor.selectedGroup[index]=0;
            if (io.KeyShift||io.KeyCtrl)
            {
                auto& selected=editor.selection[index];
                auto found=std::find(selected.begin(),selected.end(),hoverNode);
                if (found==selected.end()) selected.push_back(hoverNode);
                else editor.toggleSelectionOnRelease=hoverNode;
                editor.selected[index]=selected.empty()?0:selected.back();
            }
            else if (!isSelected(index,hoverNode)) selectOne(index,hoverNode);
            else editor.selected[index]=hoverNode;
            if (isSelected(index,hoverNode))
            {
                editor.dragNode=hoverNode;editor.moved=false;editor.dragStartMouse=io.MousePos;
                captureDragSelection(graph,index);
                beginPointerMove(index);
            }
            if (!io.KeyShift && !io.KeyCtrl && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))openProperties(index);
        }
        else if (hoverGroup || hoverResizeGroup)
        {
            clearSelection(index);editor.selectedGroup[index]=hoverGroup?hoverGroup:hoverResizeGroup;
            if (auto* group=findGroup(graph,editor.selectedGroup[index]))
            {
                editor.dragStartMouse=io.MousePos;editor.dragStartGroup=ImVec2(group->x,group->y);
                editor.dragStartSize=ImVec2(group->width,group->height);editor.moved=false;editor.dragPositions.clear();editor.dragTiePositions.clear();editor.dragGroupPositions.clear();
                if (hoverResizeGroup) editor.resizeGroup=group->id;
                else
                {
                    editor.dragGroup=group->id;
                    for (int id:group->nodes) if (const auto* node=findNode(graph,id)) editor.dragPositions.push_back({id,node->x,node->y});
                    for(const auto& tie:graph.ties)if(tieInGroup(graph,tie.id,*group))editor.dragTiePositions.push_back({tie.id,tie.x,tie.y,tie.angle});
                }
                beginPointerMove(index);
            }
        }
        else if(hoverBend>=0)
        {
            if(io.KeyAlt)resetWireBend(graph.links[hoverBend]);
            else beginWireBend(index,graph.links[hoverBend]);
        }
        else
        {
            if(emptyDoubleClick&&!io.KeyCtrl&&!io.KeyShift&&!io.KeyAlt&&!editor.panned&&!editor.wireNode&&!editor.popupNavigation)
                addLooseReroute(graph,index,ImVec2((io.MousePos.x-origin.x-graph.panX)/graph.zoom,(io.MousePos.y-origin.y-graph.panY)/graph.zoom));
            else{beginPan(graph,ImGuiMouseButton_Left);editor.emptyClickCandidate=!editor.popupNavigation;}
        }
    }
    if(editor.bendGraph==index)
    {
        const bool released=ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        if(editor.bendChanged||ImGui::IsMouseDragging(ImGuiMouseButton_Left)||
            (released&&distanceSquared(io.MousePos,editor.bendStartMouse)>=io.MouseDragThreshold*io.MouseDragThreshold))updateWireBend(io.MousePos);
        if(released)finishWireBend(true);
    }
    if(editor.resizeNode)
    {
        if(const auto* node=findNode(graph,editor.resizeNode))ImGui::SetTooltip("%.0f x %.0f | RMB / Esc: cancel",cardWidth(*node),Layout::height(*node,true));
    }
    if(editor.keyboardMove)
    {
        ImGui::SetTooltip("Move selection | LMB / Enter: confirm | RMB / Esc: cancel");
        if(editor.confirmKeyboardMove)finishKeyboardMove(index,true);
    }
    if(editor.cutStroke&&(ImGui::IsMouseDown(ImGuiMouseButton_Right)||ImGui::IsMouseReleased(ImGuiMouseButton_Right)))
    {
        collectWireHits(graph,routes,editor.cutLast,io.MousePos,editor.cutHits);editor.cutLast=io.MousePos;
        draw->AddLine(editor.cutStart,io.MousePos,IM_COL32(245,117,122,255),2);
        for(const auto& hit:editor.cutHits)draw->AddCircleFilled(hit.point,3,IM_COL32(245,117,122,255));
        ImGui::SetTooltip("Release to cut %zu connections | Esc: cancel",editor.cutHits.size());
        if(ImGui::IsKeyPressed(ImGuiKey_Escape))editor.cutStroke=false;
        else if(ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        {
            if(!editor.cutHits.empty())
            {
                std::size_t animated=0;
                for(std::size_t i=0;i<graph.links.size()&&animated<MaxCutAnimations;++i)
                    if(const auto hit=std::find_if(editor.cutHits.begin(),editor.cutHits.end(),[&](const TieHit& item){return item.link==linkKey(graph.links[i]);});hit!=editor.cutHits.end())
                    {queueCutAnimation(graph,index,graph.links[i],routes[i],hit->point);++animated;}
                remember();graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[&](const Link& link){
                    return std::any_of(editor.cutHits.begin(),editor.cutHits.end(),[&](const TieHit& hit){return hit.link==linkKey(link);});}),graph.links.end());edited();
            }
            editor.cutStroke=false;editor.cutHits.clear();
        }
    }
    if(editor.tieStroke&&(ImGui::IsMouseDown(ImGuiMouseButton_Left)||ImGui::IsMouseReleased(ImGuiMouseButton_Left)))
    {
        if(ImGui::IsKeyPressed(ImGuiKey_Escape)){cancelGestures();editor.tieMode=false;}
        else
        {
            for(const auto& tie:graph.ties)
            {
                if(std::find(editor.crossedTies.begin(),editor.crossedTies.end(),tie.id)!=editor.crossedTies.end())continue;
                const auto center=screenPoint(graph,origin,tie.x,tie.y),direction=tieDirection(tie);const ImVec2 axis(-direction.y,direction.x);
                const auto extent=multiply(axis,tieHalfSpan(graph,tie.id)*graph.zoom);float ignored=0;
                if(segmentIntersection(editor.tieLast,io.MousePos,add(center,multiply(extent,-1)),add(center,extent),ignored)||
                    (distanceSquared(editor.tieLast,io.MousePos)>0.01f&&wireDistance({editor.tieLast,io.MousePos},center)<std::pow(std::max(4.0f,4*graph.zoom),2)))editor.crossedTies.push_back(tie.id);
            }
            collectWireHits(graph,routes,editor.tieLast,io.MousePos,editor.tieHits);editor.tieLast=io.MousePos;
            draw->AddLine(editor.tieStart,io.MousePos,editor.crossedTies.empty()?Cyan:IM_COL32(245,117,122,255),1.5f);
            for(const auto& hit:editor.tieHits)draw->AddCircleFilled(hit.point,3,Cyan);
            if(editor.crossedTies.empty())ImGui::SetTooltip("Release to tie %zu wires | RMB or Esc cancels",editor.tieHits.size());
            else ImGui::SetTooltip("Release to remove %zu ties; connections stay intact | RMB or Esc cancels",editor.crossedTies.size());
            if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                if(!editor.crossedTies.empty())
                {remember();for(const int id:editor.crossedTies)detachTie(graph,id);edited(false);}
                else
                {
                    ImVec2 center;for(const auto& hit:editor.tieHits)center=add(center,hit.point);
                    if(!editor.tieHits.empty())center=multiply(center,1.0f/editor.tieHits.size());
                    center=ImVec2((center.x-origin.x-graph.panX)/graph.zoom,(center.y-origin.y-graph.panY)/graph.zoom);
                    float angle=std::atan2(io.MousePos.y-editor.tieStart.y,io.MousePos.x-editor.tieStart.x)*180/Pi-90;
                    if(angle>90)angle-=180;if(angle<-90)angle+=180;
                    std::string error;if(!createWireTie(graph,index,editor.tieHits,center,angle,error)){editor.message=error;editor.error=true;}
                }
                editor.tieStroke=editor.tieMode=false;editor.tieHits.clear();editor.crossedTies.clear();
            }
        }
    }
    if (editor.wireNode)
    {
        const auto focus=editor.portFocus[index];const int targetNode=editor.keyboardWire?focus.node:hoverNode;
        const int targetPort=editor.keyboardWire?focus.port:hoverPort;const bool targetOutput=editor.keyboardWire?focus.output:hoverOutput;
        std::string connectionError;
        bool compatible=false;
        Link candidate;
        if(targetNode&&targetPort>=0&&targetOutput!=editor.wireOutput)
        {
            candidate=editor.wireOutput?Link{editor.wireNode,editor.wirePort,targetNode,targetPort}:Link{targetNode,targetPort,editor.wireNode,editor.wirePort};
            compatible=canConnectReroutes(graph,candidate,connectionError);
        }
        if (const Node* node=findNode(graph,editor.wireNode))
        {
            const ImVec2 pos=reroute(node->kind)?rerouteCenter(graph,origin,*node):portPoint(graph,origin,*node,editor.wireOutput,editor.wirePort);
            ImVec2 endpoint=io.MousePos;
            if(compatible)
            {
                const auto* target=findNode(graph,targetNode);
                endpoint=reroute(target->kind)?rerouteCenter(graph,origin,*target):portPoint(graph,origin,*target,targetOutput,targetPort);
            }
            const auto& anchorPins=editor.wireOutput?definition(node->kind).outputs:definition(node->kind).inputs;
            auto type=anchorPins[editor.wirePort].type;
            if(compatible)type=connectionType(graph,candidate);
            const ImU32 color=compatible||connectionError.empty()?pinColor(type):IM_COL32(241,111,119,255);
            previewWire(draw,editor.wireOutput?pos:endpoint,editor.wireOutput?endpoint:pos,color);
            if(compatible)ImGui::SetTooltip("Release to connect. RMB or Esc to cancel.");
            else if(!connectionError.empty())ImGui::SetTooltip("%s",connectionError.c_str());
            else if(hovered&&!hoverNode)ImGui::SetTooltip("Release to add a compatible node. RMB or Esc to cancel.");
        }
        if (!editor.keyboardWire&&ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (compatible)
            {
                remember();adaptLooseReroutes(graph,candidate);
                graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[&](const Link& old) { return old.toNode==candidate.toNode && old.toPort==candidate.toPort; }),graph.links.end());
                graph.links.push_back(candidate); edited();
            }
            else if(!connectionError.empty()){editor.message=connectionError;editor.error=true;}
            else if(hovered&&!hoverNode&&!hoverGroup&&!hoverResizeGroup)
            {
                editor.catalogWireNode=editor.wireNode;editor.catalogWirePort=editor.wirePort;editor.catalogWireOutput=editor.wireOutput;
                editor.catalogPosition=editor.pastePosition[index];editor.search[0]=0;editor.catalogFocus=true;
                ImGui::OpenPopup("node_catalog");
            }
            editor.wireNode=0;
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        if(editor.toggleSelectionOnRelease)
        {
            auto& selected=editor.selection[index];
            selected.erase(std::remove(selected.begin(),selected.end(),editor.toggleSelectionOnRelease),selected.end());
            editor.selected[index]=selected.empty()?0:selected.back();
        }
        editor.toggleSelectionOnRelease=0;
        editor.dragNode=editor.dragGroup=editor.resizeGroup=editor.dragTie=0;
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)&&ImGui::IsKeyPressed(ImGuiKey_Escape))
    {if(editor.keyboardMove)finishKeyboardMove(index,false);cancelGestures();editor.portFocus[index]={};}
    if(editor.resizeNode||(hovered&&hoverResizeNode))ImGui::SetMouseCursor(resizeCursor(editor.resizeNode?editor.resizeMask:hoverResizeEdges));
    else if(editor.panButton>=0||editor.dragNode||editor.dragGroup||editor.dragTie||editor.keyboardMove||editor.bendGraph>=0)ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    else if(editor.resizeGroup||(hovered&&hoverResizeGroup))ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    else if(hovered&&(hoverCollapse||hoverPort>=0||hoverTie||tieTool||hoverBend>=0))ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    draw->AddRect(origin,end,IM_COL32(65,70,79,255),7);
    const auto& notice=editor.loadFailed?editor.loadError:editor.message;
    if(!notice.empty())
    {
        if(editor.shownMessage!=notice){editor.shownMessage=notice;editor.messageAt=ImGui::GetTime();}
        const bool persistentError=editor.error||editor.loadFailed;
        const float alpha=persistentError?1.0f:std::clamp(static_cast<float>(4.0-(ImGui::GetTime()-editor.messageAt)),0.0f,1.0f);
        if(alpha>0)
        {
            const float wrap=std::max(60.0f,size.x-32);
            const auto extent=ImGui::CalcTextSize(notice.c_str(),nullptr,false,wrap);
            const ImVec2 at(origin.x+16,std::max(origin.y+12,end.y-extent.y-20));
            draw->AddRectFilled(ImVec2(origin.x+8,at.y-8),ImVec2(end.x-8,end.y-8),IM_COL32(33,36,42,static_cast<int>(245*alpha)),5);
            draw->AddText(ImGui::GetFont(),ImGui::GetFontSize(),at,persistentError?IM_COL32(250,147,137,255):IM_COL32(177,198,216,static_cast<int>(255*alpha)),notice.c_str(),nullptr,wrap);
        }
    }
    ImGui::PopClipRect();
    catalog(graph,index);
    inspector(graph,index);
    wireInspector(graph,index,origin);
    groupInspector(graph,index);
    tieInspector(graph,index);
    if(showProperties)propertiesPanel(graph,index,ImVec2(dockProperties?end.x+8:end.x-propertyWidth-8,origin.y),ImVec2(propertyWidth,size.y));
}
void drawEditor(int index)
{
    if (!editor.initialized)
    {
        editor.draft=service().workspace(); editor.initialized=true;editor.observedRevision=service().revision();
        for (int i=0;i<static_cast<int>(GraphCount);++i)
        {
            auto& graph = editor.draft.graphs[i];
            snapLayout(graph);
            editor.fit[i] = graph.panX == 40 && graph.panY == 40 && graph.zoom == 1;
            editor.autoFit[i]=editor.fit[i];
        }
    }
    if(editor.observedRevision!=service().revision())
    {
        editor.draft=service().workspace();editor.observedRevision=service().revision();
        editor.undo.clear();editor.redo.clear();cancelGestures();
    }
    if(editor.resizeNode&&!ImGui::IsMouseDown(ImGuiMouseButton_Left)&&!ImGui::IsMouseReleased(ImGuiMouseButton_Left))finishNodeResize(false);
    if (editor.activeGraph != index)NodeEditor_EndInteraction();
    else if(!editor.keyboardMove&&!editor.keyboardWire&&!ImGui::IsMouseDown(ImGuiMouseButton_Left)&&!ImGui::IsMouseReleased(ImGuiMouseButton_Left))cancelLeftGesture();
    if(!ImGui::IsMouseDown(editor.marqueeButton)&&!ImGui::IsMouseReleased(editor.marqueeButton))editor.rightGesture=editor.marquee=false;
    if(!ImGui::IsMouseDown(ImGuiMouseButton_Right)&&!ImGui::IsMouseReleased(ImGuiMouseButton_Right))editor.cutStroke=false;
    editor.activeGraph = index;
    if (ImGui::GetTime()-editor.telemetryAt>1.0/30)
    {refreshTelemetry();}
    Graph& graph=editor.draft.graphs[index];
    ImGui::PushID(index);
    editor.toolbarItemCount=0;
    const bool hasSelection=!editor.selection[index].empty();
    const auto* selectedGroup=findGroup(graph,editor.selectedGroup[index]);
    const bool canCopySelection=hasSelection||(selectedGroup&&!selectedGroup->nodes.empty());
    bool bypassable=false;
    for(const auto& node:graph.nodes)if(isSelected(index,node.id)&&canBypass(node.kind))bypassable=true;
    ImGui::BeginDisabled(editor.keyboardMove);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(4,5));
    if (toolButton("add","Add node",ToolIcon::Add))
    { editor.catalogPosition=ImVec2((80-graph.panX)/graph.zoom,(60-graph.panY)/graph.zoom);editor.catalogWireNode=0;editor.catalogFocus=true;editor.search[0]=0;ImGui::OpenPopup("node_catalog"); }
    ImGui::SameLine();
    if(toolButton("inspect","Node information (Enter / double-click node)",ToolIcon::Inspect,true,editor.inspectorOpen))
    {if(editor.inspectorOpen)editor.inspectorOpen=false;else openProperties(index);}
    ImGui::SameLine();
    if(toolButton("group","Group selected nodes (Ctrl+G)",ToolIcon::Group,hasSelection))groupSelection(graph,index);
    ImGui::SameLine();
    if(toolButton("tie","Tie wires: drag across connections (Z + drag)",ToolIcon::Tie,true,editor.tieMode))editor.tieMode=!editor.tieMode;
    ImGui::SameLine();
    if(toolButton("bypass","Toggle bypass (Ctrl+B)",ToolIcon::Bypass,bypassable))bypassSelection(graph,index);
    ImGui::SameLine();
    if(toolButton("collapse","Collapse / expand selected nodes (Space)",ToolIcon::Collapse,hasSelection))collapseSelection(graph,index);
    toolDivider();
    if(toolButton("undo","Undo (Ctrl+Z)",ToolIcon::Undo,!editor.undo.empty()))history(false);
    ImGui::SameLine();
    if(toolButton("redo","Redo (Ctrl+Y / Ctrl+Shift+Z)",ToolIcon::Redo,!editor.redo.empty()))history(true);
    toolDivider();
    if(toolButton("arrange","Arrange graph by connections (Alt+L)\nSpace expanded cards, fit groups and keep settings. Undo restores positions.",ToolIcon::Arrange,!graph.nodes.empty()))arrangeGraph(graph,index);
    ImGui::SameLine();
    if(toolButton("fit","Fit graph",ToolIcon::Fit)){editor.fit[index]=editor.autoFit[index]=true;}
    ImGui::SameLine();
    if(toolButton("expand","Expand editor and fit graph",ToolIcon::Expand)){Overlay_ExpandForNodes();editor.fit[index]=editor.autoFit[index]=true;}
    toolDivider();
    if(toolButton("files","Import / export all graphs",ToolIcon::Files))ImGui::OpenPopup("graph_files");
    ImGui::SameLine();
    if(toolButton("restore","Restore this starter graph",ToolIcon::Restore))ImGui::OpenPopup("restore_graph");
    ImGui::SameLine();
    if(toolButton("tools","Selection tools and keyboard shortcuts",ToolIcon::Tools))ImGui::OpenPopup("graph_tools");
    ImGui::SameLine();
    const float available=ImGui::GetContentRegionAvail().x;
    if(available>=32)ImGui::SetCursorPosX(ImGui::GetCursorPosX()+available-32);
    if(toolButton("help","Keyboard and mouse reference (?)\nAll edits apply immediately and save automatically.",ToolIcon::Help))ImGui::OpenPopup("graph_shortcuts");
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
    if(beginEditorPopup("graph_tools"))
    {
        if(ImGui::MenuItem("Select all","Ctrl+A"))selectAll(graph,index);
        if(ImGui::MenuItem("Copy selected","Ctrl+C",false,canCopySelection))copySelection(graph,index);
        if(ImGui::MenuItem("Cut selected","Ctrl+X",false,canCopySelection))cutSelection(graph,index);
        if(ImGui::MenuItem("Paste selection","Ctrl+V"))pasteSelection(graph,index);
        if(ImGui::MenuItem("Duplicate selected","Ctrl+D",false,canCopySelection))duplicateSelection(graph,index);
        if(ImGui::MenuItem("Frame selection","F",false,hasSelection||!editor.tieSelection[index].empty()||editor.selectedGroup[index]!=0))frameSelection(graph,index,editor.canvasSize[index],true);
        if(ImGui::MenuItem("Arrange graph","Alt+L",false,!graph.nodes.empty()))arrangeGraph(graph,index);
        if(ImGui::MenuItem("Group selected","Ctrl+G",false,hasSelection))groupSelection(graph,index);
        if(ImGui::MenuItem("Bypass selected","Ctrl+B",false,bypassable))bypassSelection(graph,index);
        if(ImGui::MenuItem("Disable / enable selected","Ctrl+M",false,hasSelection))disableSelection(graph,index);
        if(ImGui::MenuItem("Keyboard and mouse","?"))editor.showShortcuts=true;
        if(ImGui::MenuItem("Collapse / expand selected","Space",false,hasSelection))collapseSelection(graph,index);
        ImGui::Separator();
        ImGui::TextDisabled("Node detail");
        if(ImGui::MenuItem("Automatic with zoom",nullptr,editor.viewMode==0))editor.viewMode=0;
        if(ImGui::MenuItem("Detailed",nullptr,editor.viewMode==1))editor.viewMode=1;
        if(ImGui::MenuItem("Compact",nullptr,editor.viewMode==2))editor.viewMode=2;
        ImGui::Separator();
        if(ImGui::MenuItem("Delete selection","Delete",false,hasSelection||!editor.tieSelection[index].empty()||editor.selectedGroup[index]!=0))deleteSelection(graph,index);
        ImGui::EndPopup();
    }
    if (beginEditorPopup("graph_files"))
    {
        ImGui::TextDisabled("All graphs, layout and values");
        if (ImGui::MenuItem("Import graphs...")) editor.fileAction=1;
        if (ImGui::MenuItem("Export graphs...")) editor.fileAction=2;
        ImGui::EndPopup();
    }
    if (beginEditorPopup("restore_graph"))
    {
        ImGui::TextUnformatted("Restore the starter graph for this tab?");
        if (ImGui::Button("Restore"))
        {
            remember();graph=defaults().graphs[index];editor.fit[index]=editor.autoFit[index]=true;editor.loadFailed=false;
            clearSelection(index);editor.selectedGroup[index]=0;cancelGestures();edited();ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if(ImGui::GetIO().KeyCtrl&&ImGui::IsKeyPressed(ImGuiKey_S)&&!editor.keyboardMove&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)&&
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)||(editor.inspectorOpen&&ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))))NodeEditor_Flush();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsAnyItemActive()&&!ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId)&&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left)&&!ImGui::IsMouseDown(ImGuiMouseButton_Middle)&&!ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        const auto& io=ImGui::GetIO();
        if(editor.keyboardMove)
        {
            if(ImGui::IsKeyPressed(ImGuiKey_Enter)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))editor.confirmKeyboardMove=true;
        }
        else if(io.KeyCtrl)
        {
            if(ImGui::IsKeyPressed(ImGuiKey_Z))history(io.KeyShift);
            if(ImGui::IsKeyPressed(ImGuiKey_Y))history(true);
            if(ImGui::IsKeyPressed(ImGuiKey_A))selectAll(graph,index);
            if(ImGui::IsKeyPressed(ImGuiKey_C))copySelection(graph,index);
            if(ImGui::IsKeyPressed(ImGuiKey_X))cutSelection(graph,index);
            if(ImGui::IsKeyPressed(ImGuiKey_V))pasteSelection(graph,index);
            if(ImGui::IsKeyPressed(ImGuiKey_D))duplicateSelection(graph,index);
            if(ImGui::IsKeyPressed(ImGuiKey_G))groupSelection(graph,index);
            if(ImGui::IsKeyPressed(ImGuiKey_I))invertSelection(graph,index);
            if(ImGui::IsKeyPressed(ImGuiKey_B))bypassSelection(graph,index);
            if(ImGui::IsKeyPressed(ImGuiKey_M))disableSelection(graph,index);
            if(ImGui::IsKeyPressed(ImGuiKey_Space))openCatalogAtCursor(graph,index);
        }
        else
        {
            if(io.KeyAlt&&ImGui::IsKeyPressed(ImGuiKey_L))arrangeGraph(graph,index);
            if(ImGui::IsKeyPressed(ImGuiKey_A)){if(io.KeyAlt){clearSelection(index);editor.selectedGroup[index]=0;}else if(io.KeyShift)openCatalogAtCursor(graph,index);else selectAll(graph,index);}
            if(ImGui::IsKeyPressed(ImGuiKey_G))beginKeyboardMove(graph,index,false);
            if(io.KeyShift&&ImGui::IsKeyPressed(ImGuiKey_D))beginKeyboardMove(graph,index,true);
            if(ImGui::IsKeyPressed(ImGuiKey_B))editor.boxSelectMode=true;
            if(ImGui::IsKeyPressed(ImGuiKey_F)||ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal))frameSelection(graph,index,editor.canvasSize[index],true);
            if(io.KeyShift&&ImGui::IsKeyPressed(ImGuiKey_Slash))ImGui::OpenPopup("graph_shortcuts");
            if(ImGui::IsKeyPressed(ImGuiKey_Tab))nextPortNode(graph,index,io.KeyShift);
            if(!findNode(graph,editor.portFocus[index].node))editor.portFocus[index]={};
            if(editor.portFocus[index].node)
            {
                if(ImGui::IsKeyPressed(ImGuiKey_UpArrow))cyclePort(graph,index,-1);
                if(ImGui::IsKeyPressed(ImGuiKey_DownArrow))cyclePort(graph,index,1);
                if(ImGui::IsKeyPressed(ImGuiKey_Home))cyclePort(graph,index,-1,true);
                if(ImGui::IsKeyPressed(ImGuiKey_End))cyclePort(graph,index,1,true);
                if(ImGui::IsKeyPressed(ImGuiKey_LeftArrow))followPort(graph,index,false);
                if(ImGui::IsKeyPressed(ImGuiKey_RightArrow))followPort(graph,index,true);
                if(ImGui::IsKeyPressed(ImGuiKey_Enter)||ImGui::IsKeyPressed(ImGuiKey_Space))activatePort(graph,index);
                if(ImGui::IsKeyPressed(ImGuiKey_Delete)||ImGui::IsKeyPressed(ImGuiKey_Backspace))disconnectPort(graph,index);
            }
            else
            {
                if(ImGui::IsKeyPressed(ImGuiKey_Space))collapseSelection(graph,index);
                if(ImGui::IsKeyPressed(ImGuiKey_Home))editor.fit[index]=editor.autoFit[index]=true;
                if(ImGui::IsKeyPressed(ImGuiKey_Enter)&&!editor.keyboardMove)openProperties(index);
                const float step=GridSize*(io.KeyShift?4:1);
                if(ImGui::IsKeyPressed(ImGuiKey_LeftArrow))nudgeSelection(graph,index,-step,0);
                if(ImGui::IsKeyPressed(ImGuiKey_RightArrow))nudgeSelection(graph,index,step,0);
                if(ImGui::IsKeyPressed(ImGuiKey_UpArrow))nudgeSelection(graph,index,0,-step);
                if(ImGui::IsKeyPressed(ImGuiKey_DownArrow))nudgeSelection(graph,index,0,step);
            }
            if(ImGui::IsKeyPressed(ImGuiKey_X)&&!editor.keyboardMove)deleteSelection(graph,index);
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Delete)&&!editor.keyboardMove&&!editor.portFocus[index].node)deleteSelection(graph,index);
    }
    canvas(graph,index);
    if(editor.showShortcuts){editor.showShortcuts=false;ImGui::OpenPopup("graph_shortcuts");}
    shortcutsPopup();
    ImGui::PopID();
}
}

void NodeEditor_Load()
{
    if(editor.resizeNode)finishNodeResize(false);
    if(editor.bendGraph>=0)finishWireBend(false);
    if(editor.pointerMove)finishPointerMove(false);
    std::error_code filesystemError;
    const bool exists=std::filesystem::exists(GraphFile,filesystemError);
    std::string error;
    if (!NodeGraph::service().load(GraphFile,error))
    {
        const std::string failure=error;
        Workspace empty;for(size_t i=0;i<GraphCount;++i)empty.graphs[i].kind=static_cast<GraphKind>(i);
        NodeGraph::service().publish(empty,error);
        editor.initialized=false;editor.loadFailed=true;editor.dirty=false;editor.error=true;
        editor.loadError=failure+" Import a valid file or restore a starter graph to resume autosave.";editor.message=editor.loadError;
        return;
    }
    editor.loadFailed=false;
    editor.loadError.clear();editor.initialized=false;
    editor.dirty=false;editor.error=false;editor.saveFailed=false;editor.message.clear();
    auto workspace=NodeGraph::service().workspace();
    const bool legacy=workspace.sourceVersion<NodeGraph::CurrentFormatVersion;
    const bool legacyLayout=workspace.sourceVersion<10;
    NodeGraph::migrateLegacySettings(workspace,config,!exists);
    bool normalized=NodeGraph::initializeMissingModels(workspace,config.ai_model,
#ifdef USE_CUDA
        "TRT"
#else
        "DML"
#endif
    );
    for(auto& graph:workspace.graphs)normalized|=normalizeLayout(graph,legacyLayout);
    if(!NodeGraph::service().publish(workspace,error)) {editor.message=error;editor.error=true;return;}
    if(legacy||!exists||normalized)
    {
        editor.dirty=true;
        editor.editedAt=std::chrono::steady_clock::now();
    }
}
void NodeEditor_SetPreview(bool preview) {editor.preview=preview;}
bool NodeEditor_ToolbarContainsPoint(float x,float y)
{
    for(std::size_t i=0;i<editor.toolbarItemCount;++i)
    {
        const auto& item=editor.toolbarItems[i];
        if(x>=item.x&&x<item.z&&y>=item.y&&y<item.w)return true;
    }
    return false;
}
void NodeEditor_PrepareFrame()
{
    if(!ImGui::GetCurrentContext())return;
    auto& flags=ImGui::GetIO().ConfigFlags;
    const bool popup=ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    if(popup&&!editor.popupNavigation)
    {
        editor.restoreKeyboardNavigation=(flags&ImGuiConfigFlags_NavEnableKeyboard)!=0;
        flags|=ImGuiConfigFlags_NavEnableKeyboard;editor.popupNavigation=true;
    }
    else if(!popup&&editor.popupNavigation)
    {
        if(!editor.restoreKeyboardNavigation)flags&=~ImGuiConfigFlags_NavEnableKeyboard;
        editor.popupNavigation=false;editor.popupUsesKeyboard=false;
    }
}
void NodeEditor_PreserveViewport()
{
    for(std::size_t graph=0;graph<GraphCount;++graph)
        if(editor.canvasSize[graph].x>0&&editor.canvasSize[graph].y>0)editor.autoFit[graph]=false;
}
void NodeEditor_EndInteraction(bool preserveViewport)
{
    service().setPreviewNodes({});NodePreview::clear();
    if(editor.resizeNode)finishNodeResize(false);
    if(editor.bendGraph>=0)finishWireBend(false);
    if(editor.pointerMove)finishPointerMove(false);
    if(editor.keyboardMove&&editor.activeGraph>=0&&editor.activeGraph<static_cast<int>(GraphCount))finishKeyboardMove(editor.activeGraph,false);
    cancelGestures();
    for(std::size_t graph=0;graph<GraphCount;++graph)invalidateActivity(graph);
    if(preserveViewport)NodeEditor_PreserveViewport();
    editor.drawnSignalPulses=0;editor.telemetryAt=-1;
}
void NodeEditor_Flush()
{
    if(!editor.dirty||editor.loadFailed||editor.keyboardMove||editor.pointerMove||editor.resizeNode||editor.bendGraph>=0)return;
    std::string error;
    editor.saveAttemptAt=std::chrono::steady_clock::now();
    if(service().save(GraphFile,error)) {editor.dirty=false;if(editor.saveFailed){editor.error=false;editor.message.clear();}editor.saveFailed=false;}
    else{editor.error=true;editor.saveFailed=true;editor.message="Could not save graphs: "+error;}
}
void NodeEditor_RunFileActions()
{
    const auto now=std::chrono::steady_clock::now();
    if(editor.dirty&&now-editor.editedAt>=std::chrono::milliseconds(350)&&(!editor.saveFailed||now-editor.saveAttemptAt>=std::chrono::seconds(2)))NodeEditor_Flush();
    const int action=editor.fileAction;
    if (!action) return;
    editor.fileAction=0;
    wchar_t filename[32768]=L"node_graphs.sngraph";
    if(action==3)filename[0]=L'\0';
    OPENFILENAMEW dialog{};
    dialog.lStructSize=sizeof(dialog);
    dialog.hwndOwner=g_hwnd;
    dialog.lpstrFile=filename;
    dialog.nMaxFile=static_cast<DWORD>(std::size(filename));
    const auto* fileNode=action==3?findNode(editor.draft.graphs[editor.fileGraph],editor.fileNode):nullptr;
    const bool depthFile=fileNode&&fileNode->kind==NodeKind::DepthInference&&editor.fileParameter=="model_path";
    dialog.lpstrFilter=action==3?(depthFile?L"Depth models (*.onnx;*.engine;*.trt;*.plan)\0*.onnx;*.engine;*.trt;*.plan\0All files\0*.*\0\0":editor.fileParameter=="model_path"?
        L"Model files (*.onnx;*.engine)\0*.onnx;*.engine\0All files\0*.*\0\0":
        L"Image files\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0\0"):
        L"SUNONE graphs (*.sngraph)\0*.sngraph\0Node graph files (*.ini)\0*.ini\0All files\0*.*\0\0";
    dialog.lpstrDefExt=action==3?nullptr:L"sngraph";
    dialog.Flags=OFN_NOCHANGEDIR|OFN_EXPLORER|OFN_PATHMUSTEXIST|(action==2?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    const bool chosen=action==2?GetSaveFileNameW(&dialog)!=FALSE:GetOpenFileNameW(&dialog)!=FALSE;
    if (!chosen)
    {
        if (CommDlgExtendedError()) { editor.message="Could not open the file picker."; editor.error=true; }
        return;
    }
    const std::filesystem::path path(filename);
    std::string data,error;
    if(action==3)
    {
        auto& graph=editor.draft.graphs[editor.fileGraph];auto* node=findNode(graph,editor.fileNode);
        if(!node){editor.message="The selected node no longer exists.";editor.error=true;return;}
        const auto& specs=definition(node->kind).textParameters;
        const auto spec=std::find_if(specs.begin(),specs.end(),[&](const TextParameterSpec& item){return item.key==editor.fileParameter;});
        const auto text=path.u8string();
        if(spec==specs.end()||text.size()>spec->maxLength){editor.message="This file path is too long for the node.";editor.error=true;return;}
        remember();node->textParameters[editor.fileParameter]=text;edited();return;
    }
    if (action==1)
    {
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if (!file || file.tellg()<0 || file.tellg()>static_cast<std::streamoff>(MaxWorkspaceBytes))
        { editor.message="Cannot read this graph file (maximum 4 MiB)."; editor.error=true; return; }
        data.resize(static_cast<size_t>(file.tellg())); file.seekg(0);
        if (!data.empty() && !file.read(data.data(),static_cast<std::streamsize>(data.size())))
        { editor.message="Could not read the graph file."; editor.error=true; return; }
        Workspace imported;
        if (!deserialize(data,imported,error)) { editor.message=error; editor.error=true; return; }
        const bool legacy=imported.sourceVersion<4;
        const bool legacyLayout=imported.sourceVersion<10;
        migrateLegacySettings(imported,config);
        for(auto& graph:imported.graphs)normalizeLayout(graph,legacyLayout);
        if(!validate(imported,error)){editor.message=error;editor.error=true;return;}
        editor.loadFailed=false;
        remember(); editor.draft=std::move(imported); edited();
        for (int i=0;i<static_cast<int>(GraphCount);++i) {editor.fit[i]=editor.autoFit[i]=true;clearSelection(i);editor.selectedGroup[i]=0;}
        cancelGestures();
        if(!editor.error)editor.message=legacy?"Graphs imported. Legacy parameters were copied from the current configuration.":"Graphs imported.";
    }
    else
    {
        if (!serialize(editor.draft,data,error)) { editor.message=error; editor.error=true; return; }
        std::filesystem::path temporary;
        HANDLE file=INVALID_HANDLE_VALUE;
        for (int attempt=0;attempt<32;++attempt)
        {
            temporary=path;
            temporary+=L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L"."+std::to_wstring(attempt);
            file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if (file!=INVALID_HANDLE_VALUE || GetLastError()!=ERROR_FILE_EXISTS) break;
        }
        if (file==INVALID_HANDLE_VALUE)
        { editor.message="Could not create the export file."; editor.error=true; return; }
        DWORD written=0;
        bool saved=WriteFile(file,data.data(),static_cast<DWORD>(data.size()),&written,nullptr)!=FALSE && written==data.size();
        saved=saved && FlushFileBuffers(file)!=FALSE;
        saved=CloseHandle(file)!=FALSE && saved;
        if (!saved || !MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temporary.c_str());
            editor.message="Could not finish exporting graphs."; editor.error=true; return;
        }
        editor.message="All graphs exported."; editor.error=false;
    }
}
void draw_node_aim() { drawEditor(0); }
void draw_node_auto_shoot() { drawEditor(1); }
void draw_node_recoil() { drawEditor(2); }
void draw_node_vision() { drawEditor(3); }
