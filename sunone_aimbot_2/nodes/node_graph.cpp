#include "nodes/node_graph.h"
#include <set>
#include "nodes/node_layout.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dxgi1_2.h>
#pragma comment(lib,"dxgi.lib")
#endif

namespace NodeGraph
{
namespace
{
using K = NodeKind;
using T = ValueType;
// Small scalar keys and stream identities only. No image or detection-buffer
// traversal is needed to know whether a node produced new data.
std::uint64_t signalStamp(const Value& value)
{
    std::uint64_t stamp = static_cast<std::uint64_t>(value.type) + 1;
    const auto mix = [&](std::uint64_t part) { stamp ^= part + 0x9e3779b97f4a7c15ull + (stamp << 6) + (stamp >> 2); };
    const auto scalar = [&](double number) { std::uint64_t bits; std::memcpy(&bits, &number, sizeof(bits)); mix(bits); };
    switch (value.type)
    {
    case T::Boolean: mix(value.boolean); break;
    case T::Number: scalar(value.number); break;
    case T::Integer: mix(static_cast<std::uint32_t>(value.integer)); break;
    case T::String: for(const unsigned char c:value.text)mix(c);break;
    case T::OverlayStyle:case T::LabelOptions:mix(reinterpret_cast<std::uintptr_t>(value.settings.get()));break;
    case T::Monitor: case T::Monitors:
    {
        const auto monitor=[&](const MonitorInfo& m){mix(m.index);mix(m.winrtIndex);mix(m.x);mix(m.y);mix(m.width);mix(m.height);mix(m.primary);for(const unsigned char c:m.device)mix(c);};
        if(value.monitor)monitor(*value.monitor);if(value.monitors)for(const auto& m:*value.monitors)monitor(m);break;
    }
    case T::Point: scalar(value.point.x); scalar(value.point.y); break;
    case T::Motion:
        scalar(value.motion.position.x);scalar(value.motion.position.y);scalar(value.motion.velocity.x);scalar(value.motion.velocity.y);
        scalar(value.motion.damping);mix(value.motion.ready);mix(value.motion.futureReady);break;
    case T::Calibration:mix(reinterpret_cast<std::uintptr_t>(value.calibration.get()));break;
    case T::Target:
        mix(value.target.valid); mix(value.target.observed); mix(value.target.fresh); mix(value.target.trackId); mix(value.target.missedFrames);
        mix(value.target.classId); mix(value.target.pointPrepared); scalar(value.target.x); scalar(value.target.y); scalar(value.target.ageSeconds);
        scalar(value.target.point.x); scalar(value.target.point.y); scalar(value.target.width); scalar(value.target.height);
        scalar(value.target.velocityX); scalar(value.target.velocityY); scalar(value.target.confidence); break;
    case T::Movement:
        mix(value.movement.steps.size()); scalar(value.movement.preciseCounts.x); scalar(value.movement.preciseCounts.y);
        mix(value.movement.hasPreciseCounts); scalar(value.movement.pixelScaleX); scalar(value.movement.pixelScaleY);
        for (const auto& step : value.movement.steps) { mix(step.dx); mix(step.dy); }
        break;
    case T::Detections:
        if (value.detections)
        { mix(value.detections->version); mix(value.detections->generation); scalar(value.detections->timestampSeconds); mix(value.detections->objects.size()); }
        break;
    case T::Frame: case T::Depth:
        if (value.frame) { mix(value.frame->version); mix(value.frame->generation); scalar(value.frame->timestampSeconds); mix(value.frame->width); mix(value.frame->height); mix(value.frame->gpu); }
        break;
    case T::Mask:
        if (value.mask) { mix(value.mask->version); mix(value.mask->generation); scalar(value.mask->timestampSeconds); mix(value.mask->width); mix(value.mask->height); }
        break;
    }
    if(value.preview)mix(reinterpret_cast<std::uintptr_t>(value.preview.get()));
    return stamp;
}
bool imageValue(T type){return type==T::Frame||type==T::Depth||type==T::Mask;}
Preview textPreview(const Value& value)
{
    auto result=std::make_shared<PreviewImage>();result->sourceType=value.type;
    std::ostringstream text;text<<std::setprecision(17);
    const auto point=[&](const char* label,Point p){text<<label<<"x = "<<p.x<<"\ny = "<<p.y<<'\n';};
    switch(value.type)
    {
    case T::String:result->text=value.text;return result;
    case T::Boolean:text<<(value.boolean?"true":"false");break;
    case T::Float:text<<value.number;break;
    case T::Integer:text<<value.integer;break;
    case T::Point:point("",value.point);break;
    case T::Target:
        if(!value.target.valid){text<<"No target";break;}
        text<<"Class = "<<value.target.classId<<"\nTrack = "<<value.target.trackId<<"\nConfidence = "<<value.target.confidence<<'\n';
        point("Position\n",{value.target.x,value.target.y});point("Size\n",{value.target.width,value.target.height});point("Aim point\n",value.target.point);
        point("Velocity\n",{value.target.velocityX,value.target.velocityY});text<<"Observed = "<<(value.target.observed?"true":"false")<<"\nAge = "<<value.target.ageSeconds;break;
    case T::Detections:
        if(!value.detections){text<<"No detections";break;}
        text<<value.detections->objects.size()<<" detections\nFrame = "<<value.detections->version<<'\n';
        for(std::size_t i=0;i<std::min<std::size_t>(64,value.detections->objects.size());++i)
        {const auto& d=value.detections->objects[i];text<<'['<<i<<"] class "<<d.classId<<", confidence "<<d.confidence<<"\n  "<<d.x<<", "<<d.y<<"   "<<d.width<<" x "<<d.height<<'\n';}
        if(value.detections->objects.size()>64)text<<"... "<<value.detections->objects.size()-64<<" more detections";break;
    case T::Movement:
        text<<value.movement.steps.size()<<" movement steps\n";
        for(std::size_t i=0;i<std::min<std::size_t>(64,value.movement.steps.size());++i)text<<'['<<i<<"] "<<value.movement.steps[i].dx<<", "<<value.movement.steps[i].dy<<'\n';
        if(value.movement.steps.size()>64)text<<"... "<<value.movement.steps.size()-64<<" more steps\n";
        if(value.movement.hasPreciseCounts)point("Precise counts\n",value.movement.preciseCounts);break;
    case T::Motion:point("Position\n",value.motion.position);point("Velocity\n",value.motion.velocity);text<<"Ready = "<<(value.motion.ready?"true":"false");break;
    case T::Monitor:case T::Monitors:
    {
        const auto monitor=[&](const MonitorInfo& m){text<<'['<<m.index<<"] "<<m.name<<(m.primary?" (primary)":"")<<'\n'<<m.device<<'\n'<<m.width<<" x "<<m.height<<" at "<<m.x<<", "<<m.y<<"\n\n";};
        if(value.monitor)monitor(*value.monitor);if(value.monitors)for(const auto& m:*value.monitors)monitor(m);break;
    }
    case T::Calibration:
        if(value.calibration){text<<value.calibration->profileName<<'\n';point("Degrees per pixel\n",value.calibration->degreesPerPixel);point("Counts per degree\n",value.calibration->countsPerDegree);point("Pixels per count\n",value.calibration->pixelsPerCount);}break;
    case T::OverlayStyle:case T::LabelOptions:
        if(value.settings){for(const auto& entry:value.settings->numbers)text<<entry.first<<" = "<<entry.second<<'\n';for(const auto& entry:value.settings->texts)text<<entry.first<<" = "<<entry.second<<'\n';}break;
    default:return {};
    }
    result->text=text.str();return result;
}
bool signalIsActive(const Value& value)
{
    if (!value.valid || !value.error.empty()) return false;
    switch (value.type)
    {
    case T::Boolean: return value.boolean;
    case T::Target: return value.target.valid;
    case T::Detections: return static_cast<bool>(value.detections);
    case T::Frame: case T::Depth: return static_cast<bool>(value.frame);
    case T::Mask: return static_cast<bool>(value.mask);
    case T::Monitor:return static_cast<bool>(value.monitor);
    case T::Monitors:return static_cast<bool>(value.monitors);
    case T::OverlayStyle:case T::LabelOptions:return static_cast<bool>(value.settings);
    case T::Calibration:return static_cast<bool>(value.calibration);
    default: return true;
    }
}
Value boolean(bool v) { Value r; r.boolean = v; return r; }
Value number(double v) { Value r; r.type = T::Number; r.number = v; r.valid = std::isfinite(v); return r; }
Value integer(std::int64_t v)
{
    Value r;r.type=T::Integer;r.valid=v>=MinInteger&&v<=MaxInteger;
    if(r.valid){r.integer=static_cast<std::int32_t>(v);r.number=static_cast<double>(r.integer);}
    else r.error="Int overflow (signed 32-bit range).";
    return r;
}
Value stringValue(std::string text)
{
    Value value;value.type=T::String;value.valid=text.size()<=4096&&text.find('\0')==std::string::npos;
    if(value.valid)value.text=std::move(text);else value.error="Text exceeds 4096 characters or contains a null byte.";return value;
}
Value scalarInputDefault(const Node& node,std::size_t pin,bool required=false)
{
    const auto& input=definition(node.kind).inputs[pin];
    if(input.type==T::String)
    {
        if(input.defaultParameter)return stringValue(textParameter(node,input.defaultParameter));
        const auto found=node.inputTexts.find(static_cast<int>(pin));
        if(found!=node.inputTexts.end())return stringValue(found->second);
        if(!required)return stringValue({});
    }
    else if(input.type==T::Boolean||input.type==T::Float||input.type==T::Integer)
    {
        double value=0;const auto found=node.inputValues.find(static_cast<int>(pin));
        const bool configured=input.defaultParameter||found!=node.inputValues.end();
        if(configured||!required)
        {
            if(input.defaultParameter)value=parameter(node,input.defaultParameter);else if(found!=node.inputValues.end())value=found->second;
            return input.type==T::Boolean?boolean(value!=0):input.type==T::Integer?integer(static_cast<std::int64_t>(value)):number(value);
        }
    }
    Value unavailable;unavailable.type=input.type;unavailable.valid=false;return unavailable;
}
Value monitorValue(Monitor monitor)
{
    Value value;value.type=T::Monitor;value.monitor=std::move(monitor);value.valid=value.monitor&&value.monitor->width>0&&value.monitor->height>0;return value;
}
Value monitorsValue(MonitorList monitors)
{
    Value value;value.type=T::Monitors;value.monitors=std::move(monitors);value.valid=value.monitors&&value.monitors->size()<=256;return value;
}
void promoteInteger(Value& value,ValueType input)
{
    if(value.type==T::Integer&&input==T::Float){value.number=static_cast<double>(value.integer);value.type=T::Float;}
}
Value point(Point p) { Value r; r.type = T::Point; r.point = p; r.valid = std::isfinite(p.x) && std::isfinite(p.y); return r; }
Value motionValue(MotionData motion)
{
    Value value;value.type=T::Motion;value.motion=motion;
    value.valid=point(motion.position).valid&&point(motion.velocity).valid&&std::isfinite(motion.damping)&&motion.damping>=0&&motion.damping<=3;
    return value;
}
Value target(TargetData t)
{
    Value r; r.type = T::Target; r.target = t;
    r.valid = !t.valid || (std::isfinite(t.point.x) && std::isfinite(t.point.y) &&
        std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.width) && std::isfinite(t.height) &&
        t.width >= 0.0 && t.height >= 0.0 && std::isfinite(t.confidence) && t.confidence>=0 && t.confidence<=1 &&
        std::isfinite(t.velocityX) && std::isfinite(t.velocityY) && std::isfinite(t.ageSeconds) && t.ageSeconds>=0 && t.missedFrames>=0);
    return r;
}
Value movement(Movement m)
{
    Value r; r.type = T::Movement;
    r.valid = m.steps.size() <= 32 && std::isfinite(m.pixelScaleX) && std::isfinite(m.pixelScaleY) &&
        std::abs(m.pixelScaleX) <= 1000000 && std::abs(m.pixelScaleY) <= 1000000 &&
        (!m.hasPreciseCounts || (std::isfinite(m.preciseCounts.x)&&std::isfinite(m.preciseCounts.y)));
    for (const MoveStep& step : m.steps)
        r.valid = r.valid && step.dx >= -32767 && step.dx <= 32767 && step.dy >= -32767 && step.dy <= 32767;
    if (r.valid) r.movement = std::move(m);
    return r;
}
bool validStream(const DetectionStream& stream)
{
    if(!stream || stream->objects.size()>100000 || !std::isfinite(stream->timestampSeconds) ||
        !std::isfinite(stream->width) || !std::isfinite(stream->height)||stream->width<0||stream->height<0)return false;
    for(const auto& d:stream->objects)
        if(!std::isfinite(d.x)||!std::isfinite(d.y)||!std::isfinite(d.width)||!std::isfinite(d.height)||
            !std::isfinite(d.confidence)||d.width<=0||d.height<=0||d.confidence<0||d.confidence>1||d.classId < -1)return false;
    return true;
}
Value streamValue(DetectionStream stream)
{
    Value value;value.type=T::Detections;value.valid=validStream(stream);value.detections=std::move(stream);
    value.boolean=value.valid;return value;
}
Value frameValue(FrameStream frame,ValueType type=T::Frame)
{
    Value value;value.type=type;value.frame=std::move(frame);
    value.valid=value.frame&&value.frame->storage&&value.frame->width>0&&value.frame->height>0&&
        std::isfinite(value.frame->timestampSeconds)&&value.frame->error.empty();
    if(value.frame)value.error=value.frame->error;return value;
}
Value maskValue(MaskStream mask)
{
    Value value;value.type=T::Mask;value.mask=std::move(mask);
    value.valid=value.mask&&value.mask->storage&&value.mask->width>0&&value.mask->height>0&&
        std::isfinite(value.mask->timestampSeconds)&&value.mask->error.empty();
    if(value.mask)value.error=value.mask->error;return value;
}
template<class Predicate> DetectionStream filterStream(const DetectionStream& input,Predicate keep)
{
    auto output=std::make_shared<DetectionFrame>(*input);
    output->objects.erase(std::remove_if(output->objects.begin(),output->objects.end(),[&](const Detection& d){return !keep(d);}),output->objects.end());
    return output;
}
bool isStyleNode(K k){return k>=K::OverlayBoxes&&k<=K::OverlayCorrection;}
bool isSink(K k) { return k == K::AimOutput || k == K::AutoShootOutput || k == K::RecoilOutput || k==K::RecoilProfileOutput || k==K::SharedRecoilOutput ||
    k==K::VisionOutput || k==K::CollectDataset || k==K::RenderOverlay||k==K::ScreenshotOutput||k==K::ImagePreview||k==K::OverlayOutput||k==K::DatasetOutput; }
bool isAuxiliarySink(K k){return k==K::CollectDataset||k==K::RenderOverlay||k==K::ScreenshotOutput||k==K::ImagePreview||k==K::OverlayOutput||k==K::DatasetOutput;}
bool isVisionOnly(K k){return k==K::CurrentFrame||k==K::CaptureSource||k==K::Inference||k==K::DepthInference||k==K::DepthToMask||
    k==K::ApplyMask||k==K::FilterMask||k==K::CollectDataset||k==K::RenderOverlay||k==K::VisionOutput||
    k==K::RerouteFrame||k==K::RerouteDepth||k==K::RerouteMask||k==K::ScreenshotOutput||
    (k>=K::OverlayBoxes&&k<=K::SetLabels);}
bool finiteBound(double v, double bound) { return std::isfinite(v) && std::abs(v) <= bound; }
struct CompiledNode
{
    Node node;
    std::array<int, MaxPins> inputs;
    bool active = false;
    bool potentiallyActive = false;
    int variableSource=-1;
    int sharedGraph=-1,sharedIndex=-1;
    std::vector<std::size_t> bindings;
    CompiledNode() { inputs.fill(-1); }
};
struct CompiledGraph { std::vector<CompiledNode> nodes; };
struct ClearInputStorage
{
    std::array<Value,MaxDataPins>& values;
    explicit ClearInputStorage(std::array<Value,MaxDataPins>& storage):values(storage){}
    ~ClearInputStorage(){for(auto& value:values)value=Value{};}
};

bool compileGraph(const Graph& graph, CompiledGraph& compiled, std::string& error)
{
    if (graph.nodes.size() > MaxNodes || graph.links.size() > MaxLinks || graph.groups.size() > MaxGroups || graph.ties.size()>MaxWireTies)
    { error = "Graph exceeds the 128 node / 256 link / 32 group / 64 wire tie limit."; return false; }
    if (!finiteBound(graph.panX, 1000000) || !finiteBound(graph.panY, 1000000) ||
        !std::isfinite(graph.zoom) || graph.zoom < 0.2f || graph.zoom > 2.5f)
    { error = "Invalid canvas viewport."; return false; }
    std::unordered_map<int, int> indices;
    std::array<int,MaxPins> emptyInputs; emptyInputs.fill(-1);
    std::vector<std::array<int, MaxPins>> incoming(graph.nodes.size(), emptyInputs);
    std::vector<int> indegree(graph.nodes.size(), 0);
    std::vector<std::vector<int>> outgoing(graph.nodes.size());
    std::unordered_map<std::string,int> variables;
    std::vector<int> variableSources(graph.nodes.size(),-1);
    std::set<K> sinks;
    for (std::size_t i = 0; i < graph.nodes.size(); ++i)
    {
        const Node& n = graph.nodes[i];
        if (n.id <= 0 || n.id > 1000000 || !indices.emplace(n.id, static_cast<int>(i)).second)
        { error = "Node IDs must be positive and unique."; return false; }
        if (static_cast<int>(n.kind) < 0 || n.kind >= K::Count)
        { error = "Unknown node type."; return false; }
        if(graph.kind!=GraphKind::Vision&&isVisionOnly(n.kind))
        {error="Frame providers and visual outputs belong to the Vision graph.";return false;}
        if (n.bypassed && !canBypass(n.kind))
        { error = "This node cannot be bypassed because its input and output types differ."; return false; }
        if (!finiteBound(n.x, 1000000) || !finiteBound(n.y, 1000000) ||
            !finiteBound(n.value,n.kind==K::Integer?2147483648.0:1000000000.0))
        { error = "Node contains an invalid position or constant."; return false; }
        if(n.kind==K::Integer&&(n.value<MinInteger||n.value>MaxInteger||std::trunc(n.value)!=n.value))
        {error="Int constants must be whole numbers in the signed 32-bit range.";return false;}
        if(!std::isfinite(n.width)||!std::isfinite(n.height)||
            (n.width!=0&&(n.width<Layout::MinWidth||n.width>Layout::MaxWidth))||
            (n.height!=0&&(n.height<Layout::minimumHeight(n)||n.height>Layout::MaxHeight))||
            (Layout::isReroute(n.kind)&&(n.width!=0||n.height!=0)))
        {error="Node dimensions are outside the supported limits.";return false;}
        const auto& spec = definition(n.kind);
        if (spec.inputs.size() > MaxPins || spec.dataInputs>MaxDataPins || n.parameters.size() > 64 || n.textParameters.size() > 16)
        { error = "Node parameter or pin limit exceeded."; return false; }
        if(variableGet(n.kind)||variableSet(n.kind)||calibrationIdentifier(n.kind))
        {
            const auto name=textParameter(n,"name");
            if(name.empty()){error="Named values require a nonempty name.";return false;}
            if(variableSet(n.kind)&&!variables.emplace(name,static_cast<int>(i)).second)
            {error="Each named value can have only one Set in a graph: "+name;return false;}
        }
        for (const auto& entry : n.parameters)
        {
            const auto found = std::find_if(spec.parameters.begin(),spec.parameters.end(),[&](const ParameterSpec& p) {return entry.first==p.key;});
            if (found==spec.parameters.end() || !std::isfinite(entry.second) || entry.second<found->minValue || entry.second>found->maxValue ||
                (found->kind!=ParameterKind::Number && std::floor(entry.second)!=entry.second))
            { error="Invalid parameter in " + std::string(spec.title) + ": " + entry.first; return false; }
        }
        for (const auto& entry : n.textParameters)
        {
            const auto found=std::find_if(spec.textParameters.begin(),spec.textParameters.end(),[&](const TextParameterSpec& p) {return entry.first==p.key;});
            if(found==spec.textParameters.end() || entry.second.size()>found->maxLength || entry.second.find('\0')!=std::string::npos)
            {error="Invalid text parameter in "+std::string(spec.title);return false;}
        }
        if(n.inputValues.size()>MaxDataPins||n.inputTexts.size()>MaxDataPins)
        {error="Too many literal input defaults.";return false;}
        for(const auto& entry:n.inputValues)
        {
            if(entry.first<0||static_cast<std::size_t>(entry.first)>=spec.dataInputs||spec.inputs[entry.first].defaultParameter||!std::isfinite(entry.second))
            {error="Invalid numeric input default.";return false;}
            const auto type=spec.inputs[entry.first].type;
            if((type!=T::Boolean&&type!=T::Float&&type!=T::Integer)||
                (type==T::Boolean&&entry.second!=0&&entry.second!=1)||
                (type==T::Integer&&(entry.second<MinInteger||entry.second>MaxInteger||std::trunc(entry.second)!=entry.second)))
            {error="Literal input default has the wrong type or range.";return false;}
        }
        for(const auto& entry:n.inputTexts)
            if(entry.first<0||static_cast<std::size_t>(entry.first)>=spec.dataInputs||spec.inputs[entry.first].defaultParameter||spec.inputs[entry.first].type!=T::String||entry.second.size()>4096||entry.second.find('\0')!=std::string::npos)
            {error="Invalid text input default.";return false;}
        if (isSink(n.kind))
        {
            if (n.kind!=K::ImagePreview&&(graph.kind==GraphKind::Vision)!=(n.kind==K::VisionOutput||isAuxiliarySink(n.kind)))
            { error="Output node belongs to another execution domain.";return false; }
            if(!isAuxiliarySink(n.kind)&&!sinks.insert(n.kind).second)
            {error="Each graph can have at most one output of each type.";return false;}
        }
    }
    std::unordered_set<int> groupIds, groupedNodes;
    for (const Group& group : graph.groups)
    {
        if (group.id <= 0 || group.id > 1000000 || !groupIds.insert(group.id).second)
        { error = "Group IDs must be positive and unique."; return false; }
        if (group.title.size() > 128 || !finiteBound(group.x,1000000) || !finiteBound(group.y,1000000) ||
            !finiteBound(group.width,1000000) || !finiteBound(group.height,1000000) || group.width < 32 || group.height < 32)
        { error = "Group title or dimensions are invalid."; return false; }
        for (int member : group.nodes)
            if (indices.find(member) == indices.end() || !groupedNodes.insert(member).second)
            { error = "Group members must exist and belong to only one group."; return false; }
    }
    std::unordered_set<int> tieIds;
    for(const auto& tie:graph.ties)
        if(tie.id<=0||tie.id>1000000||!tieIds.insert(tie.id).second||!finiteBound(tie.x,1000000)||!finiteBound(tie.y,1000000)||!finiteBound(tie.angle,360))
        {error="Wire tie IDs and positions must be valid and unique.";return false;}
    for (const Link& link : graph.links)
    {
        if(!finiteBound(link.bend.roundness,1)||link.bend.roundness<0||!finiteBound(link.bend.twist,1))
        {error="Wire roundness must be between 0 and 1, and twist between -1 and 1.";return false;}
        if(link.ties.size()>MaxTiesPerLink){error="A wire can pass through at most 16 ties.";return false;}
        std::unordered_set<int> usedTies;
        for(const int tie:link.ties)if(!tieIds.count(tie)||!usedTies.insert(tie).second)
        {error="A wire refers to a missing or repeated tie.";return false;}
        const auto from = indices.find(link.fromNode), to = indices.find(link.toNode);
        if (from == indices.end() || to == indices.end())
        { error = "A link refers to a missing node."; return false; }
        const auto& a = definition(graph.nodes[from->second].kind);
        const auto& b = definition(graph.nodes[to->second].kind);
        if (link.fromPort < 0 || link.toPort < 0 ||
            static_cast<std::size_t>(link.fromPort) >= a.outputs.size() ||
            static_cast<std::size_t>(link.toPort) >= b.inputs.size())
        { error = "A link refers to a missing port."; return false; }
        if (!canConnectTypes(a.outputs[link.fromPort].type,b.inputs[link.toPort].type))
        { error = "Cannot connect " + std::string(typeName(a.outputs[link.fromPort].type)) + " to " + typeName(b.inputs[link.toPort].type) + ". Use a conversion node for Float to Int."; return false; }
        if (incoming[to->second][link.toPort] != -1)
        { error = "An input can have only one connection."; return false; }
        incoming[to->second][link.toPort] = from->second;
        ++indegree[to->second];
        outgoing[from->second].push_back(to->second);
    }
    for(std::size_t i=0;i<graph.nodes.size();++i)if(variableGet(graph.nodes[i].kind))
    {
        const auto writer=variables.find(textParameter(graph.nodes[i],"name"));
        if(writer!=variables.end()&&definition(graph.nodes[writer->second].kind).outputs[0].type==definition(graph.nodes[i].kind).outputs[0].type)
        {variableSources[i]=writer->second;outgoing[writer->second].push_back(static_cast<int>(i));++indegree[i];}
    }
    for(std::size_t i=0;i<graph.nodes.size();++i)if(graph.nodes[i].kind==K::CaptureSource&&incoming[i][0]>=0)
        for(std::size_t p=1;p<definition(K::CaptureSource).inputs.size();++p)
        {
            const auto* key=definition(K::CaptureSource).inputs[p].defaultParameter;
            if(incoming[i][p]>=0&&key&&(std::strcmp(key,"monitor_idx")==0||std::strcmp(key,"monitor_device")==0||std::strcmp(key,"capture_target")==0))
            {error="Connect either a Monitor or its index/device override, not both.";return false;}
        }
    std::vector<int> ready;
    for (std::size_t i = 0; i < indegree.size(); ++i)
        if (!indegree[i]) ready.push_back(static_cast<int>(i));
    std::vector<int> order;
    for (std::size_t cursor = 0; cursor < ready.size(); ++cursor)
    {
        const int index = ready[cursor];
        order.push_back(index);
        for (int next : outgoing[index])
            if (--indegree[next] == 0) ready.push_back(next);
    }
    if (order.size() != graph.nodes.size()) { error = "Connections cannot contain a cycle."; return false; }
    std::vector<int> remap(graph.nodes.size(), -1);
    for (std::size_t i = 0; i < order.size(); ++i) remap[order[i]] = static_cast<int>(i);
    // Keep unused nodes editable, but never execute side effects from branches
    // which cannot contribute to the output (especially disconnected prediction).
    std::vector<bool> active(graph.nodes.size(), false);
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) active[i] = isSink(graph.nodes[i].kind)&&graph.nodes[i].kind!=K::ImagePreview;
    const auto styleFlag=[&](auto&& self,int index,const char* key)->bool{
        if(index<0)return false;const auto& node=graph.nodes[index];if(node.disabled)return false;
        const auto& def=definition(node.kind);
        if(!node.bypassed)for(std::size_t p=0;p<def.inputs.size();++p)
            if(def.inputs[p].defaultParameter&&std::strcmp(key,def.inputs[p].defaultParameter)==0)
                return incoming[index][p]>=0||parameter(node,key)!=0;
        if(variableSources[index]>=0)return self(self,variableSources[index],key);
        return def.dataInputs&&def.inputs[0].type==T::OverlayStyle?self(self,incoming[index][0],key):false;
    };
    const auto propagate=[&](std::vector<bool>& active){
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (active[*it])
        {
            const auto& node=graph.nodes[*it];const auto& def=definition(node.kind);
            if(node.disabled)continue;
            if(variableSources[*it]>=0)active[variableSources[*it]]=true;
            if(node.bypassed&&isSink(node.kind))continue;
            if(node.kind==K::OverlayOutput&&incoming[*it][5]<0)continue;
            const bool enabledWired=std::any_of(def.inputs.begin(),def.inputs.end(),[&](const Pin& pin){const auto p=static_cast<std::size_t>(&pin-def.inputs.data());return pin.defaultParameter&&std::strcmp(pin.defaultParameter,"enabled")==0&&incoming[*it][p]>=0;});
            if((node.kind==K::CaptureSource||node.kind==K::Inference||node.kind==K::DepthInference||node.kind==K::DepthToMask)&&parameter(node,"enabled")==0&&!enabledWired)continue;
            if(isSink(node.kind)&&incoming[*it][0]<0&&def.inputs[0].defaultParameter&&parameter(node,def.inputs[0].defaultParameter)==0)continue;
            if(isSink(node.kind)&&incoming[*it][0]>=0)
            {
                const int triggerIndex=incoming[*it][0];const auto& trigger=graph.nodes[triggerIndex];
                const auto& triggerInputs=definition(trigger.kind).inputs;
                const bool keyWired=std::any_of(triggerInputs.begin(),triggerInputs.end(),[&](const Pin& pin){
                    return pin.defaultParameter&&std::strcmp(pin.defaultParameter,"virtual_key")==0&&incoming[triggerIndex][&pin-triggerInputs.data()]>=0;
                });
                if((trigger.kind==K::Boolean&&trigger.value==0)||
                    ((trigger.kind==K::KeyHeld||trigger.kind==K::KeyPressed||trigger.kind==K::KeyReleased)&&parameter(trigger,"virtual_key")==0&&!keyWired))continue;
            }
            for(std::size_t pin=0;pin<def.inputs.size();++pin)
            {
                if(node.kind==K::ImagePreview&&pin==4)
                {
                    bool dynamic=false;for(std::size_t p=0;p<def.inputs.size();++p)if(def.inputs[p].defaultParameter&&std::strcmp(def.inputs[p].defaultParameter,"mode")==0)dynamic=incoming[*it][p]>=0;
                    int mode=static_cast<int>(parameter(node,"mode"));if(!mode)for(int p=1;p<=3;++p)if(incoming[*it][p]>=0){mode=p;break;}
                    const int source=mode>=1&&mode<=3?incoming[*it][mode]:-1;
                    if(!dynamic&&(source<0||!imageValue(definition(graph.nodes[source].kind).outputs[0].type)))continue;
                }
                if(node.kind==K::ImagePreview&&pin>=1&&pin<=3)
                {
                    bool dynamic=false;for(std::size_t p=0;p<def.inputs.size();++p)if(def.inputs[p].defaultParameter&&std::strcmp(def.inputs[p].defaultParameter,"mode")==0)dynamic=incoming[*it][p]>=0;
                    int mode=static_cast<int>(parameter(node,"mode"));if(!mode)for(int p=1;p<=3;++p)if(incoming[*it][p]>=0){mode=p;break;}
                    if(!dynamic&&pin!=static_cast<std::size_t>(mode))continue;
                }
                if(node.bypassed&&def.outputs.size()==1)
                {const auto first=std::find_if(def.inputs.begin(),def.inputs.begin()+def.dataInputs,[&](const Pin& p){return p.type==def.outputs[0].type;});if(pin!=static_cast<size_t>(std::distance(def.inputs.begin(),first)))continue;}
                if((node.kind==K::ApplyMask||node.kind==K::FilterMask)&&parameter(node,"enabled")==0&&!enabledWired&&pin!=0)continue;
                if(node.kind==K::RenderOverlay&&pin<def.dataInputs&&((pin==3&&parameter(node,"depth_mask_enabled")==0)||(pin==4&&parameter(node,"depth_debug_overlay_enabled")==0)))
                {
                    const char* key=pin==3?"depth_mask_enabled":"depth_debug_overlay_enabled";
                    bool wired=false;for(std::size_t p=0;p<def.inputs.size();++p)wired|=def.inputs[p].defaultParameter&&std::strcmp(def.inputs[p].defaultParameter,key)==0&&incoming[*it][p]>=0;
                    if(!wired)continue;
                }
                if(node.kind==K::OverlayOutput&&(pin==3||pin==4)&&
                    !styleFlag(styleFlag,incoming[*it][5],pin==3?"depth_mask_enabled":"depth_debug_overlay_enabled"))continue;
                const int input=incoming[*it][pin];if(input>=0)active[input]=true;
            }
        }
    };
    propagate(active);auto potentiallyActive=active;
    for(std::size_t i=0;i<graph.nodes.size();++i)if(graph.nodes[i].kind==K::ImagePreview)potentiallyActive[i]=true;
    propagate(potentiallyActive);
    compiled.nodes.clear();
    for (int old : order)
    {
        CompiledNode n; n.node = graph.nodes[old]; n.active = active[old];n.potentiallyActive=potentiallyActive[old];
        if(variableSources[old]>=0)n.variableSource=remap[variableSources[old]];
        for (std::size_t p = 0; p < n.inputs.size(); ++p)
            if (incoming[old][p] >= 0)
            {
                n.inputs[p] = remap[incoming[old][p]];
                if(definition(n.node.kind).inputs[p].defaultParameter||(n.node.kind==K::CaptureSource&&p==0))n.bindings.push_back(p);
            }
        compiled.nodes.push_back(n);
    }
    if(graph.kind==GraphKind::Vision&&std::count_if(compiled.nodes.begin(),compiled.nodes.end(),[](const CompiledNode& node){
        if(!node.potentiallyActive||node.node.disabled||node.node.kind!=K::CaptureSource)return false;
        if(parameter(node.node,"enabled")!=0)return true;const auto& def=definition(node.node.kind);
        return std::any_of(node.bindings.begin(),node.bindings.end(),[&](std::size_t pin){return def.inputs[pin].defaultParameter&&std::strcmp(def.inputs[pin].defaultParameter,"enabled")==0;});})>1)
    {error="Only one reachable Capture Source is supported in a Vision graph.";return false;}
    return true;
}
bool providerMayBeEnabled(const CompiledNode& node)
{
    if(parameter(node.node,"enabled")!=0)return true;
    const auto& inputs=definition(node.node.kind).inputs;
    for(std::size_t pin=0;pin<inputs.size();++pin)
        if(inputs[pin].defaultParameter&&std::strcmp(inputs[pin].defaultParameter,"enabled")==0&&node.inputs[pin]>=0)return true;
    return isSink(node.node.kind)&&node.inputs[0]>=0;
}
bool sameNodeExecution(const Node& a,const Node& b)
{
    return a.kind==b.kind&&a.value==b.value&&a.bypassed==b.bypassed&&a.disabled==b.disabled&&
        a.parameters==b.parameters&&a.textParameters==b.textParameters&&a.inputValues==b.inputValues&&a.inputTexts==b.inputTexts;
}
bool eventNode(K kind){return kind==K::KeyHeld||kind==K::KeyPressed||kind==K::KeyReleased||kind==K::Toggle;}
bool sameExecution(const Workspace& a,const Workspace& b)
{
    if(a.graphs.size()!=b.graphs.size())return false;
    for (std::size_t g = 0; g < a.graphs.size(); ++g)
    {
        const Graph& first = a.graphs[g];
        const Graph& second = b.graphs[g];
        if (workflowId(first,g)!=workflowId(second,g)||first.kind != second.kind || first.active!=second.active || first.enabled!=second.enabled || first.nodes.size() != second.nodes.size() || first.links.size() != second.links.size()) return false;
        std::unordered_map<int,const Node*> nodes;
        for (const Node& node : first.nodes) nodes.emplace(node.id,&node);
        for (const Node& node : second.nodes)
        {
            const auto found = nodes.find(node.id);
            if (found == nodes.end()) return false;
            const Node& other = *found->second;
            if (!sameNodeExecution(node,other)) return false;
        }
        using Edge = std::tuple<int,int,int,int>;
        std::vector<Edge> left,right;
        for (const Link& link : first.links) left.emplace_back(link.fromNode,link.fromPort,link.toNode,link.toPort);
        for (const Link& link : second.links) right.emplace_back(link.fromNode,link.fromPort,link.toNode,link.toPort);
        std::sort(left.begin(),left.end()); std::sort(right.begin(),right.end());
        if (left != right) return false;
    }
    return true;
}
}

const std::vector<Definition>& definitions()
{
    static const std::vector<Definition> defs = [] {
    std::vector<Definition> defs{
        {K::AimHeld,"Aim held","Inputs","Current raw targeting hotkey state.",{},{{"Held",T::Boolean}}},
        {K::ShootHeld,"Shoot held","Inputs","Current shoot button state.",{},{{"Held",T::Boolean}}},
        {K::ZoomHeld,"Zoom held","Inputs","Current zoom button state.",{},{{"Held",T::Boolean}}},
        {K::TargetValid,"Target valid","Aim","A current target is available, including brief tracker prediction.",{{"Target",T::Target}},{{"Valid",T::Boolean}}},
        {K::TargetObserved,"Target observed","Aim","The target was actually observed, rather than only predicted.",{{"Target",T::Target}},{{"Observed",T::Boolean}}},
        {K::AutoShootEnabled,"Auto shoot setting","Settings","Enable Auto Shoot for this branch. The setting belongs to this node.",{},{{"Enabled",T::Boolean}}},
        {K::RecoilEnabled,"Recoil setting","Settings","Enable recoil compensation for this branch.",{},{{"Enabled",T::Boolean}}},
        {K::RecoilStrength,"Recoil strength","Settings","Vertical recoil compensation amount stored in this node.",{},{{"Strength",T::Number}}},
        {K::Target,"Target selection","Aim","Select a nearest or tracked target and apply the configured head/body aim offsets. Lock controls tracker locking. Mode: 0 config, 1 nearest, 2 tracker.",{{"Detections",T::Detections},{"Lock",T::Boolean}},{{"Target",T::Target}}},
        {K::Center,"Capture center","Inputs","Center of the detection frame in pixels.",{},{{"Point",T::Point}}},
        {K::TargetDistance,"Target distance","Aim","Distance from a selected target to the supplied center point in pixels.",{{"Target",T::Target},{"Center",T::Point}},{{"Pixels",T::Number}}},
        {K::CaptureFps,"Capture FPS","Inputs","Current measured capture frame rate.",{},{{"FPS",T::Number}}},
        {K::Boolean,"Boolean","Values","An editable true or false constant.",{},{{"Value",T::Boolean}}},
        {K::Number,"Float","Values","An editable floating-point constant, stored with double precision.",{},{{"Value",T::Float}}},
        {K::And,"AND","Logic","True when both inputs are true.",{{"A",T::Boolean},{"B",T::Boolean}},{{"Result",T::Boolean}}},
        {K::Or,"OR","Logic","True when either input is true.",{{"A",T::Boolean},{"B",T::Boolean}},{{"Result",T::Boolean}}},
        {K::Not,"NOT","Logic","Invert a boolean value.",{{"Value",T::Boolean}},{{"Result",T::Boolean}}},
        {K::Greater,"Greater than","Logic","True when A is greater than B.",{{"A",T::Number},{"B",T::Number}},{{"Result",T::Boolean}}},
        {K::Less,"Less than","Logic","True when A is less than B.",{{"A",T::Number},{"B",T::Number}},{{"Result",T::Boolean}}},
        {K::Add,"Add","Math","Add two numbers.",{{"A",T::Number},{"B",T::Number}},{{"Result",T::Number}}},
        {K::Subtract,"Subtract","Math","Subtract B from A.",{{"A",T::Number},{"B",T::Number}},{{"Result",T::Number}}},
        {K::Multiply,"Multiply","Math","Multiply two numbers.",{{"A",T::Number},{"B",T::Number}},{{"Result",T::Number}}},
        {K::Divide,"Divide","Math","Divide A by B. A zero divisor disables this branch.",{{"A",T::Number},{"B",T::Number}},{{"Result",T::Number}}},
        {K::Clamp,"Clamp","Math","Limit a number to the range between Min and Max.",{{"Value",T::Number},{"Min",T::Number},{"Max",T::Number}},{{"Result",T::Number}}},
        {K::PointX,"Point X","Points","Read the horizontal coordinate.",{{"Point",T::Point}},{{"X",T::Number}}},
        {K::PointY,"Point Y","Points","Read the vertical coordinate.",{{"Point",T::Point}},{{"Y",T::Number}}},
        {K::MakePoint,"Make point","Points","Build a point from X and Y coordinates.",{{"X",T::Number},{"Y",T::Number}},{{"Point",T::Point}}},
        {K::OffsetPoint,"Offset point","Points","Add pixel offsets to a point.",{{"Point",T::Point},{"X offset",T::Number},{"Y offset",T::Number}},{{"Point",T::Point}}},
        {K::Distance,"Distance","Points","Euclidean distance between two points.",{{"A",T::Point},{"B",T::Point}},{{"Pixels",T::Number}}},
        {K::Prediction,"Prediction","Aim","Apply the existing Kalman/delay prediction to the input point.",{{"Point",T::Point}},{{"Predicted",T::Point}}},
        {K::AimOutput,"Mouse output","Outputs","Dispatch the prepared mouse movement batch when Enable is true.",{{"Enable",T::Boolean},{"Movement",T::Movement}},{}},
        {K::AutoShootOutput,"Auto shoot","Outputs","Request shooting for the connected target when Enable is true.",{{"Enable",T::Boolean},{"Target",T::Target}},{}},
        {K::RecoilOutput,"Recoil movement","Outputs","Send vertical recoil counts. Connect Mouse calibration to include the movement in camera compensation and trails.",{{"Enable",T::Boolean},{"Strength",T::Number},{"Pixels per count",T::Point}},{}},
        {K::Detections,"Detections","Inputs","Current detection stream. Connect this to target selection to run tracking and selection, including target-loss updates.",{},{{"Detections",T::Detections}}},
        {K::AimPoint,"Aim point","Aim","Read the selected target's aim point after head/body offsets.",{{"Target",T::Target}},{{"Point",T::Point}}},
        {K::ScopeCheck,"Scope check","Aim","True when the connected target is within the configured auto shoot scope.",{{"Target",T::Target}},{{"In scope",T::Boolean}}},
        {K::SpeedCurve,"Speed curve","Aim","Calculate the current near/snap distance response and speed multiplier for a point.",{{"Point",T::Point}},{{"Speed",T::Number}}},
        {K::MouseDelta,"Mouse delta","Aim","Convert a point and speed to mouse counts using the current FOV and sensitivity profile.",{{"Point",T::Point},{"Speed",T::Number}},{{"Movement",T::Movement}}},
        {K::WindMotion,"Wind motion","Aim","Apply the configured Wind Mouse motion profile to the movement batch. Bypass to use direct movement.",{{"Movement",T::Movement}},{{"Movement",T::Movement}}},
        {K::AutoAimEnabled,"Auto aim setting","Settings","Enable automatic aiming for this branch. Connect through OR to combine with the targeting hotkey.",{},{{"Enabled",T::Boolean}}},
        {K::DeadZone,"Dead zone","Points","Return Center when Point is within Radius (pixels), otherwise pass Point through. Negative radius is treated as zero.",{{"Point",T::Point},{"Center",T::Point},{"Radius px",T::Number}},{{"Point",T::Point}}},
        {K::LerpPoint,"Blend points","Points","Interpolate from A to B by Amount, clamped to 0..1.",{{"A",T::Point},{"B",T::Point},{"Amount",T::Number}},{{"Point",T::Point}}},
        {K::ScaleMovement,"Scale movement","Movement","Multiply each prepared mouse step by Factor. Counts are rounded and capped to +/-32767 per axis; negative factors reverse movement.",{{"Movement",T::Movement},{"Factor",T::Number}},{{"Movement",T::Movement}}},
        {K::LimitMovement,"Limit movement","Movement","Limit the magnitude of the batch's combined displacement to Max Counts. Keep step order and distribute rounding so the final displacement remains within the limit.",{{"Movement",T::Movement},{"Max counts",T::Number}},{{"Movement",T::Movement}}},
        {K::Pulse,"Pulse","Logic","Periodic signal using monotonic runtime seconds. Period must be positive; Duty is clamped to 0..1. Useful for timed conditions and burst gates.",{{"Period sec",T::Number},{"Duty",T::Number}},{{"Active",T::Boolean}}},
        {K::RerouteBoolean,"Reroute boolean","Routing","Route a boolean connection without changing its value.",{{"Value",T::Boolean}},{{"Value",T::Boolean}}},
        {K::RerouteNumber,"Reroute float","Routing","Route a Float connection without changing its value.",{{"Value",T::Float}},{{"Value",T::Float}}},
        {K::ReroutePoint,"Reroute point","Routing","Route a point connection without changing its value.",{{"Value",T::Point}},{{"Value",T::Point}}},
        {K::RerouteDetections,"Reroute detections","Routing","Route the detection stream without changing its state.",{{"Value",T::Detections}},{{"Value",T::Detections}}},
        {K::RerouteTarget,"Reroute target","Routing","Route target data without changing validity, observation or freshness.",{{"Value",T::Target}},{{"Value",T::Target}}},
        {K::RerouteMovement,"Reroute movement","Routing","Route prepared mouse movement without changing its steps.",{{"Value",T::Movement}},{{"Value",T::Movement}}},
        {K::FilterClasses,"Filter classes","Detections","Keep the selected detection classes. Filtering happens on the connected stream.",{{"Detections",T::Detections}},{{"Detections",T::Detections}}},
        {K::FilterConfidence,"Filter confidence","Detections","Keep detections at or above Confidence.",{{"Detections",T::Detections},{"Confidence",T::Number,"threshold"}},{{"Detections",T::Detections}}},
        {K::FilterSize,"Filter size","Detections","Keep detections whose bounding-box area is inside the supplied range.",{{"Detections",T::Detections},{"Min area",T::Number,"min_area"},{"Max area",T::Number,"max_area"}},{{"Detections",T::Detections}}},
        {K::FilterCircle,"Filter circle","Detections","Keep detection centers inside a circle. Center defaults to capture center.",{{"Detections",T::Detections},{"Center",T::Point},{"Radius",T::Number,"radius"}},{{"Detections",T::Detections}}},
        {K::PreferClass,"Prefer class","Detections","Prefer the chosen class when present, retaining the original stream as fallback.",{{"Detections",T::Detections}},{{"Detections",T::Detections}}},
        {K::TargetNearest,"Nearest target","Targets","Choose from the connected detections without a tracker. Each node has independent settings.",{{"Detections",T::Detections},{"Lock",T::Boolean}},{{"Target",T::Target}}},
        {K::TargetTracker,"Track target","Targets","Track the connected detection stream with independent state for this node.",{{"Detections",T::Detections},{"Lock",T::Boolean}},{{"Target",T::Target}}},
        {K::TargetClass,"Target class","Targets","Read the selected target's class ID.",{{"Target",T::Target}},{{"Class",T::Integer}}},
        {K::TargetConfidence,"Target confidence","Targets","Read the selected target's confidence.",{{"Target",T::Target}},{{"Confidence",T::Number}}},
        {K::TargetTrackId,"Track ID","Targets","Read the track ID, or -1 for untracked targets.",{{"Target",T::Target}},{{"ID",T::Integer}}},
        {K::TargetVelocity,"Target velocity","Targets","Read the target velocity in capture pixels per second.",{{"Target",T::Target}},{{"Velocity",T::Point}}},
        {K::TargetMissedFrames,"Missed frames","Targets","Read how many observations the selected track has missed.",{{"Target",T::Target}},{{"Frames",T::Integer}}},
        {K::TargetAge,"Target age","Targets","Read the age of the current target observation in seconds.",{{"Target",T::Target}},{{"Seconds",T::Number}}},
        {K::TargetPosition,"Target position","Targets","Read the top-left bounding-box coordinate.",{{"Target",T::Target}},{{"Position",T::Point}}},
        {K::TargetSize,"Target size","Targets","Read the bounding-box width and height.",{{"Target",T::Target}},{{"Size",T::Point}}},
        {K::CameraCompensation,"Camera compensation","Aim","Subtract mouse movement since the observation. Bypass this node to use raw coordinates.",{{"Point",T::Point}},{{"Point",T::Point}}},
        {K::NormalizeFps,"Normalize FPS","Movement","Scale mouse counts by Base FPS / measured FPS above the base rate.",{{"Movement",T::Movement},{"FPS",T::Number}},{{"Movement",T::Movement}}},
        {K::KeyHeld,"Key held","Inputs","Current state of this node's virtual key, independent of global bindings.",{},{{"Held",T::Boolean}}},
        {K::KeyPressed,"Key pressed","Inputs","A rising-edge signal for this node's virtual key.",{},{{"Pressed",T::Boolean}}},
        {K::KeyReleased,"Key released","Inputs","A falling-edge signal for this node's virtual key.",{},{{"Released",T::Boolean}}},
        {K::Toggle,"Toggle","Logic","Toggle the stored state on each rising edge of Trigger.",{{"Trigger",T::Boolean}},{{"State",T::Boolean}}},
        {K::CaptureSource,"Capture source","Vision","Acquire the configured frame source. A graph has one physical source; its frame can fan out to multiple providers.",{},{{"Frame",T::Frame}}},
        {K::Inference,"Object inference","Vision","Run this node's model and backend on its connected frame. Decode settings belong to this node.",{{"Frame",T::Frame}},{{"Detections",T::Detections}}},
        {K::DepthInference,"Depth inference","Vision","Run the connected frame through this node's depth model.",{{"Frame",T::Frame}},{{"Depth",T::Depth}}},
        {K::DepthToMask,"Depth to mask","Vision","Build a near-depth mask with this node's threshold and expansion settings.",{{"Depth",T::Depth}},{{"Mask",T::Mask}}},
        {K::ApplyMask,"Apply frame mask","Vision","Apply the connected mask before inference. Disabled nodes pass Frame without evaluating Mask.",{{"Frame",T::Frame},{"Mask",T::Mask}},{{"Frame",T::Frame}}},
        {K::FilterMask,"Filter detection mask","Vision","Keep detections accepted by Mask. Disabled nodes pass detections without evaluating Mask.",{{"Detections",T::Detections},{"Mask",T::Mask}},{{"Detections",T::Detections}}},
        {K::FilterNms,"Non-maximum suppression","Detections","Suppress overlapping boxes in confidence order. Threshold one preserves all boxes.",{{"Detections",T::Detections},{"IoU threshold",T::Number,"threshold"}},{{"Detections",T::Detections}}},
        {K::LimitDetections,"Limit detections","Detections","Keep the highest-confidence detections up to this node's limit.",{{"Detections",T::Detections},{"Limit",T::Number,"limit"}},{{"Detections",T::Detections}}},
        {K::CollectDataset,"Collect dataset","Vision outputs","Save frames and optional labels under explicit graph control.",{{"Enable",T::Boolean,"enabled"},{"Frame",T::Frame},{"Detections",T::Detections}},{{"Saved",T::Boolean}}},
        {K::RenderOverlay,"Render overlay","Vision outputs","Publish this graph's frames, detections and optional mask or depth to the visual overlay.",{{"Enable",T::Boolean,"enabled"},{"Frame",T::Frame},{"Detections",T::Detections},{"Mask",T::Mask},{"Depth",T::Depth}},{{"Rendered",T::Boolean}}},
        {K::VisionOutput,"Vision output","Vision outputs","Publish the connected detections to the control graphs.",{{"Enable",T::Boolean,"enabled"},{"Detections",T::Detections}},{}} ,
        {K::ControlActive,"Control active","Inputs","True while the control graph's aiming output is active.",{},{{"Active",T::Boolean}}},
        {K::RerouteFrame,"Reroute frame","Routing","Route a frame without copying storage.",{{"Frame",T::Frame}},{{"Frame",T::Frame}}},
        {K::RerouteDepth,"Reroute depth","Routing","Route a depth image without copying storage.",{{"Depth",T::Depth}},{{"Depth",T::Depth}}},
        {K::RerouteMask,"Reroute mask","Routing","Route a mask without copying storage.",{{"Mask",T::Mask}},{{"Mask",T::Mask}}},
        {K::ScreenshotOutput,"Save screenshot","Vision outputs","Save the connected frame while Trigger is true, respecting this node's cooldown.",{{"Trigger",T::Boolean},{"Frame",T::Frame}},{{"Saved",T::Boolean}}},
        {K::Integer,"Int","Values","An editable signed 32-bit integer constant.",{},{{"Value",T::Integer}}},
        {K::ToFloat,"Int to Float","Conversion","Convert a signed integer to Float without losing precision.",{{"Int",T::Integer}},{{"Float",T::Float}}},
        {K::ToInteger,"Float to Int","Conversion","Convert Float to signed 32-bit Int with explicit rounding. Out-of-range values disable the branch.",{{"Float",T::Float}},{{"Int",T::Integer}}},
        {K::RerouteInteger,"Reroute int","Routing","Route an Int connection without changing its type or value.",{{"Value",T::Integer}},{{"Value",T::Integer}}},
        {K::IntegerAdd,"Add int","Integer math","Add signed integers. Overflow disables the branch.",{{"A",T::Integer},{"B",T::Integer}},{{"Result",T::Integer}}},
        {K::IntegerSubtract,"Subtract int","Integer math","Subtract signed integers. Overflow disables the branch.",{{"A",T::Integer},{"B",T::Integer}},{{"Result",T::Integer}}},
        {K::IntegerMultiply,"Multiply int","Integer math","Multiply signed integers. Overflow disables the branch.",{{"A",T::Integer},{"B",T::Integer}},{{"Result",T::Integer}}},
        {K::IntegerDivide,"Divide int","Integer math","Divide signed integers, truncating toward zero. Zero divisors and overflow disable the branch.",{{"A",T::Integer},{"B",T::Integer}},{{"Result",T::Integer}}},
        {K::IntegerClamp,"Clamp int","Integer math","Limit an Int to the inclusive Min/Max range.",{{"Value",T::Integer},{"Min",T::Integer},{"Max",T::Integer}},{{"Result",T::Integer}}},
        {K::String,"String","Values","An editable text value or path.",{},{{"Text",T::String}}},
        {K::ConcatString,"Join text","Text","Join two strings without changing their contents.",{{"A",T::String},{"B",T::String}},{{"Text",T::String}}},
        {K::IntegerToString,"Int to text","Conversion","Format an integer as decimal text.",{{"Int",T::Integer}},{{"Text",T::String}}},
        {K::FloatToString,"Float to text","Conversion","Format a Float with the selected decimal precision.",{{"Float",T::Float}},{{"Text",T::String}}},
        {K::GetMonitors,"Get monitors","Monitors","List connected monitors with stable device names and capture indices.",{},{{"Monitors",T::Monitors}}},
        {K::MonitorAt,"Monitor at index","Monitors","Select a monitor from a list. An invalid index disables this branch.",{{"Monitors",T::Monitors},{"Index",T::Integer,"index"}},{{"Monitor",T::Monitor}}},
        {K::PrimaryMonitor,"Primary monitor","Monitors","Get the primary monitor from the current display inventory.",{},{{"Monitor",T::Monitor}}},
        {K::MonitorCount,"Monitor count","Monitors","Count monitors in a list.",{{"Monitors",T::Monitors}},{{"Count",T::Integer}}},
        {K::MonitorIndex,"Monitor index","Monitors","Read the monitor's desktop-duplication capture index.",{{"Monitor",T::Monitor}},{{"Index",T::Integer}}},
        {K::MonitorName,"Monitor name","Monitors","Read the stable Windows display device name.",{{"Monitor",T::Monitor}},{{"Name",T::String}}},
        {K::MonitorSize,"Monitor size","Monitors","Read monitor width and height in desktop pixels.",{{"Monitor",T::Monitor}},{{"Size",T::Point}}},
        {K::MonitorPosition,"Monitor position","Monitors","Read the monitor's desktop origin, including negative coordinates.",{{"Monitor",T::Monitor}},{{"Position",T::Point}}},
        {K::MonitorPrimary,"Is primary monitor","Monitors","Test whether this monitor is the primary display.",{{"Monitor",T::Monitor}},{{"Primary",T::Boolean}}},
        {K::SelectBoolean,"Select boolean","Selection","Choose True or False input with a condition. Only the chosen branch executes.",{{"Condition",T::Boolean},{"True",T::Boolean},{"False",T::Boolean}},{{"Value",T::Boolean}}},
        {K::SelectFloat,"Select float","Selection","Choose one Float branch with a condition.",{{"Condition",T::Boolean},{"True",T::Float},{"False",T::Float}},{{"Value",T::Float}}},
        {K::SelectInteger,"Select int","Selection","Choose one Int branch with a condition.",{{"Condition",T::Boolean},{"True",T::Integer},{"False",T::Integer}},{{"Value",T::Integer}}},
        {K::SelectString,"Select text","Selection","Choose one text branch with a condition.",{{"Condition",T::Boolean},{"True",T::String},{"False",T::String}},{{"Value",T::String}}},
        {K::SelectPoint,"Select point","Selection","Choose one Point branch with a condition.",{{"Condition",T::Boolean},{"True",T::Point},{"False",T::Point}},{{"Value",T::Point}}},
        {K::SelectMonitor,"Select monitor","Selection","Choose one Monitor branch with a condition.",{{"Condition",T::Boolean},{"True",T::Monitor},{"False",T::Monitor}},{{"Value",T::Monitor}}},
        {K::SelectMonitors,"Select monitor list","Selection","Choose one monitor-list branch with a condition.",{{"Condition",T::Boolean},{"True",T::Monitors},{"False",T::Monitors}},{{"Value",T::Monitors}}},
        {K::RerouteString,"Reroute text","Routing","Route text without changing it.",{{"Text",T::String}},{{"Text",T::String}}},
        {K::RerouteMonitor,"Reroute monitor","Routing","Route a monitor selection.",{{"Monitor",T::Monitor}},{{"Monitor",T::Monitor}}},
        {K::RerouteMonitors,"Reroute monitor list","Routing","Route a display inventory without copying it.",{{"Monitors",T::Monitors}},{{"Monitors",T::Monitors}}},
    };
    const T variableTypes[]={T::Boolean,T::Float,T::Integer,T::String,T::Point,T::Monitor,T::Monitors,T::Detections,T::Target,T::Movement,T::Frame,T::Depth,T::Mask};
    const char* getTitles[]={"Get boolean","Get float","Get int","Get text","Get point","Get monitor","Get monitor list","Get detections","Get target","Get movement","Get frame","Get depth","Get mask"};
    const char* setTitles[]={"Set boolean","Set float","Set int","Set text","Set point","Set monitor","Set monitor list","Set detections","Set target","Set movement","Set frame","Set depth","Set mask"};
    for(std::size_t i=0;i<std::size(variableTypes);++i)
    {
        const auto get=static_cast<K>(static_cast<int>(K::GetBoolean)+i*2),set=static_cast<K>(static_cast<int>(get)+1);
        defs.push_back({get,getTitles[i],"Named values","Read the matching Set in this graph. Dependencies and cycles are checked before execution.",{},{{"Value",variableTypes[i]}},{},{{"name","Name","value",128}}});
        defs.push_back({set,setTitles[i],"Named values","Publish a named value in this graph. Get reads the current value through an implicit dependency.",{{"Value",variableTypes[i]}},{{"Value",variableTypes[i]}},{},{{"name","Name","value",128}}});
    }
    using P=ParameterKind;
    defs.push_back({K::ImagePreview,"Preview","Inspection","Inspect any connected value: video frames, images, depth, masks, text, numbers or structured data. Updates only while visible, at Preview FPS.",
        {{"Enabled",T::Boolean,"enabled"},{"Data",T::Any},{"Depth",T::Depth},{"Mask",T::Mask},{"Detections",T::Detections}}, {},
        {{"enabled","Enabled",1,0,1,P::Boolean},{"mode","View",0,0,3,P::Integer,"Auto|Data|Depth|Mask"},
         {"max_fps","Preview FPS",15,1,30,P::Integer},{"max_size","Image size",320,96,640,P::Integer},
         {"colormap","Depth colors",0,0,2,P::Integer,"Turbo|Gray|Inferno"},{"draw_boxes","Detection boxes",1,0,1,P::Boolean}}});
    for(const auto& item:std::vector<std::pair<K,const char*>>{{K::OverlayBoxes,"Overlay boxes"},{K::OverlayFrame,"Overlay frame / FOV"},
        {K::OverlayMotion,"Overlay trails"},{K::OverlayIcon,"Overlay icon"},{K::OverlayDepth,"Overlay depth / mask"},{K::OverlayCorrection,"Overlay correction"}})
        defs.push_back({item.first,item.second,"Overlay style","Add this layer's settings to a style chain. Connect the final Style to Render overlay.",{{"Style",T::OverlayStyle}},{{"Style",T::OverlayStyle}}});
    defs.push_back({K::OverlayOutput,"Render overlay","Vision outputs","Render the connected style. Unused depth and mask branches stay inactive.",
        {{"Enable",T::Boolean,"enabled"},{"Frame",T::Frame},{"Detections",T::Detections},{"Mask",T::Mask},{"Depth",T::Depth},{"Style",T::OverlayStyle}},{{"Rendered",T::Boolean}}});
    defs.push_back({K::DatasetLabels,"Dataset labels","Recording options","Configure labels independently from frame recording. Disconnect to record images only.",{},{{"Labels",T::LabelOptions}}});
    defs.push_back({K::DatasetOutput,"Collect dataset","Vision outputs","Record frames with optional label settings.",
        {{"Enable",T::Boolean,"enabled"},{"Frame",T::Frame},{"Detections",T::Detections},{"Labels",T::LabelOptions}},{{"Saved",T::Boolean}}});
    defs.push_back({K::RerouteOverlay,"Reroute overlay style","Routing","Route an overlay style without copying settings.",{{"Style",T::OverlayStyle}},{{"Style",T::OverlayStyle}}});
    defs.push_back({K::RerouteLabels,"Reroute label options","Routing","Route dataset label settings.",{{"Labels",T::LabelOptions}},{{"Labels",T::LabelOptions}}});
    for(const auto& item:std::vector<std::pair<K,T>>{{K::GetOverlay,T::OverlayStyle},{K::SetOverlay,T::OverlayStyle},{K::GetLabels,T::LabelOptions},{K::SetLabels,T::LabelOptions}})
    {
        const bool get=item.first==K::GetOverlay||item.first==K::GetLabels;
        const char* title=item.first==K::GetOverlay?"Get overlay style":item.first==K::SetOverlay?"Set overlay style":item.first==K::GetLabels?"Get label options":"Set label options";
        defs.push_back({item.first,title,"Named values","Share settings by name within this graph.",get?std::vector<Pin>{}:std::vector<Pin>{{"Value",item.second}},{{"Value",item.second}},{},{{"name","Name","value",128}}});
    }
    defs.push_back({K::CurrentFrame,"Frame input","Vision","Read the current frame from the active capture workflow.",{},{{"Frame",T::Frame}}});
    defs.push_back({K::MouseCalibration,"Mouse calibration","Mouse","Convert one device count to screen pixels using this node's mouse profile and the current capture dimensions. Connect to Recoil movement.",{},{{"Pixels per count",T::Point}}});
    defs.push_back({K::RecoilProfileOutput,"Recoil movement (local profile)","Outputs","Recoil output with its own calibration. Preserves old graphs at the node limit; use Mouse calibration and Recoil movement for separate cards.",{{"Enable",T::Boolean},{"Strength",T::Number}}, {}});
    defs.push_back({K::ChooseTarget,"Nearest target","Targets","Select the nearest candidate. Filter classes and choose the aim point in separate nodes.",{{"Detections",T::Detections}},{{"Target",T::Target}}});
    defs.push_back({K::TrackDetections,"Track target","Targets","Associate detections and keep the selected track. Geometry, velocity, offsets and prediction are separate graph stages.",{{"Detections",T::Detections},{"Lock",T::Boolean}},{{"Target",T::Target}}});
    defs.push_back({K::BoxPoint,"Point in box","Geometry","Position + Size * Anchor. Anchor uses relative coordinates, for example 0.5, 0.5 for the center.",{{"Position",T::Point},{"Size",T::Point},{"Anchor",T::Point}},{{"Point",T::Point}}});
    defs.push_back({K::TargetAnchor,"Aim offsets","Targets","Choose the relative head/body anchor from the connected class ID.",{{"Class",T::Integer},{"Body offset",T::Float,"body_y_offset"},{"Head offset",T::Float,"head_y_offset"}},{{"Anchor",T::Point}}});
    defs.push_back({K::BoxContainsPoint,"Point in box?","Geometry","Check the point against the scaled box. Connect Target observed separately when a real observation is required.",{{"Position",T::Point},{"Size",T::Point},{"Point",T::Point},{"Multiplier",T::Float,"multiplier"}},{{"Inside",T::Boolean}}});
    defs.push_back({K::MotionEstimate,"Estimate motion","Motion","Filter new point observations once and estimate their velocity. Read Position, Velocity, Damping and Ready from this shared sample.",{{"Point",T::Point},{"Reset",T::Boolean}},{{"Motion",T::Motion}}});
    defs.push_back({K::MotionPosition,"Filtered position","Motion","Read the filtered position from an estimated motion sample.",{{"Motion",T::Motion}},{{"Position",T::Point}}});
    defs.push_back({K::MotionVelocity,"Velocity","Motion","Read the estimated velocity in pixels per second. Estimation is shared with Filtered position.",{{"Motion",T::Motion}},{{"Velocity",T::Point}}});
    defs.push_back({K::MotionDamping,"Velocity damping","Motion","Read the sample's damping for the projection formula.",{{"Motion",T::Motion}},{{"Damping",T::Float}}});
    defs.push_back({K::MotionReady,"Motion ready","Motion","True after filter warmup. Connect to Project point to keep warmup behavior explicit.",{{"Motion",T::Motion}},{{"Ready",T::Boolean}}});
    defs.push_back({K::PredictionTime,"Prediction time","Motion","Combine lookahead, measured observation delay and an extra time offset.",{{"Lookahead",T::Float,"lookahead"}},{{"Seconds",T::Float}}});
    defs.push_back({K::ProjectPoint,"Project point","Motion","Project Position along Velocity for the requested time, with optional damping and warmup gating.",{{"Position",T::Point},{"Velocity",T::Point},{"Seconds",T::Float,"seconds"},{"Damping",T::Float,"damping"},{"Ready",T::Boolean,"ready"}},{{"Point",T::Point}}});
    defs.push_back({K::FutureTrail,"Future trail","Motion","Pass the point through and optionally publish future sample positions for Overlay trails.",{{"Point",T::Point},{"Motion",T::Motion},{"Seconds",T::Float,"seconds"}},{{"Point",T::Point}}});
    defs.push_back({K::RerouteMotion,"Reroute motion","Routing","Route an estimated motion sample.",{{"Motion",T::Motion}},{{"Motion",T::Motion}}});
    defs.push_back({K::GetMotion,"Get motion","Named values","Read the matching motion sample in this graph.",{},{{"Motion",T::Motion}},{},{{"name","Name","motion",128}}});
    defs.push_back({K::SetMotion,"Set motion","Named values","Share this motion sample by name within the graph.",{{"Motion",T::Motion}},{{"Motion",T::Motion}},{},{{"name","Name","motion",128}}});
    defs.push_back({K::MouseProfile,"Mouse calibration","Mouse","Named FOV and device calibration shared by aiming and recoil. Place it in any enabled Logic workflow; edit its inputs to update both consumers.",{},{{"Calibration",T::Calibration}}});
    defs.push_back({K::SharedCalibration,"Shared calibration","Mouse","Read the named Mouse calibration from an enabled Logic workflow. Missing or ambiguous names stop this branch.",{},{{"Calibration",T::Calibration}},{},{{"name","Name","mouse",128}}});
    defs.push_back({K::CalibrationScale,"Pixels per count","Mouse","Read the shared calibration's screen-pixel feedback scale, including the legacy raw-count fallback for zero profile axes.",{{"Calibration",T::Calibration}},{{"Pixels per count",T::Point}}});
    defs.push_back({K::PointDelta,"Point delta","Points","Subtract Center from Point before movement conversion.",{{"Point",T::Point},{"Center",T::Point}},{{"Delta",T::Point}},{{"implicit_center","Use capture center",1,0,1,P::Boolean}}});
    defs.push_back({K::PixelsToCounts,"Pixels to counts","Mouse","Convert pixel displacement to precise device counts. Zero sensitivity or profile axes produce no aim movement.",{{"Delta",T::Point},{"Calibration",T::Calibration}},{{"Movement",T::Movement}}});
    defs.push_back({K::ApplyMouseSpeed,"Apply speed","Mouse","Scale precise mouse counts before FPS normalization and integer quantization. Carries calibration into motion feedback.",{{"Movement",T::Movement},{"Speed",T::Float}},{{"Movement",T::Movement}}});
    defs.push_back({K::SharedMouseDelta,"Mouse delta (shared, combined)","Combined nodes","Compatibility form for full graphs. Uses the same named calibration as recoil.",{{"Point",T::Point},{"Speed",T::Float},{"Center",T::Point}},{{"Movement",T::Movement}},{},{{"name","Name","mouse",128}}});
    defs.push_back({K::SharedRecoilOutput,"Recoil (shared, combined)","Outputs","Recoil output using a named common profile when there is no room for separate reader nodes.",{{"Enable",T::Boolean},{"Strength",T::Float}}, {},{},{{"name","Name","mouse",128}}});
    defs.push_back({K::RerouteCalibration,"Reroute calibration","Routing","Route a shared mouse calibration.",{{"Calibration",T::Calibration}},{{"Calibration",T::Calibration}}});
    defs.push_back({K::GetCalibration,"Get calibration","Named values","Read the matching local Set calibration.",{},{{"Calibration",T::Calibration}},{},{{"name","Name","calibration",128}}});
    defs.push_back({K::SetCalibration,"Set calibration","Named values","Share this calibration by name inside this graph.",{{"Calibration",T::Calibration}},{{"Calibration",T::Calibration}},{},{{"name","Name","calibration",128}}});
    defs.push_back({K::SharedCalibrationScale,"Shared scale (combined)","Combined nodes","Read the named shared profile's pixels-per-count scale in a full graph.",{},{{"Pixels per count",T::Point}},{},{{"name","Name","mouse",128}}});
    defs[static_cast<std::size_t>(K::String)].textParameters={{"value","Value","",4096}};
    defs[static_cast<std::size_t>(K::FloatToString)].parameters={{"digits","Decimal places",3,0,12,P::Integer}};
    defs[static_cast<std::size_t>(K::MonitorAt)].parameters={{"index","Index",0,0,255,P::Integer}};
    defs[static_cast<size_t>(K::ToInteger)].parameters={{"rounding","Rounding",0,0,3,P::Integer,"Truncate|Floor|Ceil|Nearest"}};
    auto& prediction=defs[static_cast<size_t>(K::Prediction)];
    prediction.inputs={{"Point",T::Point},{"Lookahead sec",T::Number,"lookahead"},{"Reset",T::Boolean}};
    prediction.parameters={{"lookahead","Lookahead (seconds)",0.0,0,1.5},{"kalman_enabled","Kalman filter",1,0,1,P::Boolean},
        {"kalman_process_noise_position","Process noise: position",40,0.0001,5000},
        {"kalman_process_noise_velocity","Process noise: velocity",1800,0.0001,50000},
        {"kalman_measurement_noise","Measurement noise",35,0.0001,5000},
        {"kalman_velocity_damping","Velocity damping",0.5,0,3},
        {"kalman_max_velocity","Maximum velocity",20000,100,60000},
        {"kalman_warmup_frames","Warmup frames",2,0,100,P::Integer},
        {"kalman_compensate_detection_delay","Compensate detection delay",1,0,1,P::Boolean},
        {"kalman_additional_prediction_ms","Extra prediction (ms)",0,-80,150},
        {"kalman_reset_timeout_sec","Reset timeout (seconds)",0.5,0.05,3},
        {"prediction_futurePositions","Future positions",20,0,300,P::Integer},
        {"draw_futurePositions","Draw future positions",0,0,1,P::Boolean}};
    auto& estimate=defs[static_cast<size_t>(K::MotionEstimate)];
    for(const auto& spec:prediction.parameters)if(std::string(spec.key).rfind("kalman_",0)==0&&std::strcmp(spec.key,"kalman_compensate_detection_delay")&&std::strcmp(spec.key,"kalman_additional_prediction_ms"))estimate.parameters.push_back(spec);
    defs[static_cast<size_t>(K::PredictionTime)].parameters={prediction.parameters[0],prediction.parameters[8],prediction.parameters[9]};
    defs[static_cast<size_t>(K::PredictionTime)].parameters[1].label="Include delay";
    defs[static_cast<size_t>(K::ProjectPoint)].parameters={{"seconds","Seconds",0,0,1.5},{"damping","Damping",0,0,3},{"ready","Ready",1,0,1,P::Boolean}};
    defs[static_cast<size_t>(K::FutureTrail)].parameters={{"seconds","Seconds",0,0,1.5},prediction.parameters[11],prediction.parameters[12]};
    defs[static_cast<size_t>(K::FutureTrail)].parameters[1].label="Trail frames";
    defs[static_cast<size_t>(K::FutureTrail)].parameters[2].label="Show trail";
    defs[static_cast<size_t>(K::TargetAnchor)].parameters={{"class_head","Head class",1,0,100000,P::Integer},{"body_y_offset","Body Y offset",0.15,0,1},{"head_y_offset","Head Y offset",0.05,0,1}};
    defs[static_cast<size_t>(K::BoxContainsPoint)].parameters={{"multiplier","Scope multiplier",1,0.01,5}};
    auto selection=[](bool legacy,int mode) { return std::vector<ParameterSpec>{
        {"mode","Selection mode",static_cast<double>(mode),1,2,P::Integer,"Nearest|Tracker"},
        {"class_player","Body class",0,0,100000,P::Integer},{"class_head","Head class",1,0,100000,P::Integer},
        {"disable_headshot","Body only",0,0,1,P::Boolean},{"body_y_offset","Body Y offset",0.15,0,1},
        {"head_y_offset","Head Y offset",0.05,0,1},{"legacy_offsets","Apply legacy aim offsets",legacy?1.0:0.0,0,1,P::Boolean},
        {"filter_classes","Filter body/head classes",legacy?1.0:0.0,0,1,P::Boolean},
        {"prefer_head","Prefer head class",legacy?1.0:0.0,0,1,P::Boolean},
        {"merge_head_body","Merge head/body tracks",legacy?1.0:0.0,0,1,P::Boolean},
        {"max_missed_frames","Maximum missed frames",6,0,120,P::Integer}}; };
    defs[static_cast<size_t>(K::Target)].parameters=selection(true,2);
    defs[static_cast<size_t>(K::Target)].parameters.push_back({"legacy_camera_compensation","Legacy camera compensation",1,0,1,P::Boolean});
    defs[static_cast<size_t>(K::TargetNearest)].parameters=selection(false,1);
    defs[static_cast<size_t>(K::TargetTracker)].parameters=selection(false,2);
    defs[static_cast<size_t>(K::TrackDetections)].parameters={{"class_player","Body class",0,0,100000,P::Integer},{"class_head","Head class",1,0,100000,P::Integer},{"max_missed_frames","Missed frame limit",6,0,120,P::Integer}};
    for(const auto& entry:std::vector<std::pair<K,const char*>>{{K::Target,"Target (combined)"},{K::TargetNearest,"Nearest target (combined)"},{K::TargetTracker,"Track target (combined)"},
        {K::AimPoint,"Aim point (combined)"},{K::ScopeCheck,"Scope check (combined)"},{K::Prediction,"Prediction (combined)"}})
    {defs[static_cast<size_t>(entry.first)].title=entry.second;defs[static_cast<size_t>(entry.first)].category="Combined nodes";}
    auto& aimPoint=defs[static_cast<size_t>(K::AimPoint)];
    aimPoint.description="Calculate the aim point from bounding box, class and per-node relative offsets.";
    aimPoint.inputs={{"Target",T::Target},{"Body offset",T::Number,"body_y_offset"},{"Head offset",T::Number,"head_y_offset"}};
    aimPoint.parameters={{"class_head","Head class",1,0,100000,P::Integer},{"body_y_offset","Body Y offset",0.15,0,1},{"head_y_offset","Head Y offset",0.05,0,1}};
    auto& scope=defs[static_cast<size_t>(K::ScopeCheck)];
    scope.inputs={{"Target",T::Target},{"Multiplier",T::Number,"multiplier"},{"Center",T::Point}};
    scope.parameters={{"multiplier","Scope multiplier",1,0.01,5}};
    defs[static_cast<size_t>(K::SpeedCurve)].inputs={{"Point",T::Point},{"Center",T::Point}};
    defs[static_cast<size_t>(K::SpeedCurve)].parameters={{"min_speed","Minimum speed",1,0.01,20},{"max_speed","Maximum speed",1,0.01,20},
        {"snap_radius","Snap radius",0,0,1000},{"near_radius","Near radius",0,0,2000},{"exponent","Speed curve exponent",1,0.1,10},{"snap_boost","Snap boost",1,0,20},{"max_distance","Maximum distance (0 uses frame)",0,0,1000000}};
    auto& delta=defs[static_cast<size_t>(K::MouseDelta)];
    delta.inputs={{"Point",T::Point},{"Speed",T::Number},{"Center",T::Point}};
    delta.parameters={{"fov_x","Horizontal FOV",90,1,179,P::Integer},{"fov_y","Vertical FOV",90,1,179,P::Integer},
        {"sens","Sensitivity",1,0,100000},{"yaw","Yaw",0.022,0,1000},{"pitch","Pitch",0.022,0,1000},
        {"fov_scaled","Scale sensitivity with FOV",0,0,1,P::Boolean},{"base_fov","Base FOV",90,0,179},
        {"legacy_center","Legacy implicit center",0,0,1,P::Boolean},{"legacy_fps","Legacy implicit FPS scaling",0,0,1,P::Boolean}};
    delta.textParameters={{"profile_name","Profile name","UNIFIED",128}};
    auto& calibration=defs[static_cast<size_t>(K::MouseCalibration)];
    calibration.parameters.assign(delta.parameters.begin(),delta.parameters.begin()+7);
    calibration.textParameters=delta.textParameters;
    defs[static_cast<size_t>(K::MouseProfile)].parameters=calibration.parameters;
    defs[static_cast<size_t>(K::MouseProfile)].textParameters=calibration.textParameters;
    defs[static_cast<size_t>(K::MouseProfile)].textParameters[0].label="Calibration label";
    defs[static_cast<size_t>(K::MouseProfile)].textParameters.push_back({"name","Shared name","mouse",128});
    defs[static_cast<size_t>(K::SharedMouseDelta)].parameters={delta.parameters[7],delta.parameters[8]};
    delta.title="Mouse delta (combined)";delta.category="Combined nodes";
    calibration.title="Local calibration (legacy)";calibration.category="Combined nodes";
    auto& recoil=defs[static_cast<size_t>(K::RecoilProfileOutput)];
    recoil.parameters=calibration.parameters;
    recoil.textParameters=calibration.textParameters;
    defs[static_cast<size_t>(K::WindMotion)].parameters={{"enabled","Wind enabled",1,0,1,P::Boolean},
        {"wind_G","Gravity",9,0.05,50},{"wind_W","Wind",3,0,80},{"wind_M","Maximum step",10,1,100},{"wind_D","Distance",12,1,100}};
    for(K kind:{K::AutoAimEnabled,K::AutoShootEnabled,K::RecoilEnabled})
        defs[static_cast<size_t>(kind)].parameters={{"enabled","Enabled",0,0,1,P::Boolean}};
    defs[static_cast<size_t>(K::RecoilStrength)].parameters={{"strength","Strength",5,0,1000}};
    defs[static_cast<size_t>(K::FilterClasses)].parameters={{"class_a","First class",0,-1,100000,P::Integer},{"class_b","Second class",1,-1,100000,P::Integer},{"class_b_enabled","Allow second class",1,0,1,P::Boolean}};
    defs[static_cast<size_t>(K::FilterConfidence)].parameters={{"threshold","Minimum confidence",0.1,0,1}};
    defs[static_cast<size_t>(K::FilterSize)].parameters={{"min_area","Minimum area",0,0,1e9},{"max_area","Maximum area",1e9,0,1e9}};
    defs[static_cast<size_t>(K::FilterCircle)].parameters={{"enabled","Enabled",1,0,1,P::Boolean},{"radius","Radius (pixels)",160,0,100000},
        {"radius_percent","Radius percent (0 uses pixels)",0,0,100,P::Integer}};
    defs[static_cast<size_t>(K::PreferClass)].parameters={{"class_id","Preferred class",1,0,100000,P::Integer},{"enabled","Enabled",1,0,1,P::Boolean}};
    defs[static_cast<size_t>(K::CameraCompensation)].parameters={{"enabled","Enabled",1,0,1,P::Boolean}};
    defs[static_cast<size_t>(K::NormalizeFps)].parameters={{"base_fps","Base FPS",30,1,1000},{"enabled","Enabled",1,0,1,P::Boolean},{"preserve_fraction","Keep precise counts",0,0,1,P::Boolean}};
    for(K kind:{K::KeyHeld,K::KeyPressed,K::KeyReleased})
        defs[static_cast<size_t>(kind)].parameters={{"virtual_key","Virtual key code (0=None)",2,0,255,P::Integer}};
    auto& capture=defs[static_cast<size_t>(K::CaptureSource)];
    capture.parameters={{"enabled","Enabled",1,0,1,P::Boolean},{"detection_resolution","Detection resolution",320,64,2048,P::Integer},
        {"capture_fps","Capture FPS (0 unlimited)",120,0,1000,P::Integer},{"monitor_idx","Monitor index",0,0,64,P::Integer},
        {"capture_borders","Capture borders",0,0,1,P::Boolean},{"capture_cursor","Capture cursor",0,0,1,P::Boolean},
        {"virtual_camera_width","Camera width",1920,1,16384,P::Integer},{"virtual_camera_heigth","Camera height",1080,1,16384,P::Integer},
        {"udp_port","UDP port",1234,1,65535,P::Integer},{"capture_use_cuda","GPU capture",0,0,1,P::Boolean}};
    capture.textParameters={{"capture_method","Capture method","duplication_api",64},{"capture_target","Target type","monitor",64},
        {"capture_window_title","Window title","",512},{"virtual_camera_name","Camera name","",512},{"udp_ip","UDP address","127.0.0.1",128}};
    auto& inference=defs[static_cast<size_t>(K::Inference)];
    inference.parameters={{"enabled","Enabled",1,0,1,P::Boolean},{"fixed_input_size","Fixed model input size",1,0,1,P::Boolean},
        {"dml_device_id","DML device ID",0,0,64,P::Integer},{"use_cuda_graph","CUDA graph",0,0,1,P::Boolean},
        {"use_pinned_memory","Pinned memory",0,0,1,P::Boolean},{"confidence_threshold","Raw decode confidence",0.001,0,1},
        {"nms_threshold","Raw decode NMS threshold",1,0,1},{"max_detections","Maximum raw detections",100000,1,100000,P::Integer},
        {"class_count","Class count (0=automatic)",0,0,10000,P::Integer},
        {"export_enable_fp16","ONNX build: FP16",1,0,1,P::Boolean},{"export_enable_fp8","ONNX build: FP8",0,0,1,P::Boolean},
        {"trt_profile_min_size","ONNX profile minimum",160,32,8192,P::Integer},
        {"trt_profile_opt_size","ONNX profile optimal",320,32,8192,P::Integer},
        {"trt_profile_max_size","ONNX profile maximum",640,32,8192,P::Integer}};
    inference.textParameters={{"model_path","Model","",1024},{"backend","Backend",
#ifdef USE_CUDA
        "TRT",
#else
        "DML",
#endif
        64}};
    auto& depth=defs[static_cast<size_t>(K::DepthInference)];
    depth.parameters={{"enabled","Enabled",0,0,1,P::Boolean},{"depth_fps","Depth FPS (0 unlimited)",30,0,1000,P::Integer}};
    depth.textParameters={{"model_path","Model","",1024}};
    defs[static_cast<size_t>(K::DepthToMask)].parameters={{"enabled","Enabled",0,0,1,P::Boolean},
        {"depth_mask_near_percent","Near depth percent",25,0,100,P::Integer},{"depth_mask_expand","Expand mask",0,0,128,P::Integer},{"depth_mask_invert","Invert mask",0,0,1,P::Boolean},
        {"depth_mask_fps","Mask FPS (0 unlimited)",5,0,1000,P::Integer}};
    for(K kind:{K::ApplyMask,K::FilterMask})defs[static_cast<size_t>(kind)].parameters={{"enabled","Enabled",0,0,1,P::Boolean},{"depth_mask_hold_frames","Hold mask frames",0,0,1000,P::Integer}};
    defs[static_cast<size_t>(K::FilterNms)].parameters={{"threshold","IoU threshold",0.5,0,1},{"class_agnostic","Class agnostic",0,0,1,P::Boolean},{"max_candidates","Maximum candidates (0 unlimited)",1000,0,100000,P::Integer}};
    defs[static_cast<size_t>(K::LimitDetections)].parameters={{"limit","Detection limit (0 unlimited)",100,0,100000,P::Integer}};
    auto& collector=defs[static_cast<size_t>(K::CollectDataset)];
    collector.parameters={{"enabled","Enabled",0,0,1,P::Boolean},{"collect_only_when_aimbot_running","Only while control is active",1,0,1,P::Boolean},
        {"collect_only_when_targets_present","Only with targets",1,0,1,P::Boolean},{"collect_save_every_n_frames","Save every N frames",1,1,100000,P::Integer},
        {"collect_jpeg_quality","JPEG quality",90,1,100,P::Integer},{"collect_cooldown_ms","Capture cooldown (ms)",500,0,60000,P::Integer},
        {"auto_label_data","Write labels",0,0,1,P::Boolean},{"auto_label_min_conf","Minimum label confidence",0.1,0,1},
        {"auto_label_max_boxes","Maximum labels",100,1,100000,P::Integer}};
    collector.textParameters={{"collect_output_dir","Dataset folder","datasets",1024},{"auto_label_record_classes","Label classes","",512},{"model_name","Dataset model label","",256}};
    auto& render=defs[static_cast<size_t>(K::RenderOverlay)];
    render.parameters={{"enabled","Enabled",0,0,1,P::Boolean},{"game_overlay_max_fps","Overlay FPS (0 unlimited)",120,0,1000,P::Integer},
        {"game_overlay_draw_boxes","Draw boxes",1,0,1,P::Boolean},{"game_overlay_compensate_latency","Compensate overlay latency",1,0,1,P::Boolean},
        {"game_overlay_draw_future","Draw future positions",1,0,1,P::Boolean},{"game_overlay_draw_wind_tail","Draw wind trail",0,0,1,P::Boolean},
        {"game_overlay_draw_frame","Draw capture frame",0,0,1,P::Boolean},{"game_overlay_draw_circle_fov","Draw FOV circle",0,0,1,P::Boolean},
        {"game_overlay_show_target_correction","Show target correction",0,0,1,P::Boolean},
        {"game_overlay_box_a","Box alpha",255,0,255,P::Integer},{"game_overlay_box_r","Box red",96,0,255,P::Integer},{"game_overlay_box_g","Box green",205,0,255,P::Integer},{"game_overlay_box_b","Box blue",255,0,255,P::Integer},
        {"game_overlay_frame_a","Frame alpha",255,0,255,P::Integer},{"game_overlay_frame_r","Frame red",96,0,255,P::Integer},{"game_overlay_frame_g","Frame green",205,0,255,P::Integer},{"game_overlay_frame_b","Frame blue",255,0,255,P::Integer},
        {"game_overlay_box_thickness","Box thickness",1,0.1,20},{"game_overlay_frame_thickness","Frame thickness",1,0.1,20},
        {"game_overlay_future_point_radius","Future point radius",2,0.1,100},{"game_overlay_future_alpha_falloff","Future alpha falloff",0.9,0.1,5},
        {"game_overlay_icon_enabled","Draw icon",0,0,1,P::Boolean},{"game_overlay_icon_width","Icon width",32,1,4096,P::Integer},{"game_overlay_icon_height","Icon height",32,1,4096,P::Integer},
        {"game_overlay_icon_offset_x","Icon X offset",0,-10000,10000},{"game_overlay_icon_offset_y","Icon Y offset",0,-10000,10000},{"game_overlay_icon_class","Icon class (-1 all)",-1,-1,100000,P::Integer},
        {"circle_fov_enabled","Circular FOV enabled",0,0,1,P::Boolean},{"circle_fov_radius_percent","FOV radius percent",100,1,100,P::Integer},
        {"depth_debug_overlay_enabled","Show depth debug",0,0,1,P::Boolean},{"depth_mask_enabled","Show depth mask",0,0,1,P::Boolean},{"depth_mask_alpha","Depth mask alpha",100,0,255,P::Integer},{"depth_colormap","Depth colormap",2,0,21,P::Integer},
        {"snap_radius","Snap radius demo",0,0,1000},{"near_radius","Near radius demo",0,0,2000},{"min_speed","Minimum speed demo",1,0.01,20},{"max_speed","Maximum speed demo",1,0.01,20},{"exponent","Curve exponent demo",1,0.1,10},{"snap_boost","Snap boost demo",1,0,20}};
    render.textParameters={{"game_overlay_icon_path","Icon path","",1024},{"game_overlay_icon_anchor","Icon anchor","center",64}};
    const auto subset=[&](K kind,const Definition& source,std::initializer_list<const char*> keys){
        auto& destination=defs[static_cast<std::size_t>(kind)];
        for(const char* key:keys)
        {
            for(const auto& spec:source.parameters)if(std::strcmp(key,spec.key)==0)destination.parameters.push_back(spec);
            for(const auto& spec:source.textParameters)if(std::strcmp(key,spec.key)==0)destination.textParameters.push_back(spec);
        }
    };
    subset(K::OverlayBoxes,render,{"game_overlay_draw_boxes","game_overlay_compensate_latency","game_overlay_box_a","game_overlay_box_r","game_overlay_box_g","game_overlay_box_b","game_overlay_box_thickness"});
    subset(K::OverlayFrame,render,{"game_overlay_draw_frame","game_overlay_draw_circle_fov","circle_fov_enabled","circle_fov_radius_percent","game_overlay_frame_a","game_overlay_frame_r","game_overlay_frame_g","game_overlay_frame_b","game_overlay_frame_thickness"});
    subset(K::OverlayMotion,render,{"game_overlay_draw_future","game_overlay_draw_wind_tail","game_overlay_future_point_radius","game_overlay_future_alpha_falloff"});
    subset(K::OverlayIcon,render,{"game_overlay_icon_enabled","game_overlay_icon_width","game_overlay_icon_height","game_overlay_icon_offset_x","game_overlay_icon_offset_y","game_overlay_icon_class","game_overlay_icon_path","game_overlay_icon_anchor"});
    subset(K::OverlayDepth,render,{"depth_debug_overlay_enabled","depth_mask_enabled","depth_mask_alpha","depth_colormap"});
    subset(K::OverlayCorrection,render,{"game_overlay_show_target_correction","snap_radius","near_radius","min_speed","max_speed","exponent","snap_boost"});
    subset(K::OverlayOutput,render,{"enabled","game_overlay_max_fps"});
    subset(K::DatasetLabels,collector,{"auto_label_data","auto_label_min_conf","auto_label_max_boxes","auto_label_record_classes","model_name"});
    subset(K::DatasetOutput,collector,{"enabled","collect_only_when_aimbot_running","collect_only_when_targets_present","collect_save_every_n_frames","collect_jpeg_quality","collect_cooldown_ms","collect_output_dir"});
    defs[static_cast<size_t>(K::VisionOutput)].parameters={{"enabled","Enabled",1,0,1,P::Boolean}};
    defs[static_cast<size_t>(K::ScreenshotOutput)].parameters={{"cooldown_ms","Capture cooldown (ms)",500,0,60000,P::Integer},{"quality","Image quality",95,1,100,P::Integer}};
    defs[static_cast<size_t>(K::ScreenshotOutput)].textParameters={{"output_dir","Screenshot folder","screenshots",1024},{"format","Format (png or jpg)","png",16}};
    defs[static_cast<std::size_t>(K::CaptureSource)].inputs={{"Monitor",T::Monitor}};
    defs[static_cast<std::size_t>(K::CaptureSource)].textParameters.push_back({"monitor_device","Monitor device","",128});
    for(auto& def:defs)
    {
        def.dataInputs=def.inputs.size();
        const auto exists=[&](const char* key){return std::any_of(def.inputs.begin(),def.inputs.end(),[&](const Pin& p){return p.defaultParameter&&std::strcmp(p.defaultParameter,key)==0;});};
        for(const auto& spec:def.parameters)if(!exists(spec.key))
            def.inputs.push_back({spec.label,spec.kind==P::Boolean?T::Boolean:spec.kind==P::Integer?T::Integer:T::Float,spec.key,true});
        for(const auto& spec:def.textParameters)if(!exists(spec.key)&&!((variableGet(def.kind)||variableSet(def.kind)||calibrationIdentifier(def.kind))&&std::strcmp(spec.key,"name")==0))
            def.inputs.push_back({spec.label,T::String,spec.key,true});
    }
    return defs;
    }();
    return defs;
}
double parameter(const Node& node,const std::string& key)
{
    const auto value=node.parameters.find(key); if(value!=node.parameters.end())return value->second;
    for(const auto& spec:definition(node.kind).parameters)if(key==spec.key)return spec.defaultValue;
    return 0;
}
std::string textParameter(const Node& node,const std::string& key)
{
    const auto value=node.textParameters.find(key);if(value!=node.textParameters.end())return value->second;
    for(const auto& spec:definition(node.kind).textParameters)if(key==spec.key)return spec.defaultValue;
    return {};
}
void initializeParameters(Node& node)
{
    for(const auto& spec:definition(node.kind).parameters)node.parameters.emplace(spec.key,spec.defaultValue);
    for(const auto& spec:definition(node.kind).textParameters)node.textParameters.emplace(spec.key,spec.defaultValue);
}
Node defaultNode(NodeKind kind,int id,float x,float y)
{
    Node node;node.id=id;node.kind=kind;node.x=x;node.y=y;
    node.value=kind==K::Boolean?1:0;initializeParameters(node);if(kind==K::ApplyMouseSpeed)node.inputValues[1]=1;return node;
}
Point projectPoint(Point position,Point velocity,double seconds,double damping,bool ready)
{
    const double t=ready?std::clamp(seconds,0.0,1.5):0;
    const double scale=damping>1e-6?(1-std::exp(-damping*t))/damping:t;
    return {position.x+velocity.x*scale,position.y+velocity.y*scale};
}
Point mousePixelCalibration(const Node& node,Point frameSize)
{
    const double sens=parameter(node,"sens"),yaw=parameter(node,"yaw"),pitch=parameter(node,"pitch");
    // The legacy conversion used raw counts when any profile axis was zero.
    if(sens==0||yaw==0||pitch==0)return {1,1};
    const double fovX=std::max(1.0,parameter(node,"fov_x")),fovY=std::max(1.0,parameter(node,"fov_y"));
    const double base=parameter(node,"base_fov");
    const double scale=parameter(node,"fov_scaled")!=0&&base>1?fovX/base:1;
    return {sens*yaw*scale*std::max(1.0,frameSize.x)/fovX,sens*pitch*scale*std::max(1.0,frameSize.y)/fovY};
}
bool calibrationIdentifier(NodeKind kind){return kind==K::MouseProfile||kind==K::SharedCalibration||kind==K::SharedMouseDelta||kind==K::SharedRecoilOutput||kind==K::SharedCalibrationScale;}
CalibrationData mouseCalibration(const Node& node,Point size)
{
    CalibrationData result;const double x=parameter(node,"fov_x"),y=parameter(node,"fov_y"),base=parameter(node,"base_fov");
    const double sensitivity=parameter(node,"sens"),yaw=parameter(node,"yaw"),pitch=parameter(node,"pitch");
    const double scale=parameter(node,"fov_scaled")!=0&&base>1?x/base:1;
    result.degreesPerPixel={x/std::max(1.0,size.x),y/std::max(1.0,size.y)};
    result.countsPerDegree={sensitivity*yaw*scale,sensitivity*pitch*scale};
    result.pixelsPerCount=mousePixelCalibration(node,size);result.aimEnabled=sensitivity!=0&&yaw!=0&&pitch!=0&&x>0&&y>0;
    result.profileName=textParameter(node,"profile_name");return result;
}
Movement pixelsToCounts(Point delta,const CalibrationData& calibration)
{
    Movement result;if(!calibration.aimEnabled)return result;
    const double x=delta.x*calibration.degreesPerPixel.x/calibration.countsPerDegree.x;
    const double y=delta.y*calibration.degreesPerPixel.y/calibration.countsPerDegree.y;
    result.preciseCounts={x,y};result.hasPreciseCounts=true;result.pixelScaleX=calibration.pixelsPerCount.x;result.pixelScaleY=calibration.pixelsPerCount.y;
    if(!std::isfinite(x)||!std::isfinite(y))return result;
    const int dx=static_cast<int>(std::clamp(x,-32767.0,32767.0)),dy=static_cast<int>(std::clamp(y,-32767.0,32767.0));
    if(dx||dy)result.steps.push_back({dx,dy});return result;
}
Movement applyMouseSpeed(const Movement& input,double speed)
{
    Movement result;result.pixelScaleX=input.pixelScaleX;result.pixelScaleY=input.pixelScaleY;
    if(input.hasPreciseCounts)
    {
        const double x=input.preciseCounts.x*speed,y=input.preciseCounts.y*speed;result.preciseCounts={x,y};result.hasPreciseCounts=true;
        if(!std::isfinite(x)||!std::isfinite(y))return result;
        const int dx=static_cast<int>(std::clamp(x,-32767.0,32767.0)),dy=static_cast<int>(std::clamp(y,-32767.0,32767.0));
        if(dx||dy)result.steps.push_back({dx,dy});
    }
    else for(const auto& step:input.steps)
    {result.steps.push_back({static_cast<int>(std::lround(std::clamp(step.dx*speed,-32767.0,32767.0))),static_cast<int>(std::lround(std::clamp(step.dy*speed,-32767.0,32767.0)))});}
    return result;
}
void migrateRecoilCalibration(Workspace& workspace)
{
    Node profile=defaultNode(K::MouseDelta,1);
    for(const auto& graph:workspace.graphs)if(graph.active&&graph.kind!=GraphKind::Vision)
    {
        const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const Node& node){return node.kind==K::MouseDelta;});
        if(found!=graph.nodes.end()){profile=*found;break;}
    }
    for(auto& graph:workspace.graphs)
    {
        std::vector<int> outputs;
        for(const auto& node:graph.nodes)if(node.kind==K::RecoilOutput&&
            std::none_of(graph.links.begin(),graph.links.end(),[&](const Link& link){return link.toNode==node.id&&link.toPort==2;}))outputs.push_back(node.id);
        if(outputs.empty())continue;
        Node calibration=defaultNode(K::MouseCalibration,1);
        for(const auto& spec:definition(K::MouseCalibration).parameters)calibration.parameters[spec.key]=parameter(profile,spec.key);
        calibration.textParameters["profile_name"]=textParameter(profile,"profile_name");
        float bottom=0;
        while(std::any_of(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==calibration.id;}))++calibration.id;
        for(const auto& node:graph.nodes)bottom=std::max(bottom,node.y+Layout::height(node));
        if(graph.nodes.size()>=MaxNodes||graph.links.size()+outputs.size()>MaxLinks)
        {
            // A full graph keeps the same formula on its output card rather
            // than dropping the calibration or adding nodes beyond its limit.
            for(auto& node:graph.nodes)if(std::find(outputs.begin(),outputs.end(),node.id)!=outputs.end())
            {
                node.kind=K::RecoilProfileOutput;
                for(const auto& spec:definition(K::MouseCalibration).parameters)node.parameters[spec.key]=parameter(calibration,spec.key);
                node.textParameters["profile_name"]=textParameter(calibration,"profile_name");
                if(node.height>0&&node.height<Layout::minimumHeight(node))node.height=Layout::up(Layout::minimumHeight(node));
            }
            continue;
        }
        calibration.y=Layout::up(bottom+64);
        for(int id:outputs)graph.links.push_back({calibration.id,0,id,2});
        if(graph.groups.size()<MaxGroups)
        {
            int id=1;for(const auto& group:graph.groups)id=std::max(id,group.id+1);
            graph.groups.push_back({id,"Mouse calibration",-32,calibration.y-64,Layout::width(calibration)+64,Layout::height(calibration)+96,0x40adde66u,{calibration.id}});
        }
        graph.nodes.push_back(std::move(calibration));
    }
}
const Definition& definition(NodeKind kind)
{
    const auto index = static_cast<std::size_t>(kind);
    return definitions()[index < definitions().size() ? index : static_cast<std::size_t>(K::Boolean)];
}
bool canBypass(NodeKind kind)
{
    if (kind < K::AimHeld || kind >= K::Count) return false;
    if (isSink(kind)) return true;
    const auto& def = definition(kind);
    if (def.outputs.size() != 1) return false;
    return std::any_of(def.inputs.begin(),def.inputs.begin()+def.dataInputs,[&](const Pin& p) { return p.type == def.outputs[0].type; });
}
const char* graphName(GraphKind kind)
{
    switch (kind) { case GraphKind::Aim: return "Aiming"; case GraphKind::AutoShoot: return "Auto shoot"; case GraphKind::Recoil: return "Recoil";case GraphKind::Vision:return "Vision"; }
    return "Unknown";
}
const char* typeName(ValueType type)
{
    switch (type) { case T::Boolean: return "Boolean"; case T::Number: return "Float"; case T::Integer: return "Int"; case T::Point: return "Point (pixels)";
        case T::Detections: return "Detections"; case T::Target: return "Target"; case T::Movement: return "Mouse movement";
        case T::Frame:return "Frame";case T::Depth:return "Depth";case T::Mask:return "Mask";
        case T::String:return "String";case T::Monitor:return "Monitor";case T::Monitors:return "Monitors";
        case T::OverlayStyle:return "Overlay style";case T::LabelOptions:return "Label options";case T::Motion:return "Motion sample";case T::Calibration:return "Mouse calibration";case T::Any:return "Any"; }
    return "Unknown";
}
bool canConnectTypes(ValueType output,ValueType input)
{
    return output!=T::Any&&(input==T::Any||output==input||(output==T::Integer&&input==T::Float));
}
bool variableGet(NodeKind kind){return (kind>=K::GetBoolean&&kind<=K::SetMask&&((static_cast<int>(kind)-static_cast<int>(K::GetBoolean))%2)==0)||kind==K::GetOverlay||kind==K::GetLabels||kind==K::GetMotion||kind==K::GetCalibration;}
bool variableSet(NodeKind kind){return (kind>=K::GetBoolean&&kind<=K::SetMask&&!variableGet(kind))||kind==K::SetOverlay||kind==K::SetLabels||kind==K::SetMotion||kind==K::SetCalibration;}
MonitorList systemMonitors()
{
    static std::mutex mutex;static MonitorList cached;static std::chrono::steady_clock::time_point sampled{};
    std::lock_guard<std::mutex> lock(mutex);const auto now=std::chrono::steady_clock::now();
    if(cached&&now-sampled<std::chrono::seconds(2))return cached;
    auto list=std::make_shared<std::vector<MonitorInfo>>();
#ifdef _WIN32
    const auto utf8=[](const wchar_t* value){const int n=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);std::string text(static_cast<std::size_t>(std::max(0,n)),0);if(n)WideCharToMultiByte(CP_UTF8,0,value,-1,text.data(),n,nullptr,nullptr);if(!text.empty())text.pop_back();return text;};
    std::vector<MONITORINFOEXW> windows;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL{MONITORINFOEXW info{};info.cbSize=sizeof(info);if(GetMonitorInfoW(monitor,&info))reinterpret_cast<std::vector<MONITORINFOEXW>*>(data)->push_back(info);return TRUE;},reinterpret_cast<LPARAM>(&windows));
    for(std::size_t i=0;i<windows.size()&&i<256;++i)
    {
        const auto& w=windows[i];MonitorInfo m;m.index=-1;m.winrtIndex=static_cast<int>(i);m.device=utf8(w.szDevice);m.name=m.device;
        m.x=w.rcMonitor.left;m.y=w.rcMonitor.top;m.width=w.rcMonitor.right-w.rcMonitor.left;m.height=w.rcMonitor.bottom-w.rcMonitor.top;m.primary=(w.dwFlags&MONITORINFOF_PRIMARY)!=0;list->push_back(std::move(m));
    }
    IDXGIFactory1* factory=nullptr;
    if(SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory))))
    {
        int index=0;for(UINT a=0;;++a)
        {
            IDXGIAdapter1* adapter=nullptr;if(FAILED(factory->EnumAdapters1(a,&adapter)))break;
            for(UINT o=0;;++o)
            {
                IDXGIOutput* output=nullptr;if(FAILED(adapter->EnumOutputs(o,&output)))break;
                DXGI_OUTPUT_DESC desc{};if(SUCCEEDED(output->GetDesc(&desc))){const auto device=utf8(desc.DeviceName);for(auto& m:*list)if(m.device==device)m.index=index;}
                ++index;output->Release();
            }
            adapter->Release();
        }
        factory->Release();
    }
#endif
    bool same=cached&&cached->size()==list->size();
    for(std::size_t i=0;same&&i<list->size();++i){const auto& a=(*cached)[i];const auto& b=(*list)[i];same=a.index==b.index&&a.winrtIndex==b.winrtIndex&&a.device==b.device&&a.x==b.x&&a.y==b.y&&a.width==b.width&&a.height==b.height&&a.primary==b.primary;}
    if(!same)cached=list;sampled=now;return cached;
}
static Workspace previousStarterGraphs(bool compact=true,bool expanded=true)
{
    Workspace w;
    for (std::size_t i = 0; i < GraphCount; ++i) {w.graphs[i].kind = static_cast<GraphKind>(i);w.graphs[i].workflow=static_cast<int>(i)+1;}
    w.graphs[0].nodes = {{1,K::AimHeld,0,0},{2,K::TargetValid,512,320},{3,K::And,512,64},
        {4,K::Target,256,320},{5,K::Prediction,0,544},{6,K::AimOutput,768,736},
        {7,K::Detections,0,320},{8,K::AimPoint,768,320},
        {9,K::SpeedCurve,256,672},{10,K::MouseDelta,512,544},{11,K::WindMotion,768,544},
        {12,K::AutoAimEnabled,0,128},{13,K::Or,256,64}};
    w.graphs[0].links = {{1,0,13,0},{12,0,13,1},{13,0,3,0},{13,0,4,1},{2,0,3,1},{3,0,6,0},{7,0,4,0},{4,0,2,0},{4,0,8,0},
        {8,0,5,0},{5,0,9,0},{5,0,10,0},{9,0,10,1},{10,0,11,0},{11,0,6,1}};
    w.graphs[0].groups = {{1,"Activation",-32,-64,1024,288,0x40ffcd60u,{1,12,13,3}},
        {2,"Target selection",-32,256,1024,192,0x40ff93adu,{7,4,2,8}},
        {3,"Motion and output",-32,480,1024,416,0x40adde66u,{5,9,10,11,6}}};
    w.graphs[1].nodes = {{1,K::AimHeld,0,0},{2,K::TargetObserved,512,320},
        {3,K::AutoShootEnabled,768,64},{4,K::And,256,672},{5,K::And,512,672},{6,K::AutoShootOutput,0,672},
        {7,K::Detections,0,320},{8,K::Target,256,320},{9,K::ScopeCheck,512,448},{10,K::And,768,672},
        {11,K::AutoAimEnabled,0,128},{12,K::Or,256,64}};
    w.graphs[1].links = {{1,0,12,0},{11,0,12,1},{12,0,4,0},{12,0,8,1},{2,0,4,1},{4,0,5,0},{3,0,5,1},{5,0,10,0},
        {7,0,8,0},{8,0,2,0},{8,0,9,0},{9,0,10,1},{10,0,6,0},{8,0,6,1}};
    w.graphs[1].groups = {{1,"Activation",-32,-64,1024,288,0x40ffcd60u,{1,11,12,3}},
        {2,"Target and scope",-32,256,1024,320,0x40ff93adu,{7,8,2,9}},
        {3,"Fire gate and output",-32,608,1024,256,0x40ad91ffu,{4,5,10,6}}};
    w.graphs[2].nodes = {{1,K::ShootHeld,0,0},{2,K::ZoomHeld,256,0},{3,K::RecoilEnabled,512,0},
        {4,K::And,768,0},{5,K::And,0,192},{6,K::RecoilStrength,256,192},{7,K::RecoilOutput,512,192}};
    w.graphs[2].links = {{1,0,4,0},{2,0,4,1},{4,0,5,0},{3,0,5,1},{5,0,7,0},{6,0,7,1}};
    w.graphs[2].groups = {{1,"Activation",-32,-64,1024,192,0x40ffcd60u,{1,2,3,4}},
        {2,"Recoil amount and output",-32,160,768,192,0x40adde66u,{5,6,7}}};
    // New graphs carry real detection streams through explicit stages. Legacy
    // Target remains available only to preserve imported version 1/2 graphs.
    for(int graphIndex=0;graphIndex<2;++graphIndex)
    {
        auto& graph=w.graphs[graphIndex];const int selected=graphIndex==0?4:8;
        for(auto& node:graph.nodes)if(node.id==selected)node.kind=K::TargetTracker;
        graph.nodes.push_back({30,K::FilterClasses,0,256});
        graph.nodes.push_back({31,K::PreferClass,256,256});
        for(auto& link:graph.links)if(link.toNode==selected&&link.toPort==0)link.fromNode=31;
        graph.links.push_back({7,0,30,0});graph.links.push_back({30,0,31,0});
        graph.groups[1].nodes.push_back(30);graph.groups[1].nodes.push_back(31);
        // Put the upstream filters above the target row without covering it.
        for(auto& node:graph.nodes)
        {
            if(node.id==7){node.x=0;node.y=320;}
            if(node.id==30){node.x=256;node.y=320;}
            if(node.id==31){node.x=512;node.y=320;}
            if(node.id==selected){node.x=768;node.y=320;}
            if(node.id==2){node.x=768;node.y=448;}
            if(graphIndex==0&&node.id==8){node.x=512;node.y=448;}
        }
        graph.groups[1].height=352;
    }
    auto& aim=w.graphs[0];
    aim.nodes.push_back({32,K::CameraCompensation,256,608});
    aim.nodes.push_back({33,K::NormalizeFps,768,736});
    aim.nodes.push_back({34,K::CaptureFps,512,864});
    aim.nodes.push_back({35,K::Center,0,736});
    for(auto& node:aim.nodes)
    {
        if(node.id==5){node.x=0;node.y=608;}
        if(node.id==9){node.x=256;node.y=864;}
        if(node.id==10){node.x=512;node.y=608;}
        if(node.id==11){node.x=768;node.y=864;}
        if(node.id==6){node.x=768;node.y=1056;}
    }
    for(auto& link:aim.links)
    {
        if(link.toNode==5&&link.toPort==0)link.fromNode=32;
        if(link.toNode==11&&link.toPort==0)link.fromNode=33;
    }
    aim.links.push_back({8,0,32,0});aim.links.push_back({10,0,33,0});aim.links.push_back({34,0,33,1});aim.links.push_back({35,0,10,2});
    for(auto& entry:aim.nodes)if(entry.id==5||entry.id==9||entry.id==10||entry.id==11||entry.id==6||entry.id==32||entry.id==33||entry.id==34||entry.id==35)entry.y+=64;
    aim.groups[2]={3,"Prediction and motion",-32,640,1024,640,0x40adde66u,{5,9,10,11,6,32,33,34,35}};
    auto& shoot=w.graphs[1];
    for(auto& node:shoot.nodes)
    {
        if(node.id==9){node.x=512;node.y=448;}
        if(node.id==4||node.id==5||node.id==6||node.id==10)node.y=800;
    }
    shoot.groups[2].y=736;shoot.groups[2].height=256;
    auto& vision=w.graphs[3];
    vision.nodes={{1,K::CaptureSource,0,0},{2,K::Inference,768,0},{3,K::DepthInference,0,192},{4,K::DepthToMask,256,192},
        {5,K::ApplyMask,512,0},{6,K::FilterConfidence,768,192},{7,K::LimitDetections,512,384},{8,K::FilterNms,256,384},
        {9,K::FilterCircle,0,384},{10,K::FilterMask,0,576},{11,K::VisionOutput,768,576},{12,K::CollectDataset,256,832},{13,K::RenderOverlay,512,832},
        {14,K::ScreenshotOutput,0,960},{15,K::KeyHeld,0,832}};
    vision.links={{1,0,5,0},{1,0,3,0},{3,0,4,0},{4,0,5,1},{5,0,2,0},{2,0,6,0},{6,0,7,0},{7,0,8,0},{8,0,9,0},
        {9,0,10,0},{4,0,10,1},{10,0,11,1},{1,0,12,1},{10,0,12,2},{1,0,13,1},{10,0,13,2},{4,0,13,3},{3,0,13,4},
        {15,0,14,0},{1,0,14,1}};
    vision.groups={{1,"Capture and inference",-32,-64,1024,384,0x40ffcd60u,{1,2,3,4,5,6}},
        {2,"Detection processing",-32,320,1024,416,0x40ff93adu,{7,8,9,10,11}},
        {3,"Dataset and visual outputs",224,768,640,320,0x40adde66u,{12,13}},
        {4,"Screenshots",-32,768,256,320,0x40ffcd60u,{14,15}}};
    // Historical layouts are retained only to recognize an existing starter
    // workspace during its one-time geometry migration.
    if(compact)
    {
    auto place=[](Graph& graph,int id,float x,float y){for(auto& node:graph.nodes)if(node.id==id){node.x=x;node.y=y;break;}};
    place(vision,1,0,0);place(vision,3,224,0);place(vision,4,448,0);place(vision,5,672,0);place(vision,2,896,0);
    place(vision,6,896,256);place(vision,7,672,256);place(vision,8,448,256);place(vision,9,224,256);place(vision,10,0,256);
    place(vision,11,0,512);place(vision,12,224,512);place(vision,13,448,512);place(vision,15,736,512);place(vision,14,960,512);
    vision.groups={{1,"Capture and inference",-32,-64,1152,256,0x40ffcd60u,{1,2,3,4,5}},
        {2,"Detection processing",-32,224,1152,192,0x40ff93adu,{6,7,8,9,10}},
        {3,"Vision, dataset and overlay",-32,448,704,288,0x40adde66u,{11,12,13}},
        {4,"Screenshots",704,448,480,288,0x40ffcd60u,{14,15}}};
    for(auto* graph:{&aim,&shoot})
    {
        graph->groups[0].x=-32;graph->groups[0].y=-64;graph->groups[0].width=480;graph->groups[0].height=352;
        graph->groups[1].x=480;graph->groups[1].y=-64;graph->groups[1].width=704;graph->groups[1].height=352;
        place(*graph,1,0,0);place(*graph,7,512,0);place(*graph,30,736,0);place(*graph,31,960,0);
    }
    place(aim,12,0,128);place(aim,13,224,0);place(aim,3,224,128);
    place(aim,4,960,128);place(aim,8,736,128);place(aim,2,512,128);
    place(aim,32,0,384);place(aim,5,224,384);place(aim,9,448,384);place(aim,10,672,384);place(aim,33,896,384);
    place(aim,6,0,576);place(aim,11,224,576);place(aim,35,672,576);place(aim,34,896,576);
    aim.groups[2].x=-32;aim.groups[2].y=320;aim.groups[2].width=1216;aim.groups[2].height=448;
    place(shoot,11,0,128);place(shoot,12,224,0);place(shoot,3,224,128);
    place(shoot,8,960,128);place(shoot,2,736,128);place(shoot,9,512,128);
    place(shoot,4,0,384);place(shoot,5,224,384);place(shoot,10,448,384);place(shoot,6,672,384);
    shoot.groups[2].x=-32;shoot.groups[2].y=320;shoot.groups[2].width=960;shoot.groups[2].height=224;
    }
    for(auto& graph:w.graphs)for(auto& node:graph.nodes)initializeParameters(node);
    for(auto& node:vision.nodes)if(node.kind==K::FilterCircle)node.parameters["enabled"]=0;
    for(auto& node:vision.nodes)if(node.id==15)node.parameters["virtual_key"]=0;
    // Expanded cards own their controls. Keep starter rows clear of the next
    // row while retaining the left-to-right flow and existing group structure.
    if(expanded)for(auto& graph:w.graphs)
    {
        for(auto& node:graph.nodes)node.y*=2;
        for(auto& group:graph.groups){group.y*=2;group.height*=2;}
    }
    return w;
}

static void readableStarterLayout(Graph& graph,int columns=4,bool stages=true)
{
    constexpr std::uint32_t blue=0x40ffcd60u,purple=0x40ff93adu,green=0x40adde66u;
    switch(graph.kind)
    {
    case GraphKind::Aim:
        graph.groups={{1,"01 Activation",0,0,0,0,blue,{1,12,13}},
            {2,"02 Target selection",0,0,0,0,purple,{7,30,31,4,8,2}},
            {3,"03 Prediction and speed",0,0,0,0,green,{32,5,9}},
            {4,"04 Mouse movement and output",0,0,0,0,blue,{35,34,3,10,33,11,6}}};break;
    case GraphKind::AutoShoot:
        graph.groups={{1,"01 Activation",0,0,0,0,blue,{1,11,12,3}},
            {2,"02 Detection filters",0,0,0,0,purple,{7,30,31}},
            {3,"03 Target and scope",0,0,0,0,green,{8,2,9}},
            {4,"04 Fire conditions and output",0,0,0,0,blue,{4,5,10,6}}};break;
    case GraphKind::Recoil:
        graph.groups={{1,"01 Shoot and zoom",0,0,0,0,blue,{1,2,4}},
            {2,"02 Recoil settings and output",0,0,0,0,green,{3,5,6,7}}};
        if(std::any_of(graph.nodes.begin(),graph.nodes.end(),[](const Node& node){return node.id==8&&node.kind==K::MouseCalibration;}))graph.groups.back().nodes.insert(graph.groups.back().nodes.end()-1,8);
        break;
    case GraphKind::Vision:
        graph.groups={{1,"01 Capture and depth",0,0,0,0,blue,{1,3,4}},
            {2,"02 Model inference",0,0,0,0,purple,{5,2}},
            {3,"03 Detection filters",0,0,0,0,green,{6,7,8,9,10}},
            {4,"04 Vision and visual outputs",0,0,0,0,blue,{11,12,13}},
            {5,"05 Screenshots",0,0,0,0,purple,{15,14}}};
        if(stages&&std::any_of(graph.nodes.begin(),graph.nodes.end(),[](const Node& node){return node.kind==K::OverlayOutput;}))
        {
            std::vector<int> overlay,dataset;
            for(const auto& node:graph.nodes)
            {
                if(isStyleNode(node.kind)||node.kind==K::OverlayOutput)overlay.push_back(node.id);
                if(node.kind==K::DatasetLabels||node.kind==K::DatasetOutput)dataset.push_back(node.id);
            }
            graph.groups={{1,"01 Capture and depth",0,0,0,0,blue,{1,3,4}},
                {2,"02 Model inference",0,0,0,0,purple,{5,2}},
                {3,"03 Filtered detections",0,0,0,0,green,{6,7,8,9,10,11}},
                {4,"04 Dataset recording",0,0,0,0,green,std::move(dataset)},
                {5,"05 Overlay layers",0,0,0,0,blue,std::move(overlay)},
                {6,"06 Screenshots",0,0,0,0,purple,{15,14}}};
        }
        break;
    }
    Layout::arrange(graph,columns);graph.panX=graph.panY=40;graph.zoom=1;
}
static bool compactOutputNodes(Graph& graph,bool separateGroups=false,int columns=4)
{
    bool changed=false;
    const auto pinFor=[](K kind,const char* key){
        const auto& pins=definition(kind).inputs;
        for(std::size_t p=0;p<pins.size();++p)if(pins[p].defaultParameter&&std::strcmp(pins[p].defaultParameter,key)==0)return static_cast<int>(p);
        return -1;
    };
    for(K legacy:{K::RenderOverlay,K::CollectDataset})
    {
        std::vector<int> originals;
        for(const auto& node:graph.nodes)if(node.kind==legacy)originals.push_back(node.id);
        for(int id:originals)
        {
            const std::vector<K> parts=legacy==K::RenderOverlay?
                std::vector<K>{K::OverlayBoxes,K::OverlayFrame,K::OverlayMotion,K::OverlayIcon,K::OverlayDepth,K::OverlayCorrection}:
                std::vector<K>{K::DatasetLabels};
            // A full custom graph keeps its compatible legacy node rather than
            // losing settings or connections to make room for the split.
            if(graph.nodes.size()+parts.size()>MaxNodes||graph.links.size()+parts.size()>MaxLinks)continue;
            auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& n){return n.id==id;});
            const Node original=*found;const K output=legacy==K::RenderOverlay?K::OverlayOutput:K::DatasetOutput;
            Node replacement=defaultNode(output,id,original.x,original.y);
            replacement.bypassed=original.bypassed;replacement.collapsed=original.collapsed;replacement.disabled=original.disabled;replacement.width=original.width;
            const auto copy=[&](Node& node){
                for(auto& entry:node.parameters)entry.second=parameter(original,entry.first);
                for(auto& entry:node.textParameters)entry.second=textParameter(original,entry.first);
            };
            copy(replacement);*found=std::move(replacement);
            std::vector<int> added;
            for(K kind:parts)
            {
                int next=1;while(std::any_of(graph.nodes.begin(),graph.nodes.end(),[&](const Node& n){return n.id==next;}))++next;
                Node part=defaultNode(kind,next);copy(part);graph.nodes.push_back(std::move(part));added.push_back(next);
            }
            for(auto& link:graph.links)if(link.toNode==id)
            {
                const auto& oldPins=definition(legacy).inputs;
                if(link.toPort<0||static_cast<std::size_t>(link.toPort)>=oldPins.size())continue;
                const auto* key=oldPins[link.toPort].defaultParameter;if(!key)continue;
                if(const int pin=pinFor(output,key);pin>=0)link.toPort=pin;
                else for(std::size_t p=0;p<parts.size();++p)if(const int pin=pinFor(parts[p],key);pin>=0)
                {link.toNode=added[p];link.toPort=pin;break;}
            }
            for(std::size_t p=1;p<added.size();++p)graph.links.push_back({added[p-1],0,added[p],0});
            graph.links.push_back({added.back(),0,id,legacy==K::RenderOverlay?5:3});
            bool grouped=false;
            for(auto& group:graph.groups)if(std::find(group.nodes.begin(),group.nodes.end(),id)!=group.nodes.end())
            {
                if(separateGroups&&graph.groups.size()<MaxGroups)group.nodes.erase(std::remove(group.nodes.begin(),group.nodes.end(),id),group.nodes.end());
                else{group.nodes.insert(group.nodes.end(),added.begin(),added.end());grouped=true;}
            }
            if(!grouped&&graph.groups.size()<MaxGroups)
            {
                int groupId=1;while(std::any_of(graph.groups.begin(),graph.groups.end(),[&](const Group& g){return g.id==groupId;}))++groupId;
                auto members=added;members.push_back(id);
                graph.groups.push_back({groupId,legacy==K::RenderOverlay?"Overlay layers":"Dataset recording",0,0,256,192,0x40adde66u,std::move(members)});
            }
            changed=true;
        }
    }
    if(changed)Layout::arrange(graph,columns);
    return changed;
}
static void processingStarterLayout(Graph& graph);
bool expandProcessingNodes(Graph& graph)
{
    bool changed=false;
    const bool standardGroups=std::any_of(graph.groups.begin(),graph.groups.end(),[&](const Group& group){return
        (graph.kind==GraphKind::Aim&&group.title=="03 Prediction and speed")||(graph.kind==GraphKind::AutoShoot&&group.title=="03 Target and scope");});
    const bool starter=standardGroups&&graph.nodes.size()==previousStarterGraphs().graphs[static_cast<std::size_t>(graph.kind)].nodes.size();
    const auto find=[&](int id)->Node*{for(auto& node:graph.nodes)if(node.id==id)return &node;return nullptr;};
    const auto pinFor=[](K kind,const std::string& key)->int{const auto& d=definition(kind);for(std::size_t p=0;p<d.inputs.size();++p)if(d.inputs[p].defaultParameter&&key==d.inputs[p].defaultParameter)return static_cast<int>(p);return -1;};
    for(const auto& link:graph.links){const auto* source=find(link.fromNode);const auto* target=find(link.toNode);if(!source||!target||link.fromPort<0||link.toPort<0||static_cast<std::size_t>(link.fromPort)>=definition(source->kind).outputs.size()||static_cast<std::size_t>(link.toPort)>=definition(target->kind).inputs.size())return false;}
    std::vector<int> ids;for(const auto& node:graph.nodes)ids.push_back(node.id);
    for(int id:ids)
    {
        const Node old=*find(id);if(old.bypassed)continue;
        const auto& def=definition(old.kind);
        if(old.kind==K::Target||old.kind==K::TargetNearest||old.kind==K::TargetTracker)
        {
            bool simple=true;
            for(const char* key:{"legacy_offsets","filter_classes","prefer_head","merge_head_body","disable_headshot","legacy_camera_compensation"})simple&=parameter(old,key)==0;
            for(const auto& link:graph.links)if(link.toNode==id&&link.toPort>=2)
            {
                const std::string key=def.inputs[link.toPort].defaultParameter?def.inputs[link.toPort].defaultParameter:"";
                simple&=key=="class_player"||key=="class_head"||key=="max_missed_frames";
            }
            if(!simple)continue;
            const K kind=parameter(old,"mode")==2?K::TrackDetections:K::ChooseTarget;
            Node replacement=defaultNode(kind,id,old.x,old.y);replacement.disabled=old.disabled;replacement.collapsed=old.collapsed;
            for(const auto& spec:definition(kind).parameters)replacement.parameters[spec.key]=parameter(old,spec.key);
            for(auto it=graph.links.begin();it!=graph.links.end();)
            {
                if(it->toNode!=id){++it;continue;}
                if(it->toPort==0||(it->toPort==1&&kind==K::TrackDetections)){++it;continue;}
                const auto* key=def.inputs[it->toPort].defaultParameter;const int port=key?pinFor(kind,key):-1;
                if(port>=0){it->toPort=port;++it;}else it=graph.links.erase(it);
            }
            if(kind==K::TrackDetections&&old.inputValues.count(1))replacement.inputValues[1]=old.inputValues.at(1);
            *find(id)=std::move(replacement);changed=true;continue;
        }
    }
    for(int id:ids)
    {
        const Node old=*find(id);if(old.bypassed)continue;
        const auto& def=definition(old.kind);
        std::vector<K> kinds;
        int observedSource=0;
        if(old.kind==K::Prediction)kinds={K::MotionEstimate,K::MotionPosition,K::MotionVelocity,K::MotionDamping,K::MotionReady,K::PredictionTime,K::ProjectPoint};
        else if(old.kind==K::AimPoint)
        {
            const auto input=std::find_if(graph.links.begin(),graph.links.end(),[&](const Link& link){return link.toNode==id&&link.toPort==0;});
            const auto* source=input==graph.links.end()?nullptr:find(input->fromNode);
            if(!source||(source->kind!=K::TrackDetections&&source->kind!=K::ChooseTarget))continue;
            kinds={K::TargetPosition,K::TargetSize,K::TargetClass,K::TargetAnchor};
        }
        else if(old.kind==K::ScopeCheck)
        {
            const auto source=std::find_if(graph.links.begin(),graph.links.end(),[&](const Link& link){return link.toNode==id&&link.toPort==0;});
            if(source!=graph.links.end())for(const auto& candidate:graph.nodes)if(candidate.kind==K::TargetObserved&&!candidate.disabled&&!candidate.bypassed&&
                std::any_of(graph.links.begin(),graph.links.end(),[&](const Link& link){return link.toNode==candidate.id&&link.toPort==0&&link.fromNode==source->fromNode&&link.fromPort==source->fromPort;})){observedSource=candidate.id;break;}
            kinds={K::TargetPosition,K::TargetSize,K::BoxContainsPoint};if(!observedSource)kinds.push_back(K::TargetObserved);
        }
        else continue;
        const std::size_t extraLinks=old.kind==K::Prediction?12:6;
        if(graph.nodes.size()+kinds.size()>MaxNodes||graph.links.size()+extraLinks>MaxLinks)continue;
        std::vector<Node> additions;
        int next=1;
        for(K kind:kinds)
        {
            while(find(next)||std::any_of(additions.begin(),additions.end(),[&](const Node& node){return node.id==next;}))++next;
            auto node=defaultNode(kind,next++,old.x,old.y);
            for(const auto& spec:definition(kind).parameters)if(std::any_of(def.parameters.begin(),def.parameters.end(),[&](const ParameterSpec& s){return std::strcmp(s.key,spec.key)==0;}))node.parameters[spec.key]=parameter(old,spec.key);
            additions.push_back(std::move(node));
        }
        auto replacement=defaultNode(old.kind==K::Prediction?K::FutureTrail:old.kind==K::AimPoint?K::BoxPoint:K::SelectBoolean,id,old.x,old.y);
        if(old.kind==K::ScopeCheck)replacement.inputValues[2]=0;
        replacement.disabled=old.disabled;replacement.collapsed=old.collapsed;
        if(old.kind==K::Prediction)for(const char* key:{"prediction_futurePositions","draw_futurePositions"})replacement.parameters[key]=parameter(old,key);
        const auto destination=[&](int port)->std::pair<int,int>{
            if(old.kind==K::Prediction)
            {
                if(port==0)return {additions[0].id,0};if(port==2)return {additions[0].id,1};
                if(port==1)return {additions[5].id,0};
                const auto* key=def.inputs[port].defaultParameter;
                if(key)for(const auto& node:additions){const int pin=pinFor(node.kind,key);if(pin>=0)return {node.id,pin};}
                if(key){const int pin=pinFor(replacement.kind,key);if(pin>=0)return {id,pin};}
            }
            else if(old.kind==K::AimPoint)
            {
                if(port==0)return {additions[0].id,0};
                const auto* key=def.inputs[port].defaultParameter;if(key)return {additions[3].id,pinFor(K::TargetAnchor,key)};
            }
            else
            {if(port==0)return {additions[0].id,0};return {additions[2].id,port==2?2:3};}
            return {-1,-1};
        };
        bool supported=true;for(const auto& link:graph.links)if(link.toNode==id){const auto to=destination(link.toPort);supported&=to.first>=0&&to.second>=0;}
        if(!supported)continue;
        std::vector<Link> inputs;
        for(auto& link:graph.links)if(link.toNode==id){inputs.push_back(link);const auto to=destination(link.toPort);link.toNode=to.first;link.toPort=to.second;}
        for(const auto& local:old.inputValues)
        {
            const auto to=destination(local.first);for(auto& node:additions)if(node.id==to.first)node.inputValues[to.second]=local.second;
        }
        if(old.kind==K::Prediction)
        {
            for(int i=1;i<=4;++i)graph.links.push_back({additions[0].id,0,additions[i].id,0});
            graph.links.insert(graph.links.end(),{{additions[1].id,0,additions[6].id,0},{additions[2].id,0,additions[6].id,1},
                {additions[5].id,0,additions[6].id,2},{additions[3].id,0,additions[6].id,3},{additions[4].id,0,additions[6].id,4},
                {additions[6].id,0,id,0},{additions[0].id,0,id,1},{additions[5].id,0,id,2}});
        }
        else
        {
            for(const auto& link:inputs)if(link.toPort==0)for(int i:old.kind==K::AimPoint?std::vector<int>{1,2}:std::vector<int>{1,3})
                {if(static_cast<std::size_t>(i)>=additions.size())continue;auto copy=link;copy.toNode=additions[i].id;copy.toPort=0;graph.links.push_back(std::move(copy));}
            if(old.kind==K::AimPoint)graph.links.insert(graph.links.end(),{{additions[2].id,0,additions[3].id,0},{additions[0].id,0,id,0},{additions[1].id,0,id,1},{additions[3].id,0,id,2}});
            else graph.links.insert(graph.links.end(),{{additions[0].id,0,additions[2].id,0},{additions[1].id,0,additions[2].id,1},{observedSource?observedSource:additions[3].id,0,id,0},{additions[2].id,0,id,1}});
        }
        for(auto& group:graph.groups)if(std::find(group.nodes.begin(),group.nodes.end(),id)!=group.nodes.end())for(const auto& node:additions)group.nodes.push_back(node.id);
        *find(id)=std::move(replacement);graph.nodes.insert(graph.nodes.end(),additions.begin(),additions.end());changed=true;
    }
    if(changed)
    {
        const auto* output=find(graph.kind==GraphKind::Aim?5:9);
        const auto* geometry=find(8);
        if(starter&&output&&geometry&&((graph.kind==GraphKind::Aim&&output->kind==K::FutureTrail&&geometry->kind==K::BoxPoint)||
            (graph.kind==GraphKind::AutoShoot&&output->kind==K::SelectBoolean)))processingStarterLayout(graph);
        else Layout::arrange(graph);
    }
    return changed;
}
static void processingStarterLayout(Graph& graph)
{
    constexpr std::uint32_t blue=0x40ffcd60u,purple=0x40ff93adu,green=0x40adde66u;
    if(graph.kind==GraphKind::Aim)
    {
        graph.groups={{1,"01 Activation",0,0,0,0,blue,{1,12,13}},
            {2,"02 Select a target",0,0,0,0,purple,{7,30,31,4,2}},
            {3,"03 Aim geometry",0,0,0,0,blue,{8}},
            {4,"04 Compensate and filter",0,0,0,0,green,{32}},
            {5,"05 Position + velocity + time",0,0,0,0,purple,{}},
            {6,"06 Speed and mouse output",0,0,0,0,blue,{9,35,34,3,10,33,11,6}}};
        for(const auto& node:graph.nodes)
        {
            if(node.kind==K::TargetPosition||node.kind==K::TargetSize||node.kind==K::TargetClass||node.kind==K::TargetAnchor)graph.groups[2].nodes.push_back(node.id);
            if(node.kind==K::MotionEstimate)graph.groups[3].nodes.push_back(node.id);
            if((node.kind>=K::MotionPosition&&node.kind<=K::MotionReady)||node.kind==K::PredictionTime||node.kind==K::ProjectPoint||node.kind==K::FutureTrail)graph.groups[4].nodes.push_back(node.id);
        }
    }
    else if(graph.kind==GraphKind::AutoShoot)
    {
        graph.groups={{1,"01 Activation",0,0,0,0,blue,{1,11,12,3}},
            {2,"02 Detection filters",0,0,0,0,purple,{7,30,31}},
            {3,"03 Select and observe",0,0,0,0,green,{8,2}},
            {4,"04 Scope geometry",0,0,0,0,blue,{9}},
            {5,"05 Fire gate and output",0,0,0,0,purple,{4,5,10,6}}};
        for(const auto& node:graph.nodes)if(node.kind==K::TargetPosition||node.kind==K::TargetSize||node.kind==K::BoxContainsPoint||(node.kind==K::TargetObserved&&node.id!=2))graph.groups[3].nodes.push_back(node.id);
    }
    const auto bridge=[&](int source,std::size_t group,K kind){
        std::vector<std::size_t> branches;
        for(std::size_t link=0;link<graph.links.size();++link)if(graph.links[link].fromNode==source&&std::find(graph.groups[group].nodes.begin(),graph.groups[group].nodes.end(),graph.links[link].toNode)!=graph.groups[group].nodes.end())branches.push_back(link);
        if(branches.size()<2||graph.nodes.size()>=MaxNodes||graph.links.size()>=MaxLinks)return;
        int id=1;while(std::any_of(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==id;}))++id;
        graph.nodes.push_back(defaultNode(kind,id));graph.groups[group].nodes.push_back(id);
        for(auto link:branches)graph.links[link].fromNode=id;
        graph.links.push_back({source,0,id,0});
    };
    if(graph.kind==GraphKind::Aim)
    {
        int motion=0;for(const auto& node:graph.nodes)if(node.kind==K::MotionEstimate)motion=node.id;
        bridge(4,2,K::RerouteTarget);bridge(motion,4,K::RerouteMotion);bridge(5,5,K::ReroutePoint);
    }
    else if(graph.kind==GraphKind::AutoShoot)bridge(8,3,K::RerouteTarget);
    Layout::arrange(graph,4);graph.zoom=1;graph.panX=graph.panY=40;
}
bool shareMouseCalibration(Workspace& workspace)
{
    for(const auto& graph:workspace.graphs)for(const auto& node:graph.nodes)if(node.kind==K::MouseProfile)return false;
    std::set<int> workflows;int nextWorkflow=1;
    for(std::size_t g=0;g<workspace.graphs.size();++g){const int id=workflowId(workspace.graphs[g],g);workflows.insert(id);nextWorkflow=std::max(nextWorkflow,id+1);}
    if(workflows.size()>=MaxWorkflows)return false;
    const auto profileKey=[](const Node& node){std::ostringstream key;key<<std::setprecision(17)<<textParameter(node,"profile_name");for(const auto& spec:definition(K::MouseProfile).parameters)key<<'|'<<parameter(node,spec.key);return key.str();};
    std::string active;std::vector<Node> profiles;
    for(const auto& graph:workspace.graphs)for(const auto& node:graph.nodes)
        if(node.kind==K::MouseDelta||node.kind==K::MouseCalibration||node.kind==K::RecoilProfileOutput)
        {
            const auto& def=definition(node.kind);
            for(const auto& link:graph.links)if(link.toNode==node.id&&link.toPort>=0&&static_cast<std::size_t>(link.toPort)<def.inputs.size())
                if(const char* key=def.inputs[link.toPort].defaultParameter)
                    for(const auto& spec:definition(K::MouseProfile).parameters)if(std::strcmp(key,spec.key)==0)return false;
            if(node.kind==K::MouseDelta&&graph.active&&active.empty())active=profileKey(node);
            if(std::none_of(profiles.begin(),profiles.end(),[&](const Node& other){return profileKey(other)==profileKey(node);}))profiles.push_back(node);
        }
    if(profiles.empty()||workspace.graphs.size()+profiles.size()>MaxGraphs)return false;
    if(active.empty())active=profileKey(profiles.front());
    Workspace candidate=workspace;
    const auto pin=[](K kind,const char* key){const auto& inputs=definition(kind).inputs;for(std::size_t p=0;p<inputs.size();++p)if(inputs[p].defaultParameter&&std::strcmp(inputs[p].defaultParameter,key)==0)return static_cast<int>(p);return -1;};
    for(auto& graph:candidate.graphs)
    {
        std::vector<int> deltas,scales,recoils;
        for(const auto& node:graph.nodes){if(node.kind==K::MouseDelta)deltas.push_back(node.id);if(node.kind==K::MouseCalibration)scales.push_back(node.id);if(node.kind==K::RecoilProfileOutput)recoils.push_back(node.id);}
        const auto nextId=[&](){int id=1;while(std::any_of(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==id;}))++id;return id;};
        for(int id:deltas)
        {
            auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==id;});const Node old=*source;
            bool fps=parameter(old,"legacy_fps")!=0;
            for(const auto& link:graph.links)fps|=link.toNode==id&&link.toPort==pin(K::MouseDelta,"legacy_fps");
            const std::size_t added=fps?5:3;
            if(old.bypassed||graph.nodes.size()+added>MaxNodes||graph.links.size()+added>MaxLinks)
            {
                auto replacement=defaultNode(K::SharedMouseDelta,id,old.x,old.y);replacement.disabled=old.disabled;replacement.collapsed=old.collapsed;
                replacement.parameters["legacy_center"]=parameter(old,"legacy_center");replacement.parameters["legacy_fps"]=parameter(old,"legacy_fps");replacement.inputValues=old.inputValues;
                for(auto& link:graph.links)if(link.toNode==id&&link.toPort>=3)
                {const auto* key=definition(K::MouseDelta).inputs[link.toPort].defaultParameter;link.toPort=pin(K::SharedMouseDelta,key);}
                *source=std::move(replacement);continue;
            }
            const int delta=nextId();graph.nodes.push_back(defaultNode(K::PointDelta,delta));
            const int convert=nextId();graph.nodes.push_back(defaultNode(K::PixelsToCounts,convert));
            const int shared=nextId();graph.nodes.push_back(defaultNode(K::SharedCalibration,shared));
            for(auto& node:graph.nodes)if(node.id==delta)node.parameters["implicit_center"]=parameter(old,"legacy_center");
            int normalize=0,frameRate=0;
            if(fps)
            {
                normalize=nextId();graph.nodes.push_back(defaultNode(K::NormalizeFps,normalize));
                frameRate=nextId();graph.nodes.push_back(defaultNode(K::CaptureFps,frameRate));
                for(auto& node:graph.nodes)if(node.id==normalize){node.parameters["enabled"]=parameter(old,"legacy_fps");node.parameters["preserve_fraction"]=1;}
                for(auto& link:graph.links)if(link.fromNode==id)link.fromNode=normalize;
            }
            for(auto& link:graph.links)if(link.toNode==id)
            {
                if(link.toPort==0){link.toNode=delta;link.toPort=0;}
                else if(link.toPort==2){link.toNode=delta;link.toPort=1;}
                else if(link.toPort>=3)
                {
                    const auto* key=definition(K::MouseDelta).inputs[link.toPort].defaultParameter;
                    if(std::strcmp(key,"legacy_center")==0){link.toNode=delta;link.toPort=pin(K::PointDelta,"implicit_center");}
                    else{link.toNode=normalize;link.toPort=pin(K::NormalizeFps,"enabled");}
                }
            }
            auto replacement=defaultNode(K::ApplyMouseSpeed,id,old.x,old.y);replacement.disabled=old.disabled;replacement.collapsed=old.collapsed;
            replacement.inputValues[1]=old.inputValues.count(1)?old.inputValues.at(1):0;
            for(auto& node:graph.nodes)if(node.id==id)node=std::move(replacement);
            graph.links.insert(graph.links.end(),{{delta,0,convert,0},{shared,0,convert,1},{convert,0,id,0}});
            if(fps)graph.links.insert(graph.links.end(),{{id,0,normalize,0},{frameRate,0,normalize,1}});
            for(auto& group:graph.groups)if(std::find(group.nodes.begin(),group.nodes.end(),id)!=group.nodes.end())
            {group.nodes.insert(group.nodes.end(),{delta,convert,shared});if(fps)group.nodes.insert(group.nodes.end(),{normalize,frameRate});}
            if(graph.kind==GraphKind::Aim)
            {
                auto stage=std::find_if(graph.groups.begin(),graph.groups.end(),[](const Group& group){return group.title=="06 Speed and mouse output";});
                if(stage!=graph.groups.end())
                {
                    std::vector<int> geometry{delta,convert,shared};
                    for(int member:stage->nodes)for(const auto& node:graph.nodes)if(node.id==member&&(node.kind==K::Center||node.kind==K::ReroutePoint))geometry.push_back(member);
                    for(int member:geometry)stage->nodes.erase(std::remove(stage->nodes.begin(),stage->nodes.end(),member),stage->nodes.end());
                    stage->title="07 Speed, FPS and mouse output";
                    int groupId=1;for(const auto& group:graph.groups)groupId=std::max(groupId,group.id+1);
                    graph.groups.push_back({groupId,"06 Point to device counts",0,0,0,0,0x40ff93adu,std::move(geometry)});
                }
            }
        }
        for(int id:scales)
        {
            auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==id;});const Node old=*source;
            auto replacement=defaultNode(graph.nodes.size()<MaxNodes&&graph.links.size()<MaxLinks?K::CalibrationScale:K::SharedCalibrationScale,id,old.x,old.y);
            replacement.disabled=old.disabled;replacement.collapsed=old.collapsed;*source=replacement;
            if(replacement.kind==K::CalibrationScale)
            {
                const int reader=nextId();graph.nodes.push_back(defaultNode(K::SharedCalibration,reader));graph.links.push_back({reader,0,id,0});
                for(auto& group:graph.groups)if(std::find(group.nodes.begin(),group.nodes.end(),id)!=group.nodes.end())group.nodes.push_back(reader);
            }
        }
        for(int id:recoils)for(auto& node:graph.nodes)if(node.id==id)
        {auto replacement=defaultNode(K::SharedRecoilOutput,id,node.x,node.y);replacement.disabled=node.disabled;replacement.collapsed=node.collapsed;replacement.inputValues=node.inputValues;node=std::move(replacement);}
        if(!deltas.empty()||!scales.empty()||!recoils.empty())Layout::arrange(graph);
    }
    std::set<std::string> titles;std::string workflowName="Profiles";
    for(int suffix=2;std::any_of(candidate.graphs.begin(),candidate.graphs.end(),[&](const Graph& graph){return workflowTitle(graph)==workflowName;});++suffix)workflowName="Profiles "+std::to_string(suffix);
    for(const auto& old:profiles)
    {
        Graph graph;graph.kind=GraphKind::Aim;graph.workflow=nextWorkflow;graph.workflowName=workflowName;graph.workflowIcon=8;graph.active=profileKey(old)==active;
        const auto title=textParameter(old,"profile_name");std::size_t length=std::min<std::size_t>(90,title.size());while(length<title.size()&&length&&(static_cast<unsigned char>(title[length])&0xc0)==0x80)--length;
        const auto label=title.empty()?"Profile":title.substr(0,length);graph.pageName=label;
        for(int suffix=2;!titles.insert(graph.pageName).second;++suffix)graph.pageName=label+" "+std::to_string(suffix);
        auto profile=defaultNode(K::MouseProfile,1);
        for(const auto& spec:definition(K::MouseProfile).parameters)profile.parameters[spec.key]=parameter(old,spec.key);
        profile.textParameters["profile_name"]=title;graph.nodes.push_back(std::move(profile));
        graph.groups={{1,"Shared aiming and recoil calibration",0,0,0,0,0x40ffcd60u,{1}}};Layout::arrange(graph);candidate.graphs.push_back(std::move(graph));
    }
    migrateWorkflowPages(candidate);
    std::string error;if(!validate(candidate,error))return false;
    workspace=std::move(candidate);return true;
}
bool migrateWorkflowPages(Workspace& workspace)
{
    bool changed=false;
    for(std::size_t i=0;i<workspace.graphs.size();++i)if(!workspace.graphs[i].workflow){workspace.graphs[i].workflow=static_cast<int>(i)+1;changed=true;}
    // Only the generated container is folded away. User graphs with a similar
    // title and no generated frame remain ordinary user-owned workflows.
    const auto generated=[](const Graph& graph){
        const auto title=workflowTitle(graph);
        return (title=="Profiles"||title=="Mouse profiles"||title.rfind("Profiles ",0)==0)&&
            std::any_of(graph.groups.begin(),graph.groups.end(),[](const Group& group){return group.title=="Shared aiming and recoil calibration";});
    };
    std::set<int> containers;
    for(std::size_t i=0;i<workspace.graphs.size();++i)if(generated(workspace.graphs[i]))containers.insert(workflowId(workspace.graphs[i],i));
    for(std::size_t source=workspace.graphs.size();source-->0;)
    {
        const auto& calibration=workspace.graphs[source];
        if(!containers.count(workflowId(calibration,source))||!calibration.active)continue;
        for(std::size_t target=0;target<workspace.graphs.size();++target)
        {
            const auto& host=workspace.graphs[target];
            if(target==source||host.kind==GraphKind::Vision||!host.active||!host.enabled||containers.count(workflowId(host,target))||
                host.nodes.size()+calibration.nodes.size()>MaxNodes||host.links.size()+calibration.links.size()>MaxLinks||
                host.groups.size()+calibration.groups.size()>MaxGroups||host.ties.size()+calibration.ties.size()>MaxWireTies)continue;
            Graph merged=host;
            std::map<int,int> nodes,ties;
            int nodeId=1,tieId=1,groupId=1;
            for(const auto& node:host.nodes)nodeId=std::max(nodeId,node.id+1);
            for(const auto& tie:host.ties)tieId=std::max(tieId,tie.id+1);
            for(const auto& group:host.groups)groupId=std::max(groupId,group.id+1);
            float right=0,left=0,top=0;
            for(const auto& node:host.nodes)right=std::max(right,node.x+Layout::width(node));
            for(const auto& node:calibration.nodes){left=std::min(left,node.x);top=std::min(top,node.y);}
            const float dx=Layout::up(right+128)-left,dy=64-top;
            std::set<std::string> localNames;std::map<std::string,std::string> renamed;
            for(const auto& node:host.nodes)if(variableGet(node.kind)||variableSet(node.kind))localNames.insert(textParameter(node,"name"));
            for(const auto& node:calibration.nodes)if(variableGet(node.kind)||variableSet(node.kind))
            {
                const auto original=textParameter(node,"name");if(renamed.count(original))continue;
                auto name=original;for(int suffix=2;localNames.count(name);++suffix)name=original.substr(0,110)+" "+std::to_string(suffix);
                localNames.insert(name);renamed[original]=name;
            }
            for(auto node:calibration.nodes)
            {
                nodes[node.id]=nodeId;node.id=nodeId++;node.x+=dx;node.y+=dy;node.disabled|=!calibration.enabled;
                if(variableGet(node.kind)||variableSet(node.kind))node.textParameters["name"]=renamed.at(textParameter(node,"name"));
                merged.nodes.push_back(std::move(node));
            }
            for(auto tie:calibration.ties){ties[tie.id]=tieId;tie.id=tieId++;tie.x+=dx;tie.y+=dy;merged.ties.push_back(tie);}
            for(auto link:calibration.links)
            {link.fromNode=nodes.at(link.fromNode);link.toNode=nodes.at(link.toNode);for(auto& tie:link.ties)tie=ties.at(tie);merged.links.push_back(std::move(link));}
            for(auto group:calibration.groups)
            {group.id=groupId++;group.x+=dx;group.y+=dy;for(auto& member:group.nodes)member=nodes.at(member);merged.groups.push_back(std::move(group));}
            CompiledGraph check;std::string error;if(!compileGraph(merged,check,error))continue;
            workspace.graphs[target]=std::move(merged);workspace.graphs.erase(workspace.graphs.begin()+source);changed=true;break;
        }
    }
    std::map<int,int> active;int next=1;
    for(std::size_t i=0;i<workspace.graphs.size();++i)
    {const int id=workflowId(workspace.graphs[i],i);next=std::max(next,id+1);if(workspace.graphs[i].active)active[id]=static_cast<int>(i);}
    std::set<int> usedIds;std::set<std::string> usedNames;
    for(std::size_t i=0;i<workspace.graphs.size();++i)
    {
        auto& graph=workspace.graphs[i];const int old=workflowId(graph,i);const bool container=containers.count(old)!=0;
        const bool additional=!graph.active||(active.count(old)&&active.at(old)!=static_cast<int>(i));
        std::string title=container?"Calibration":workflowTitle(graph);
        if(additional)title+=(container?" ":" / ")+pageTitle(graph);
        if(title.size()>90){std::size_t length=90;while(length&&(static_cast<unsigned char>(title[length])&0xc0)==0x80)--length;title.resize(length);}
        const auto base=title;for(int suffix=2;usedNames.count(title);++suffix)title=base+" "+std::to_string(suffix);
        usedNames.insert(title);
        const auto freeId=[&](){while(next>1000000||active.count(next)||usedIds.count(next)){if(next>1000000)next=1;else ++next;}return next++;};
        int id=additional?freeId():old;if(!usedIds.insert(id).second){id=freeId();usedIds.insert(id);}
        changed|=graph.workflow!=id||graph.workflowName!=title||!graph.pageName.empty()||!graph.active;
        graph.workflow=id;graph.workflowName=title;graph.pageName.clear();graph.enabled=graph.enabled&&graph.active;graph.active=true;
    }
    return changed;
}
#include "nodes/starter_layouts.inl"

static Workspace generatedStarterGraphs()
{
    auto workspace=previousStarterGraphs();
    workspace.graphs[2].nodes.push_back(defaultNode(K::MouseCalibration,8));
    workspace.graphs[2].links.push_back({8,0,7,2});
    for(auto& graph:workspace.graphs)readableStarterLayout(graph);
    compactOutputNodes(workspace.graphs[3],true);
    readableStarterLayout(workspace.graphs[3]);
    for(int index:{0,1})expandProcessingNodes(workspace.graphs[index]);
    shareMouseCalibration(workspace);
    migrateWorkflowPages(workspace);
    auto& aim=workspace.graphs[0];
    const auto source=std::find_if(aim.groups.begin(),aim.groups.end(),[](const Group& group){return group.title=="Shared aiming and recoil calibration";});
    const auto stage=std::find_if(aim.groups.begin(),aim.groups.end(),[](const Group& group){return group.title=="06 Point to device counts";});
    if(source!=aim.groups.end()&&stage!=aim.groups.end()){stage->nodes.insert(stage->nodes.end(),source->nodes.begin(),source->nodes.end());aim.groups.erase(source);}
    Layout::arrange(aim);
    return workspace;
}
Workspace defaults()
{
    auto workspace=generatedStarterGraphs();
    for(auto& graph:workspace.graphs)applyStarterLayout(graph);
    return workspace;
}
bool migrateStarterLayout(Graph& graph)
{
    if(std::any_of(graph.nodes.begin(),graph.nodes.end(),[](const Node& node){return node.width!=0||node.height!=0;}))return false;
    const auto sameGroups=[](const std::vector<Group>& a,const std::vector<Group>& b){
        if(a.size()!=b.size())return false;
        for(const auto& expected:a)
        {
            const auto found=std::find_if(b.begin(),b.end(),[&](const Group& group){return group.id==expected.id;});
            if(found==b.end()||found->title!=expected.title||found->x!=expected.x||found->y!=expected.y||
                found->width!=expected.width||found->height!=expected.height||found->color!=expected.color||found->nodes!=expected.nodes)return false;
        }
        return true;
    };
    const auto matches=[&](const Graph& old){
        if(graph.nodes.size()!=old.nodes.size()||!sameGroups(graph.groups,old.groups))return false;
        for(const auto& expected:old.nodes)
        {
            const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return node.id==expected.id;});
            if(found==graph.nodes.end()||found->x!=expected.x||found->y!=expected.y)return false;
            const bool selection=(found->kind==K::TargetNearest||found->kind==K::TargetTracker)&&(expected.kind==K::TargetNearest||expected.kind==K::TargetTracker);
            if(found->kind!=expected.kind&&!selection)return false;
        }
        return true;
    };
    const auto update=[&]{
        auto arranged=graph;readableStarterLayout(arranged);
        bool changed=!sameGroups(graph.groups,arranged.groups);
        for(std::size_t n=0;n<graph.nodes.size();++n)changed|=graph.nodes[n].x!=arranged.nodes[n].x||graph.nodes[n].y!=arranged.nodes[n].y;
        if(changed){arranged.panX=graph.panX;arranged.panY=graph.panY;arranged.zoom=graph.zoom;graph=std::move(arranged);}
        return changed;
    };
    // Recognize shipped geometry, including the first split-output layout.
    // Matching positions and groups leaves custom arrangements untouched.
    static const auto shipped=[] {
        std::vector<Workspace> layouts;
        for(int columns:{6,4})
        {
            auto original=previousStarterGraphs();
            for(auto& graph:original.graphs)readableStarterLayout(graph,columns,false);
            layouts.push_back(original);
            auto merged=original;compactOutputNodes(merged.graphs[3],false,columns);layouts.push_back(std::move(merged));
            compactOutputNodes(original.graphs[3],true,columns);layouts.push_back(std::move(original));
        }
        return layouts;
    }();
    for(const auto& old:shipped)if(matches(old.graphs[static_cast<std::size_t>(graph.kind)]))return update();
    for(bool compact:{false,true})for(bool expanded:{false,true})
    {
        const auto old=previousStarterGraphs(compact,expanded).graphs[static_cast<std::size_t>(graph.kind)];
        if(matches(old))return update();
    }
    return false;
}

int workflowId(const Graph& graph,std::size_t index){return graph.workflow>0?graph.workflow:static_cast<int>(index)+1;}
std::string workflowTitle(const Graph& graph){return graph.workflowName.empty()?(graph.kind==GraphKind::Aim?"Aim Graph":graph.kind==GraphKind::AutoShoot?"Auto Shoot":graph.kind==GraphKind::Recoil?"Recoil":"Vision"):graph.workflowName;}
std::string pageTitle(const Graph& graph){return graph.pageName.empty()?"Default":graph.pageName;}
int activePage(const Workspace& workspace,int workflow)
{
    for(std::size_t i=0;i<workspace.graphs.size();++i)if(workflowId(workspace.graphs[i],i)==workflow&&workspace.graphs[i].active)return static_cast<int>(i);
    return -1;
}
int previewKey(int graph,int node){return graph==3?node:(graph+1)*NodeKeyStride+node;}
namespace
{
bool compileWorkspace(const Workspace& workspace, std::string& error,std::vector<CompiledGraph>* output)
{
    error.clear();
    if(workspace.graphs.size()>MaxGraphs){error="Workspace supports at most "+std::to_string(MaxGraphs)+" workflows.";return false;}
    if(output){output->clear();output->reserve(workspace.graphs.size());}
    struct WorkflowInfo{GraphKind kind;std::string name;int icon;bool enabled;int active=0;std::set<std::string> pages;};
    std::map<int,WorkflowInfo> workflows;std::vector<CompiledNode> captures;bool sharedFrame=false;
    for (std::size_t i = 0; i < workspace.graphs.size(); ++i)
    {
        const auto& graph=workspace.graphs[i];
        if(static_cast<int>(graph.kind)<0||static_cast<int>(graph.kind)>3)
        {error="Invalid workflow domain.";return false;}
        const auto validName=[](const std::string& name){return name.size()<=96&&name.find_first_of(std::string("\0\r\n",3))==std::string::npos;};
        if(graph.workflow<0||graph.workflow>1000000||graph.workflowIcon< -1||graph.workflowIcon>15||!validName(graph.workflowName)||!validName(graph.pageName))
        {error="Invalid workflow or page name.";return false;}
        const auto id=workflowId(graph,i);auto found=workflows.try_emplace(id,WorkflowInfo{graph.kind,workflowTitle(graph),graph.workflowIcon,graph.enabled}).first;
        auto& workflow=found->second;
        if(workspace.sourceVersion>=20&&(!graph.active||!graph.pageName.empty()||!workflow.pages.empty()))
        {error="Each workflow must contain one graph. Import a legacy file to migrate its pages.";return false;}
        if(workflow.kind!=graph.kind||workflow.name!=workflowTitle(graph)||workflow.icon!=graph.workflowIcon||workflow.enabled!=graph.enabled||!workflow.pages.insert(pageTitle(graph)).second)
        {error="Workflow pages require a shared type/name and unique page names.";return false;}
        workflow.active+=graph.active;
        CompiledGraph compiled;
        if(!compileGraph(graph,compiled,error)){error=workflowTitle(graph)+" / "+pageTitle(graph)+": "+error;return false;}
        if(graph.active&&graph.enabled&&graph.kind==GraphKind::Vision)for(const auto& node:compiled.nodes)
        {
            sharedFrame|=node.potentiallyActive&&node.node.kind==K::CurrentFrame&&!node.node.disabled&&!node.node.bypassed;
            if(node.node.kind==K::CaptureSource&&!node.node.disabled&&!node.node.bypassed)captures.push_back(node);
        }
        if(output)output->push_back(std::move(compiled));
    }
    if(workflows.size()>MaxWorkflows){error="Workspace supports at most "+std::to_string(MaxWorkflows)+" workflows.";return false;}
    for(const auto& workflow:workflows)if(workflow.second.active!=1){error="Each workflow requires exactly one active page.";return false;}
    const auto captureSources=std::count_if(captures.begin(),captures.end(),[&](const CompiledNode& node){
        return (node.potentiallyActive||sharedFrame)&&providerMayBeEnabled(node);
    });
    if(captureSources>1){error="Only one reachable Capture source is supported across active workflows.";return false;}
    return true;
}
}
bool validate(const Workspace& workspace, std::string& error){return compileWorkspace(workspace,error,nullptr);}
bool canConnect(const Graph& graph, const Link& link, std::string& error)
{
    Graph draft = graph;
    draft.links.erase(std::remove_if(draft.links.begin(), draft.links.end(), [&](const Link& l) {
        return l.toNode == link.toNode && l.toPort == link.toPort;
    }), draft.links.end());
    draft.links.push_back(link);
    CompiledGraph compiled;
    error.clear();
    return compileGraph(draft, compiled, error);
}

void removeNodes(Graph& graph,const std::vector<int>& ids)
{
    if(ids.empty())return;
    const std::unordered_set<int> removed(ids.begin(),ids.end());
    std::unordered_map<int,const Node*> nodes;
    std::unordered_map<int,const Link*> incoming;
    for(const auto& node:graph.nodes)nodes.emplace(node.id,&node);
    for(const auto& link:graph.links)if(link.toPort==0)incoming.emplace(link.toNode,&link);
    std::vector<Link> retained;retained.reserve(graph.links.size());
    for(const auto& outgoing:graph.links)
    {
        if(removed.count(outgoing.toNode))continue;
        Link link=outgoing;std::vector<const Link*> route{&outgoing};std::unordered_set<int> visited;
        bool connected=true;
        while(removed.count(link.fromNode))
        {
            const auto node=nodes.find(link.fromNode);const auto input=incoming.find(link.fromNode);
            if(node==nodes.end()||!Layout::isReroute(node->second->kind)||input==incoming.end()||!visited.insert(link.fromNode).second)
            {connected=false;break;}
            route.push_back(input->second);link.fromNode=input->second->fromNode;link.fromPort=input->second->fromPort;
        }
        if(!connected)continue;
        if(route.size()>1)
        {
            link.ties.clear();std::unordered_set<int> seen;
            for(auto part=route.rbegin();part!=route.rend();++part)for(const int tie:(*part)->ties)
                if(seen.insert(tie).second)link.ties.push_back(tie);
            if(link.ties.size()>MaxTiesPerLink)
            {
                const auto all=std::move(link.ties);link.ties.clear();link.ties.reserve(MaxTiesPerLink);
                for(std::size_t i=0;i<MaxTiesPerLink;++i)link.ties.push_back(all[(i*(all.size()-1)+(MaxTiesPerLink-1)/2)/(MaxTiesPerLink-1)]);
            }
        }
        retained.push_back(std::move(link));
    }
    graph.links=std::move(retained);
    graph.nodes.erase(std::remove_if(graph.nodes.begin(),graph.nodes.end(),[&](const Node& node){return removed.count(node.id)!=0;}),graph.nodes.end());
    for(auto& group:graph.groups)group.nodes.erase(std::remove_if(group.nodes.begin(),group.nodes.end(),[&](int id){return removed.count(id)!=0;}),group.nodes.end());
}

struct Service::Program
{
    Workspace workspace;
    std::vector<CompiledGraph> graphs;
    bool sharedFrame=false,potentialFrame=false;
    std::uint64_t revision = 0;
};
struct Service::ConfigurationSnapshot
{
    std::uint64_t revision=0;
    std::map<int,Value> values;
    std::map<int,std::uint64_t> readInputs;
    std::map<int,Node> providers;
    std::map<int,std::string> errors;
};
Service::Service()
{
    std::string error;
    publish(defaults(), error);
}
Workspace Service::workspace() const { return std::atomic_load(&program_)->workspace; }
Workspace Service::workspaceMetadata() const
{
    const auto program=std::atomic_load(&program_);Workspace result;result.graphs.clear();result.graphs.reserve(program->workspace.graphs.size());
    for(const auto& graph:program->workspace.graphs)
    {
        Graph page;page.kind=graph.kind;page.workflow=graph.workflow;page.workflowName=graph.workflowName;
        page.pageName=graph.pageName;page.workflowIcon=graph.workflowIcon;page.active=graph.active;page.enabled=graph.enabled;
        result.graphs.push_back(std::move(page));
    }
    return result;
}
std::uint64_t Service::revision() const { return std::atomic_load(&program_)->revision; }
bool Service::publish(const Workspace& workspace, std::string& error)
{
    auto next = std::make_shared<Program>();
    if (!compileWorkspace(workspace, error,&next->graphs)) return false;
    next->workspace = workspace;
    for(std::size_t i=0;i<next->workspace.graphs.size();++i)if(!next->workspace.graphs[i].workflow)next->workspace.graphs[i].workflow=static_cast<int>(i)+1;
    for(std::size_t i=0;i<workspace.graphs.size();++i)
    {
        for(auto& entry:next->graphs[i].nodes)
        {
            entry.node.runtimeKey=static_cast<int>(i)*NodeKeyStride+entry.node.id;
            if(workspace.graphs[i].active&&workspace.graphs[i].enabled&&workspace.graphs[i].kind==GraphKind::Vision)
            {
                next->sharedFrame|=entry.active&&entry.node.kind==K::CurrentFrame&&!entry.node.disabled&&!entry.node.bypassed;
                next->potentialFrame|=entry.potentiallyActive&&entry.node.kind==K::CurrentFrame&&!entry.node.disabled&&!entry.node.bypassed;
            }
        }
    }
    std::map<std::string,std::pair<int,int>> calibrations;
    for(std::size_t g=0;g<next->graphs.size();++g)if(workspace.graphs[g].active&&workspace.graphs[g].enabled&&workspace.graphs[g].kind!=GraphKind::Vision)
        for(std::size_t i=0;i<next->graphs[g].nodes.size();++i)if(next->graphs[g].nodes[i].node.kind==K::MouseProfile&&!next->graphs[g].nodes[i].node.disabled&&!next->graphs[g].nodes[i].node.bypassed)
        {
            const auto name=textParameter(next->graphs[g].nodes[i].node,"name");
            if(!calibrations.emplace(name,std::pair{static_cast<int>(g),static_cast<int>(i)}).second)calibrations[name]={-2,-2};
        }
    for(auto& graph:next->graphs)for(auto& node:graph.nodes)
        if(calibrationIdentifier(node.node.kind)&&node.node.kind!=K::MouseProfile)
        {const auto source=calibrations.find(textParameter(node.node,"name"));if(source!=calibrations.end()){node.sharedGraph=source->second.first;node.sharedIndex=source->second.second;}}
    std::lock_guard<std::mutex> lock(publishMutex_);
    auto previous = std::atomic_load(&program_);
    next->revision = previous ? previous->revision + (sameExecution(previous->workspace,workspace) ? 0 : 1) : 1;
    const bool changed=!previous||next->revision!=previous->revision;
    std::atomic_store(&program_, std::shared_ptr<const Program>(std::move(next)));
    if(changed){std::lock_guard<std::mutex> telemetry(telemetryMutex_);configurationErrors_.clear();}
    return true;
}

Actions Service::evaluate(const Context& context)
{
    return evaluateDomain(context,false);
}
Actions Service::evaluateVision(const Context& context)
{
    return evaluateDomain(context,true);
}
std::vector<Node> Service::providerNodes(NodeKind kind) const
{
    const auto program=std::atomic_load(&program_);std::vector<Node> nodes;const auto previews=previewNodes();
    const bool sharedFrame=program->sharedFrame||(program->potentialFrame&&previews&&!previews->empty());
    for(std::size_t graph=0;graph<program->graphs.size();++graph)
    if(program->workspace.graphs[graph].active&&program->workspace.graphs[graph].enabled&&program->workspace.graphs[graph].kind==GraphKind::Vision)
    for(const auto& entry:program->graphs[graph].nodes)
    {
        if((!entry.active&&!(sharedFrame&&kind==K::CaptureSource))||entry.node.kind!=kind||entry.node.bypassed||entry.node.disabled)continue;
        const auto& specs=definition(entry.node.kind).parameters;
        const bool enabledParameter=std::any_of(specs.begin(),specs.end(),[](const ParameterSpec& p){return std::string(p.key)=="enabled";});
        if(enabledParameter&&!providerMayBeEnabled(entry))continue;
        nodes.push_back(entry.node);
    }
    return nodes;
}
bool Service::hasDynamicProviders() const
{
    const auto program=std::atomic_load(&program_);
    for(std::size_t graph=0;graph<program->graphs.size();++graph)
    if(program->workspace.graphs[graph].active&&program->workspace.graphs[graph].enabled&&program->workspace.graphs[graph].kind==GraphKind::Vision)
    for(const auto& entry:program->graphs[graph].nodes)
    {
        if(entry.node.kind==K::ImagePreview&&!entry.node.disabled&&!entry.node.bypassed)return true;
        if((entry.active||(program->sharedFrame&&entry.node.kind==K::CaptureSource))&&(entry.node.kind==K::CaptureSource||entry.node.kind==K::Inference)&&!entry.bindings.empty())return true;
    }
    return false;
}
std::vector<Node> Service::evaluateConfiguration(const Context& context)
{
    std::lock_guard<std::mutex> lock(configurationMutex_);return evaluateDomain(context,true,true).configuredProviders;
}
Actions Service::evaluateDomain(const Context& context,bool vision,bool configurationOnly)
{
    const auto now = std::chrono::steady_clock::now();
    const auto program = std::atomic_load(&program_);
    const auto previews=previewNodes();
    const bool sharedFrame=program->sharedFrame||(program->potentialFrame&&previews&&!previews->empty());
    const auto previewWanted=[&](int id){return previews&&std::binary_search(previews->begin(),previews->end(),id);};
    // One control worker and one Vision worker own separate mutable state.
    // Neither holds a mutex while running provider callbacks.
    auto& domain=configurationOnly?configurationDomain_:domains_[vision?1:0];
    ++domain.eventTick;
    const bool monitorSignals = !configurationOnly&&(telemetryEnabled_.load(std::memory_order_relaxed)||
        now.time_since_epoch().count()<remoteTelemetryUntil_.load(std::memory_order_relaxed));
    const auto configuration=context.useConfigurationSnapshot&&!configurationOnly&&vision?std::atomic_load(&configurationSnapshot_):nullptr;
    auto nextConfiguration=configurationOnly?std::make_shared<ConfigurationSnapshot>():nullptr;
    if(nextConfiguration)nextConfiguration->revision=program->revision;
    if(domain.revision!=program->revision)
    {
        // Latches belong to workflow/node identities, not vector slots or the
        // whole document revision. Only rebuild dependency comparisons on edits.
        std::map<int,EventState> retained;
        if(domain.eventProgram)
        {
            const auto& before=*domain.eventProgram;
            std::map<std::pair<int,std::string>,std::size_t> previousGraphs;
            for(std::size_t g=0;g<before.graphs.size();++g)previousGraphs.emplace(std::pair{workflowId(before.workspace.graphs[g],g),pageTitle(before.workspace.graphs[g])},g);
            for(std::size_t g=0;g<program->graphs.size();++g)
            {
                const auto& page=program->workspace.graphs[g];
                if(!page.active||!page.enabled||(page.kind==GraphKind::Vision)!=vision)continue;
                const auto found=previousGraphs.find({workflowId(page,g),pageTitle(page)});if(found==previousGraphs.end())continue;
                const auto previousGraph=found->second;const auto& previousPage=before.workspace.graphs[previousGraph];
                if(page.kind!=previousPage.kind)continue;
                const auto& oldNodes=before.graphs[previousGraph].nodes;const auto& nodes=program->graphs[g].nodes;
                std::unordered_map<int,std::size_t> oldIndices;
                for(std::size_t i=0;i<oldNodes.size();++i)oldIndices.emplace(oldNodes[i].node.id,i);
                std::vector<bool> unchanged(nodes.size(),false);
                for(std::size_t i=0;i<nodes.size();++i)
                {
                    const auto& next=nodes[i];const auto oldIndex=oldIndices.find(next.node.id);if(oldIndex==oldIndices.end())continue;
                    const auto& old=oldNodes[oldIndex->second];bool same=sameNodeExecution(next.node,old.node);
                    const auto dependency=[&](int nextIndex,int oldIndex){return nextIndex<0||oldIndex<0?nextIndex==oldIndex:
                        nodes[nextIndex].node.id==oldNodes[oldIndex].node.id&&unchanged[nextIndex];};
                    for(std::size_t pin=0;pin<next.inputs.size()&&same;++pin)same=dependency(next.inputs[pin],old.inputs[pin]);
                    same=same&&dependency(next.variableSource,old.variableSource);
                    // Cross-workflow calibration changes are conservatively
                    // reseeded; their live values must not synthesize edges.
                    unchanged[i]=same&&next.sharedGraph<0&&old.sharedGraph<0;
                    if(!eventNode(next.node.kind)||next.node.kind!=old.node.kind||next.node.disabled||next.node.bypassed||!next.potentiallyActive)continue;
                    EventState state;state.reseed=true;
                    const auto previous=domain.events.find(static_cast<int>(previousGraph)*NodeKeyStride+next.node.id);
                    if(previousPage.active&&previousPage.enabled&&previous!=domain.events.end())state=previous->second;
                    if(!unchanged[i])state.reseed=true;
                    retained.emplace(static_cast<int>(g)*NodeKeyStride+next.node.id,state);
                }
            }
        }
        domain.events=std::move(retained);domain.eventProgram=program;
        domain.filters.clear();domain.configurations.clear();domain.settings.clear();domain.calibrations.clear();domain.previews.clear();domain.revision=program->revision;domain.sampledAt={};
        for(auto& signals:domain.signals)signals.clear();domain.signalProgram.reset();
    }
    if(monitorSignals&&!domain.telemetryWasEnabled)
    {
        domain.sampledAt={};for(auto& signals:domain.signals)signals.clear();domain.signalProgram.reset();
    }
    domain.telemetryWasEnabled=monitorSignals;
    for(auto it=domain.previews.begin();it!=domain.previews.end();)
        if(!previewWanted(it->first))it=domain.previews.erase(it);else++it;
    const auto inDomain=[&](int graph){const auto& page=program->workspace.graphs[graph];return page.active&&page.enabled&&((page.kind==GraphKind::Vision)==vision);};
    bool captureObserved=false;
    const auto inputIsCaptured=[&]{
        const bool captured=context.inputCaptured||(context.inputCapturedNow&&context.inputCapturedNow());
        if(captured&&!captureObserved)
        {
            captureObserved=true;
            for(int graph=0;graph<static_cast<int>(program->graphs.size());++graph)if(inDomain(graph))
                for(const auto& compiled:program->graphs[graph].nodes)
                    if(compiled.node.kind==K::KeyHeld||compiled.node.kind==K::KeyPressed||
                       compiled.node.kind==K::KeyReleased||compiled.node.kind==K::Toggle)
                        domain.events[graph*1000001+compiled.node.id].reseed=true;
        }
        return captured;
    };
    inputIsCaptured();
    Actions actions;
    actions.revision = program->revision;
    bool sample = monitorSignals && now - domain.sampledAt >= std::chrono::milliseconds(33);
    auto completedAt=now;
    Telemetry telemetry;
    for (int g=0;g<static_cast<int>(program->graphs.size());++g)
    {
        auto& frame=domain.evaluations[g];
        if(!inDomain(g)){frame.values.clear();frame.evaluated.clear();frame.visiting.clear();frame.readInputs.clear();continue;}
        auto& values=frame.values;auto& evaluated=frame.evaluated;auto& readInputs=frame.readInputs;
        const auto& nodes = program->graphs[g].nodes;
        domain.inputStorage[g].resize(nodes.size());
        values.resize(nodes.size());evaluated.assign(nodes.size(),0);frame.visiting.assign(nodes.size(),0);readInputs.assign(nodes.size(),0);
        if(configuration&&configuration->revision==program->revision&&vision)
            for(std::size_t i=0;i<nodes.size();++i)
            {
                const auto found=configuration->values.find(g*NodeKeyStride+nodes[i].node.id);if(found==configuration->values.end())continue;
                values[i]=found->second;evaluated[i]=true;const auto read=configuration->readInputs.find(g*NodeKeyStride+nodes[i].node.id);if(read!=configuration->readInputs.end())readInputs[i]=read->second;
            }
        if(monitorSignals&&domain.signalProgram!=program)
        {
            std::vector<DomainState::SignalState> remapped;remapped.reserve(nodes.size());
            for(const auto& entry:nodes)
            {
                const auto& previous=domain.signals[g];
                const auto found=std::find_if(previous.begin(),previous.end(),[&](const auto& signal){return signal.nodeId==entry.node.id;});
                remapped.push_back(found==previous.end()?DomainState::SignalState{}:*found);remapped.back().nodeId=entry.node.id;
            }
            domain.signals[g]=std::move(remapped);
        }
    }
    std::size_t activeNodes=0;for(const auto& frame:domain.evaluations)activeNodes+=frame.values.size();domain.pending.resize(activeNodes);
    std::pair<int,int> requiredDependency{-1,-1};
    auto evaluateNode = [&](int g,int index) -> const Value&
    {
        const auto& nodes=program->graphs[g].nodes;
        auto& values=domain.evaluations[g].values;auto& evaluated=domain.evaluations[g].evaluated;auto& readInputs=domain.evaluations[g].readInputs;

            const std::size_t i = static_cast<std::size_t>(index);
            if (evaluated[i]) return values[i];
            evaluated[i] = true;
            const auto ready=[&](int dependency){if(evaluated[dependency])return true;evaluated[i]=false;requiredDependency={g,dependency};return false;};
            const auto& n = nodes[i];
            if(n.node.kind==K::ImagePreview&&!previewWanted(previewKey(g,n.node.id)))
            {values[i]=boolean(false);values[i].valid=false;return values[i];}
            const Node* resolved=&n.node;
            const auto& rawDefinition=definition(n.node.kind);
            if(!n.node.disabled&&!n.node.bypassed)
            {
                const bool provider=n.node.kind==K::CaptureSource||n.node.kind==K::Inference||n.node.kind==K::DepthInference||n.node.kind==K::DepthToMask;
                if(provider||isSink(n.node.kind))
                {
                    for(std::size_t pin=0;pin<rawDefinition.inputs.size();++pin)
                    {
                        const char* key=rawDefinition.inputs[pin].defaultParameter;
                        if(!(provider&&key&&std::strcmp(key,"enabled")==0)&&!(isSink(n.node.kind)&&pin==0))continue;
                        Value enabled=scalarInputDefault(n.node,pin);
                        if(n.inputs[pin]>=0){readInputs[i]|=std::uint64_t{1}<<pin;if(!ready(n.inputs[pin]))return values[i];enabled=values[n.inputs[pin]];}
                        if(!enabled.valid||!enabled.boolean)
                        {
                            values[i]=boolean(false);values[i].valid=enabled.valid;
                            // Missing upstream data is a normal inactive state.
                            // Only propagate an error actually reported by the dependency.
                            if(!enabled.valid)values[i].error=enabled.error;
                            if(provider){values[i].type=rawDefinition.outputs[0].type;values[i].valid=false;}
                            return values[i];
                        }
                        break;
                    }
                }
            }
            if(configuration&&configuration->revision==program->revision)
            {
                const auto cached=configuration->values.find(g*NodeKeyStride+n.node.id);
                if(cached!=configuration->values.end())
                {values[i]=cached->second;const auto read=configuration->readInputs.find(g*NodeKeyStride+n.node.id);if(read!=configuration->readInputs.end())readInputs[i]=read->second;return values[i];}
                const auto provider=configuration->providers.find(g*NodeKeyStride+n.node.id);if(provider!=configuration->providers.end())resolved=&provider->second;
            }
            if(resolved==&n.node&&!n.node.disabled&&!n.node.bypassed&&!n.bindings.empty())
            {
                auto inserted=domain.configurations.try_emplace(g*1000001+n.node.id,n.node);auto& effective=inserted.first->second;
                const auto& definition_=definition(n.node.kind);
                for(const auto pin:n.bindings)
                {
                    readInputs[i]|=std::uint64_t{1}<<pin;if(!ready(n.inputs[pin]))return values[i];const auto& value=values[n.inputs[pin]];const auto& input=definition_.inputs[pin];
                    auto fail=[&](const std::string& reason)->const Value&{values[i].type=definition_.outputs.empty()?T::Boolean:definition_.outputs[0].type;values[i].valid=false;values[i].error=reason;return values[i];};
                    if(!value.valid||!value.error.empty())return fail(value.error.empty()?std::string("Unavailable setting: ")+input.name:value.error);
                    if(n.node.kind==K::CaptureSource&&pin==0)
                    {
                        if(!value.monitor)return fail("Monitor is unavailable.");
                        effective.parameters["monitor_idx"]=value.monitor->index;effective.textParameters["monitor_device"]=value.monitor->device;effective.textParameters["capture_target"]="monitor";continue;
                    }
                    const char* key=input.defaultParameter;
                    if(input.type==T::String)
                    {
                        const auto spec=std::find_if(definition_.textParameters.begin(),definition_.textParameters.end(),[&](const TextParameterSpec& s){return std::strcmp(s.key,key)==0;});
                        if(spec==definition_.textParameters.end()||value.text.size()>spec->maxLength)return fail(std::string("Invalid text setting: ")+input.name);
                        auto& text=effective.textParameters[key];if(text!=value.text)text=value.text;
                    }
                    else
                    {
                        const auto spec=std::find_if(definition_.parameters.begin(),definition_.parameters.end(),[&](const ParameterSpec& s){return std::strcmp(s.key,key)==0;});
                        if(spec==definition_.parameters.end())return fail("Unknown setting binding.");
                        double number=input.type==T::Boolean?(value.boolean?1:0):value.type==T::Integer?static_cast<double>(value.integer):value.number;
                        if(input.type==T::Float&&spec->kind==ParameterKind::Integer)number=std::clamp(std::floor(number),spec->minValue,spec->maxValue);
                        if(!std::isfinite(number)||number<spec->minValue||number>spec->maxValue||(spec->kind!=ParameterKind::Number&&std::trunc(number)!=number))return fail(std::string("Setting outside supported range: ")+input.name);
                        effective.parameters[key]=number;
                        if(n.node.kind==K::CaptureSource&&std::strcmp(key,"monitor_idx")==0)effective.textParameters["monitor_device"].clear();
                    }
                }
                resolved=&effective;
            }
            const auto& def = definition(n.node.kind);
            const Node& settings = *resolved;
            if(resolved!=&n.node)for(const auto pin:n.bindings)readInputs[i]|=std::uint64_t{1}<<pin;
            if(settings.disabled)
            {values[i]=Value{};values[i].type=def.outputs.empty()?T::Boolean:def.outputs.front().type;values[i].valid=false;return values[i];}
            if (settings.bypassed)
            {
                if (isSink(settings.kind))
                {
                    values[i] = boolean(false);
                    return values[i];
                }
                const auto input = std::find_if(def.inputs.begin(),def.inputs.end(),[&](const Pin& p) {
                    return p.type == def.outputs[0].type;
                });
                const auto pin = static_cast<std::size_t>(std::distance(def.inputs.begin(),input));
                values[i].type = def.outputs[0].type;
                if (n.inputs[pin] >= 0) {readInputs[i]|=std::uint64_t{1}<<pin;if(!ready(n.inputs[pin]))return values[i];values[i] = values[n.inputs[pin]];}
                else values[i]=scalarInputDefault(settings,pin);
                promoteInteger(values[i],def.outputs[0].type);
                return values[i];
            }
            if(variableGet(settings.kind))
            {
                if(n.variableSource>=0){if(!ready(n.variableSource))return values[i];values[i]=values[n.variableSource];}
                else{values[i].type=def.outputs[0].type;values[i].valid=false;values[i].error="No matching Set for "+textParameter(settings,"name");}
                return values[i];
            }
            const Value* shared=nullptr;
            if(calibrationIdentifier(settings.kind)&&settings.kind!=K::MouseProfile)
            {
                auto& value=values[i];value.type=def.outputs.empty()?T::Boolean:def.outputs[0].type;value.valid=false;
                if(vision||n.sharedGraph<0||!inDomain(n.sharedGraph))
                {value.error=n.sharedGraph==-2?"Ambiguous shared calibration: "+textParameter(settings,"name"):"Missing shared calibration: "+textParameter(settings,"name");return value;}
                const auto& source=domain.evaluations[n.sharedGraph];
                if(!source.evaluated[n.sharedIndex]){evaluated[i]=false;requiredDependency={n.sharedGraph,n.sharedIndex};return value;}
                shared=&source.values[n.sharedIndex];
                if(!shared->valid||!shared->calibration){value.error=shared->error.empty()?"Shared calibration is unavailable.":shared->error;return value;}
                if(settings.kind==K::SharedCalibration){value=*shared;return value;}
                if(settings.kind==K::SharedCalibrationScale){value=point(shared->calibration->pixelsPerCount);return value;}
            }
            if(settings.kind==K::ImagePreview)
            {
                auto& out=values[i];out=boolean(false);out.valid=false;
                int pin=static_cast<int>(parameter(settings,"mode"));
                if(!pin)for(int p=1;p<=3;++p)if(n.inputs[p]>=0){pin=p;break;}
                if(pin<1||pin>3||n.inputs[pin]<0){domain.previews.erase(previewKey(g,settings.id));return out;}
                const std::array<double,4> options={parameter(settings,"mode"),parameter(settings,"max_size"),parameter(settings,"colormap"),parameter(settings,"draw_boxes")};
                const auto cached=domain.previews.find(previewKey(g,settings.id));
                if(!configurationOnly&&cached!=domain.previews.end()&&cached->second.settings==options&&
                    (!context.frame||!cached->second.image||!imageValue(cached->second.image->sourceType)||cached->second.image->generation==context.frame->generation)&&
                    now-cached->second.updatedAt<std::chrono::duration<double>(1.0/parameter(settings,"max_fps")))
                {out.preview=cached->second.image;out.valid=static_cast<bool>(out.preview);out.boolean=out.valid;out.error=cached->second.error;return out;}
                readInputs[i]|=std::uint64_t{1}<<pin;if(!ready(n.inputs[pin]))return out;
                const auto& image=values[n.inputs[pin]];
                if(!image.valid&&!configurationOnly){out.error=image.error;domain.previews.erase(previewKey(g,settings.id));return out;}
                const bool raster=imageValue(image.type);
                DetectionStream detections;
                if(raster&&parameter(settings,"draw_boxes")!=0&&n.inputs[4]>=0)
                {readInputs[i]|=std::uint64_t{1}<<4;if(!ready(n.inputs[4]))return out;if(values[n.inputs[4]].valid)detections=values[n.inputs[4]].detections;}
                if(configurationOnly)return out;
                if(raster&&!context.preview){out.error="Image preview is unavailable in this workflow.";return out;}
                const auto currentPreviews=previewNodes();
                if(!currentPreviews||!std::binary_search(currentPreviews->begin(),currentPreviews->end(),previewKey(g,settings.id)))return out;
                auto& state=domain.previews[previewKey(g,settings.id)];
                const auto generation=image.mask?image.mask->generation:image.frame?image.frame->generation:0;
                const auto identity=image.mask?image.mask->storage.get():image.frame?image.frame->storage.get():nullptr;
                const std::array<std::uint64_t,5> stamp={generation,signalStamp(image),reinterpret_cast<std::uintptr_t>(identity),
                    detections?static_cast<std::uint64_t>(detections->version):0,detections?detections->generation:0};
                const bool reset=state.stamp[0]!=generation||state.settings!=options;
                state.updatedAt=now;
                if(reset||state.stamp!=stamp||!state.image)
                {
                    state.stamp=stamp;state.settings=options;state.error.clear();state.image.reset();
                    try
                    {
                        state.image=raster?context.preview(image,detections,settings):textPreview(image);
                        if(raster&&state.image&&(state.image->width<1||state.image->height<1||state.image->width>640||state.image->height>640||
                            state.image->rgba.size()!=static_cast<std::size_t>(state.image->width)*state.image->height*4))
                        {state.image.reset();state.error="Preview returned an invalid image.";}
                    }
                    catch(const std::exception& error){state.error=error.what();}
                    catch(...){state.error="Preview conversion failed.";}
                }
                out.preview=state.image;out.valid=static_cast<bool>(state.image);out.boolean=out.valid;out.error=state.error;return out;
            }
            if(configurationOnly&&(settings.kind==K::CaptureSource||settings.kind==K::Inference||settings.kind==K::DepthInference||settings.kind==K::DepthToMask))
            {
                for(std::size_t pin=0;pin<def.dataInputs;++pin)if(n.inputs[pin]>=0)
                {readInputs[i]|=std::uint64_t{1}<<pin;if(!ready(n.inputs[pin]))return values[i];}
                if(parameter(settings,"enabled")!=0){auto provider=settings;provider.runtimeKey=g*NodeKeyStride+settings.id;actions.configuredProviders.push_back(std::move(provider));}
                values[i].type=def.outputs[0].type;values[i].valid=false;return values[i];
            }
            if(settings.kind>=K::SelectBoolean&&settings.kind<=K::SelectMonitors)
            {
                Value condition=scalarInputDefault(settings,0);if(n.inputs[0]>=0){readInputs[i]|=1;if(!ready(n.inputs[0]))return values[i];condition=values[n.inputs[0]];}
                const std::size_t chosen=condition.boolean?1:2;values[i].type=def.outputs[0].type;
                if(!condition.valid){values[i].valid=false;values[i].error=condition.error;return values[i];}
                if(n.inputs[chosen]>=0){readInputs[i]|=std::uint64_t{1}<<chosen;if(!ready(n.inputs[chosen]))return values[i];values[i]=values[n.inputs[chosen]];promoteInteger(values[i],def.outputs[0].type);}
                else values[i]=scalarInputDefault(settings,chosen);
                return values[i];
            }
            if (isSink(settings.kind))
            {
                if(inputIsCaptured()&&(!vision||settings.kind==K::ScreenshotOutput))
                {values[i]=boolean(false);return values[i];}
                // Enable is a real graph dependency. Disabled outputs do not
                // advance prediction or motion state in their data branch.
                Value fallbackEnable;
                if (n.inputs[0] < 0 && !def.inputs[0].defaultParameter&&settings.inputValues.count(0)==0)
                {
                    values[i] = boolean(false);
                    values[i].valid = false;
                    return values[i];
                }
                fallbackEnable=scalarInputDefault(settings,0,true);
                if(n.inputs[0]>=0)readInputs[i]|=1u;
                if(n.inputs[0]>=0&&!ready(n.inputs[0]))return values[i];
                const Value& enable = n.inputs[0]>=0?values[n.inputs[0]]:fallbackEnable;
                if (!enable.valid || !enable.boolean)
                {
                    values[i] = boolean(false);
                    values[i].valid = enable.valid;
                    return values[i];
                }
            }
            if((settings.kind==K::ApplyMask||settings.kind==K::FilterMask)&&parameter(settings,"enabled")==0)
            {
                values[i].type=def.outputs[0].type;
                if(n.inputs[0]>=0){readInputs[i]|=1u;if(!ready(n.inputs[0]))return values[i];values[i]=values[n.inputs[0]];}else values[i].valid=false;
                return values[i];
            }
            if((settings.kind==K::CaptureSource||settings.kind==K::Inference||settings.kind==K::DepthInference||settings.kind==K::DepthToMask)&&parameter(settings,"enabled")==0)
            {values[i].type=def.outputs[0].type;values[i].valid=false;return values[i];}
            const Node* outputSettings=&settings;
            if(settings.kind==K::OverlayOutput||settings.kind==K::DatasetOutput)
            {
                const bool overlay=settings.kind==K::OverlayOutput;const int pin=overlay?5:3;Settings style;
                if(n.inputs[pin]>=0)
                {
                    readInputs[i]|=std::uint64_t{1}<<pin;if(!ready(n.inputs[pin]))return values[i];
                    const auto& input=values[n.inputs[pin]];
                    if(!input.valid){values[i]=boolean(false);values[i].valid=false;values[i].error=input.error;return values[i];}
                    style=input.settings;
                }
                if(overlay&&!style){values[i]=boolean(false);values[i].valid=false;return values[i];}
                auto& state=domain.settings[g*1000001+settings.id];
                if(!state.resolvedOnce||state.input!=style||state.numbers!=settings.parameters||state.texts!=settings.textParameters)
                {
                    state.resolved=defaultNode(overlay?K::RenderOverlay:K::CollectDataset,settings.id);
                    if(overlay)for(const char* key:{"game_overlay_draw_boxes","game_overlay_draw_future","game_overlay_draw_wind_tail","game_overlay_draw_frame",
                        "game_overlay_draw_circle_fov","game_overlay_show_target_correction","game_overlay_icon_enabled","circle_fov_enabled","depth_debug_overlay_enabled","depth_mask_enabled"})
                        state.resolved.parameters[key]=0;
                    else state.resolved.parameters["auto_label_data"]=0;
                    if(style)
                    {
                        for(const auto& entry:style->numbers)state.resolved.parameters[entry.first]=entry.second;
                        for(const auto& entry:style->texts)state.resolved.textParameters[entry.first]=entry.second;
                    }
                    for(const auto& spec:def.parameters)state.resolved.parameters[spec.key]=parameter(settings,spec.key);
                    for(const auto& spec:def.textParameters)state.resolved.textParameters[spec.key]=textParameter(settings,spec.key);
                    state.input=style;state.numbers=settings.parameters;state.texts=settings.textParameters;state.resolvedOnce=true;
                }
                outputSettings=&state.resolved;
            }
            auto& inputs=domain.inputStorage[g][i];
            ClearInputStorage clearInputs(inputs);
            bool valid = true;
            for (std::size_t p = 0; p < def.dataInputs; ++p)
            {
                inputs[p].type = def.inputs[p].type;
                if((settings.kind==K::RenderOverlay||settings.kind==K::OverlayOutput)&&(p==3||p==4))
                {
                    const bool wanted=p==3?parameter(*outputSettings,"depth_mask_enabled")!=0:parameter(*outputSettings,"depth_debug_overlay_enabled")!=0;
                    if(wanted&&n.inputs[p]>=0){readInputs[i]|=std::uint64_t{1}<<p;if(!ready(n.inputs[p]))return values[i];inputs[p]=values[n.inputs[p]];}
                    if(!wanted||!inputs[p].valid){inputs[p]=Value{};inputs[p].type=def.inputs[p].type;}
                }
                else if (n.inputs[p] >= 0) {readInputs[i]|=std::uint64_t{1}<<p;if(!ready(n.inputs[p]))return values[i];inputs[p] = values[n.inputs[p]];}
                else if(def.inputs[p].defaultParameter||(!settings.inputValues.empty()&&settings.inputValues.count(static_cast<int>(p)))||(!settings.inputTexts.empty()&&settings.inputTexts.count(static_cast<int>(p))))
                {
                    inputs[p]=scalarInputDefault(settings,p,true);
                }
                else if(((settings.kind==K::MouseDelta||settings.kind==K::SharedMouseDelta)&&p==2&&parameter(settings,"legacy_center")!=0)||
                    (settings.kind==K::PointDelta&&p==1&&parameter(settings,"implicit_center")!=0)||(settings.kind==K::FilterCircle&&p==1)||(settings.kind==K::SpeedCurve&&p==1)||((settings.kind==K::ScopeCheck||settings.kind==K::BoxContainsPoint)&&p==2))inputs[p]=point(context.center);
                else if(settings.kind==K::RecoilOutput&&p==2)inputs[p]=point({1,1});
                else if(isAuxiliarySink(settings.kind)&&p>=2){inputs[p].valid=true;}
                // A point has no meaningful default; a disconnected branch is inactive.
                else if(settings.kind==K::CaptureSource&&p==0)inputs[p].valid=true;
                else if (isSink(settings.kind) || def.inputs[p].type == T::Point || def.inputs[p].type == T::Target ||
                    def.inputs[p].type == T::Detections || def.inputs[p].type == T::Movement||def.inputs[p].type==T::Frame||def.inputs[p].type==T::Depth||def.inputs[p].type==T::Mask) inputs[p].valid = false;
                else if(def.inputs[p].type==T::Monitor||def.inputs[p].type==T::Monitors)inputs[p].valid=false;
                else if(def.inputs[p].type==T::Motion||def.inputs[p].type==T::Calibration)inputs[p].valid=false;
                else if(def.inputs[p].type==T::OverlayStyle||def.inputs[p].type==T::LabelOptions)inputs[p].valid=isStyleNode(settings.kind)&&p==0;
                promoteInteger(inputs[p],def.inputs[p].type);
                valid = valid && inputs[p].valid;
            }
            const double a = inputs[0].number, b = inputs[1].number;
            const int nodeKey=g*1000001+settings.id;
            auto filtered=[&](std::array<double,4> arguments,auto create) {
                if(!valid){Value unavailable;unavailable.type=T::Detections;unavailable.valid=false;return unavailable;}
                auto& cache=domain.filters[nodeKey];
                if(cache.input!=inputs[0].detections||cache.arguments!=arguments||!cache.output)
                {cache.input=inputs[0].detections;cache.arguments=arguments;cache.output=create();}
                return streamValue(cache.output);
            };
            Value out;
            try
            {
            switch (settings.kind)
            {
            case K::OverlayBoxes:case K::OverlayFrame:case K::OverlayMotion:case K::OverlayIcon:case K::OverlayDepth:case K::OverlayCorrection:case K::DatasetLabels:
            {
                out.type=settings.kind==K::DatasetLabels?T::LabelOptions:T::OverlayStyle;
                if(!valid){out.valid=false;out.error=inputs[0].error;break;}
                const Settings source=isStyleNode(settings.kind)?inputs[0].settings:nullptr;auto& state=domain.settings[nodeKey];
                if(!state.output||state.input!=source||state.numbers!=settings.parameters||state.texts!=settings.textParameters)
                {
                    auto combined=source?std::make_shared<SettingsData>(*source):std::make_shared<SettingsData>();
                    for(const auto& spec:def.parameters)combined->numbers[spec.key]=parameter(settings,spec.key);
                    for(const auto& spec:def.textParameters)combined->texts[spec.key]=textParameter(settings,spec.key);
                    state.input=source;state.output=std::move(combined);state.numbers=settings.parameters;state.texts=settings.textParameters;
                }
                out.settings=state.output;break;
            }
            case K::AimHeld: out = boolean(!inputIsCaptured()&&context.aimHeld); break;
            case K::AutoAimEnabled: out = boolean(parameter(settings,"enabled")!=0); break;
            case K::ShootHeld: out = boolean(!inputIsCaptured()&&context.shootHeld); break;
            case K::ZoomHeld: out = boolean(!inputIsCaptured()&&context.zoomHeld); break;
            case K::TargetValid: out = boolean(inputs[0].target.valid); break;
            case K::TargetObserved: out = boolean(inputs[0].target.valid && inputs[0].target.observed); break;
            case K::AutoShootEnabled: out = boolean(parameter(settings,"enabled")!=0); break;
            case K::RecoilEnabled: out = boolean(parameter(settings,"enabled")!=0); break;
            case K::RecoilStrength: out = number(parameter(settings,"strength")); break;
            case K::Target:
            case K::TargetNearest:
            case K::TargetTracker:
            case K::ChooseTarget:
            case K::TrackDetections:
                out = target({});
                out.valid=false;
                if (valid && context.selectTarget)
                {actions.activeSelectionKeys.push_back(nodeKey);out=target(context.selectTarget(nodeKey,inputs[0].detections,inputs[1].boolean,settings));}
                break;
            case K::Center: out = point(context.center); break;
            case K::TargetDistance:
                out = number(std::hypot(inputs[0].target.point.x - inputs[1].point.x, inputs[0].target.point.y - inputs[1].point.y));
                out.valid = out.valid && inputs[0].target.valid; break;
            case K::CaptureFps: out = number(context.captureFps); break;
            case K::Boolean: out = boolean(settings.value != 0.0); break;
            case K::Number: out = number(settings.value); break;
            case K::Integer: out = integer(static_cast<std::int64_t>(settings.value)); break;
            case K::String:out=stringValue(textParameter(settings,"value"));break;
            case K::ConcatString:out=stringValue(inputs[0].text+inputs[1].text);break;
            case K::IntegerToString:out=stringValue(std::to_string(inputs[0].integer));break;
            case K::FloatToString:
            {
                char text[512];const int size=std::snprintf(text,sizeof(text),"%.*f",static_cast<int>(parameter(settings,"digits")),a);
                out=stringValue(size>=0&&size<static_cast<int>(sizeof(text))?std::string(text):std::string{});out.valid=out.valid&&size>=0&&size<static_cast<int>(sizeof(text));break;
            }
            case K::GetMonitors:out=monitorsValue(context.monitors?context.monitors():systemMonitors());break;
            case K::PrimaryMonitor:
            {
                const auto list=context.monitors?context.monitors():systemMonitors();Monitor selected;
                if(list)for(const auto& m:*list)if(m.primary){selected=Monitor(list,&m);break;}
                out=monitorValue(std::move(selected));if(!out.valid)out.error="Primary monitor is unavailable.";break;
            }
            case K::MonitorAt:
            {
                Monitor selected;const auto list=inputs[0].monitors;const auto index=inputs[1].integer;
                if(list&&index>=0&&static_cast<std::size_t>(index)<list->size())selected=Monitor(list,&(*list)[index]);
                out=monitorValue(std::move(selected));if(!out.valid)out.error="Monitor index is unavailable.";break;
            }
            case K::MonitorCount:out=integer(inputs[0].monitors?static_cast<std::int64_t>(inputs[0].monitors->size()):0);break;
            case K::MonitorIndex:out=integer(inputs[0].monitor?inputs[0].monitor->index:0);break;
            case K::MonitorName:out=stringValue(inputs[0].monitor?inputs[0].monitor->device:std::string{});break;
            case K::MonitorSize:out=point(inputs[0].monitor?Point{static_cast<double>(inputs[0].monitor->width),static_cast<double>(inputs[0].monitor->height)}:Point{});break;
            case K::MonitorPosition:out=point(inputs[0].monitor?Point{static_cast<double>(inputs[0].monitor->x),static_cast<double>(inputs[0].monitor->y)}:Point{});break;
            case K::MonitorPrimary:out=boolean(inputs[0].monitor&&inputs[0].monitor->primary);break;
            case K::ToFloat: out=number(inputs[0].integer); break;
            case K::ToInteger:
            {
                const int mode=static_cast<int>(parameter(settings,"rounding"));
                const double rounded=mode==1?std::floor(a):mode==2?std::ceil(a):mode==3?std::round(a):std::trunc(a);
                if(!std::isfinite(rounded)||rounded<MinInteger||rounded>MaxInteger)
                {out.type=T::Integer;out.valid=false;out.error="Float is outside the signed 32-bit Int range.";}
                else out=integer(static_cast<std::int64_t>(rounded));
                break;
            }
            case K::IntegerAdd: out=integer(static_cast<std::int64_t>(inputs[0].integer)+inputs[1].integer); break;
            case K::IntegerSubtract: out=integer(static_cast<std::int64_t>(inputs[0].integer)-inputs[1].integer); break;
            case K::IntegerMultiply: out=integer(static_cast<std::int64_t>(inputs[0].integer)*inputs[1].integer); break;
            case K::IntegerDivide:
                if(inputs[1].integer==0){out.type=T::Integer;out.valid=false;if(valid)out.error="Int division by zero.";}
                else out=integer(static_cast<std::int64_t>(inputs[0].integer)/inputs[1].integer);
                break;
            case K::IntegerClamp: out=integer(std::clamp(inputs[0].integer,std::min(inputs[1].integer,inputs[2].integer),std::max(inputs[1].integer,inputs[2].integer))); break;
            case K::And: out = boolean(inputs[0].boolean && inputs[1].boolean); break;
            case K::Or: out = boolean(inputs[0].boolean || inputs[1].boolean); break;
            case K::Not: out = boolean(!inputs[0].boolean); break;
            case K::Greater: out = boolean(a > b); break;
            case K::Less: out = boolean(a < b); break;
            case K::Add: out = number(a + b); break;
            case K::Subtract: out = number(a - b); break;
            case K::Multiply: out = number(a * b); break;
            case K::Divide:
                out=number(b!=0.0?a/b:std::numeric_limits<double>::quiet_NaN());
                if(valid){if(b==0.0)out.error="Float division by zero.";else if(!out.valid)out.error="Float result is not finite.";}break;
            case K::Clamp: out = number(std::clamp(a, std::min(b, inputs[2].number), std::max(b, inputs[2].number))); break;
            case K::PointX: out = number(inputs[0].point.x); break;
            case K::PointY: out = number(inputs[0].point.y); break;
            case K::MakePoint: out = point({a,b}); break;
            case K::OffsetPoint: out = point({inputs[0].point.x + b, inputs[0].point.y + inputs[2].number}); break;
            case K::Distance: out = number(std::hypot(inputs[0].point.x - inputs[1].point.x, inputs[0].point.y - inputs[1].point.y)); break;
            case K::Prediction:
                out = point(inputs[0].point);
                out.valid=false;
                if (valid && context.predict)
                {actions.activePredictionKeys.push_back(nodeKey);out=point(context.predict(nodeKey,inputs[0].point,settings,inputs[1].number,inputs[2].boolean));}
                break;
            case K::MotionEstimate:
                out.type=T::Motion;out.valid=false;
                if(valid&&context.estimateMotion){actions.activePredictionKeys.push_back(nodeKey);out=motionValue(context.estimateMotion(nodeKey,inputs[0].point,settings,inputs[1].boolean));}
                break;
            case K::MotionPosition:out=point(inputs[0].motion.position);break;
            case K::MotionVelocity:out=point(inputs[0].motion.velocity);break;
            case K::MotionDamping:out=number(inputs[0].motion.damping);break;
            case K::MotionReady:out=boolean(inputs[0].motion.ready);break;
            case K::PredictionTime:
                out=number(std::clamp(inputs[0].number+(parameter(settings,"kalman_compensate_detection_delay")!=0?std::clamp(context.observationDelaySeconds,0.0,0.35):0)+parameter(settings,"kalman_additional_prediction_ms")*0.001,0.0,1.5));break;
            case K::ProjectPoint:
                out=point(projectPoint(inputs[0].point,inputs[1].point,inputs[2].number,inputs[3].number,inputs[4].boolean));break;
            case K::FutureTrail:
                out=point(inputs[0].point);actions.aimPoint=inputs[0].point;
                if(valid&&parameter(settings,"draw_futurePositions")!=0&&context.futureTrail)context.futureTrail(inputs[1].motion,inputs[2].number,settings);
                break;
            case K::BoxPoint:
                out=point({inputs[0].point.x+inputs[1].point.x*inputs[2].point.x,inputs[0].point.y+inputs[1].point.y*inputs[2].point.y});break;
            case K::TargetAnchor:
                out=point({0.5,inputs[0].integer==static_cast<int>(parameter(settings,"class_head"))?inputs[2].number:inputs[1].number});break;
            case K::BoxContainsPoint:
                out=boolean(std::abs(inputs[0].point.x+inputs[1].point.x*0.5-inputs[2].point.x)<inputs[1].point.x*inputs[3].number*0.5&&
                    std::abs(inputs[0].point.y+inputs[1].point.y*0.5-inputs[2].point.y)<inputs[1].point.y*inputs[3].number*0.5);break;
            case K::AimOutput:
                out=boolean(valid&&inputs[0].boolean);
                if(out.boolean){actions.aim=true;actions.movement=inputs[1].movement;actions.movements.push_back(inputs[1].movement);}
                break;
            case K::AutoShootOutput:
                out=boolean(valid&&inputs[0].boolean&&inputs[1].target.valid);
                if(out.boolean){actions.autoShoot=true;if(!actions.shootTarget.valid||inputs[1].target.observed)actions.shootTarget=inputs[1].target;}
                break;
            case K::RecoilOutput:
            case K::RecoilProfileOutput:
            case K::SharedRecoilOutput:
                out=boolean(valid&&inputs[0].boolean);
                if(out.boolean)
                {
                    const auto calibration=settings.kind==K::SharedRecoilOutput?shared->calibration->pixelsPerCount:settings.kind==K::RecoilProfileOutput?mousePixelCalibration(settings,{context.center.x*2,context.center.y*2}):inputs[2].point;
                    actions.recoil=true;actions.recoilStrength+=inputs[1].number;actions.recoilPixelDeltaY+=inputs[1].number*calibration.y;
                }
                break;
            case K::MouseCalibration:
                out=point(mousePixelCalibration(settings,{context.center.x*2,context.center.y*2}));
                break;
            case K::MouseProfile:
            {
                auto& cache=domain.calibrations[nodeKey];const Point size{context.center.x*2,context.center.y*2};
                if(!cache.value||cache.numbers!=settings.parameters||cache.texts!=settings.textParameters||cache.size.x!=size.x||cache.size.y!=size.y)
                {cache.value=std::make_shared<const CalibrationData>(mouseCalibration(settings,size));cache.numbers=settings.parameters;cache.texts=settings.textParameters;cache.size=size;}
                out.type=T::Calibration;out.calibration=cache.value;break;
            }
            case K::CalibrationScale:
                out.type=T::Point;out.valid=false;if(valid&&inputs[0].calibration)out=point(inputs[0].calibration->pixelsPerCount);break;
            case K::PointDelta:out=point({inputs[0].point.x-inputs[1].point.x,inputs[0].point.y-inputs[1].point.y});actions.aimPoint=inputs[0].point;break;
            case K::PixelsToCounts:
                out.type=T::Movement;out.valid=false;if(valid&&inputs[1].calibration)out=movement(pixelsToCounts(inputs[0].point,*inputs[1].calibration));break;
            case K::ApplyMouseSpeed:out=movement(applyMouseSpeed(inputs[0].movement,inputs[1].number));break;
            case K::SharedMouseDelta:
            {
                auto converted=applyMouseSpeed(pixelsToCounts({inputs[0].point.x-inputs[2].point.x,inputs[0].point.y-inputs[2].point.y},*shared->calibration),inputs[1].number);
                if(parameter(settings,"legacy_fps")!=0&&context.captureFps>30)converted=applyMouseSpeed(converted,30/context.captureFps);
                out=movement(std::move(converted));actions.aimPoint=inputs[0].point;break;
            }
            case K::Detections:
                out=streamValue(context.detections);
                break;
            case K::AimPoint:
                if(inputs[0].target.pointPrepared)out=point(inputs[0].target.point);
                else out=point({inputs[0].target.x+inputs[0].target.width*0.5,
                    inputs[0].target.y+inputs[0].target.height*(inputs[0].target.classId==static_cast<int>(parameter(settings,"class_head"))?inputs[2].number:inputs[1].number)});
                out.valid = out.valid && inputs[0].target.valid;
                break;
            case K::ScopeCheck:
                out = boolean(valid && inputs[0].target.valid && context.inScope && context.inScope(inputs[0].target,inputs[1].number,inputs[2].point));
                break;
            case K::SpeedCurve:
                out = number(0);
                out.valid = false;
                if (valid && context.speedCurve) out = number(context.speedCurve(inputs[0].point,inputs[1].point,parameter(settings,"max_distance")>0?parameter(settings,"max_distance"):std::hypot(context.center.x,context.center.y),settings));
                break;
            case K::MouseDelta:
                out = movement({});
                out.valid = false;
                if (valid && context.mouseDelta)
                {
                    out = movement(context.mouseDelta(inputs[0].point, inputs[1].number,inputs[2].point,settings));
                    actions.aimPoint = inputs[0].point;
                }
                break;
            case K::WindMotion:
                out = movement({});
                out.valid = false;
                if (valid && context.windMotion)
                {actions.activeWindKeys.push_back(nodeKey);out=movement(context.windMotion(nodeKey,inputs[0].movement,settings));}
                break;
            case K::DeadZone:
                out = point(std::hypot(inputs[0].point.x - inputs[1].point.x,
                    inputs[0].point.y - inputs[1].point.y) <= std::max(0.0, inputs[2].number)
                    ? inputs[1].point : inputs[0].point);
                break;
            case K::LerpPoint:
            {
                const double amount = std::clamp(inputs[2].number, 0.0, 1.0);
                out = point({inputs[0].point.x + (inputs[1].point.x - inputs[0].point.x) * amount,
                    inputs[0].point.y + (inputs[1].point.y - inputs[0].point.y) * amount});
                break;
            }
            case K::ScaleMovement:
            {
                Movement scaled;
                scaled.pixelScaleX=inputs[0].movement.pixelScaleX;scaled.pixelScaleY=inputs[0].movement.pixelScaleY;
                bool finite = true;
                for (const MoveStep& step : inputs[0].movement.steps)
                {
                    const double dx = step.dx * b, dy = step.dy * b;
                    if (!std::isfinite(dx) || !std::isfinite(dy)) { finite = false; break; }
                    scaled.steps.push_back({static_cast<int>(std::lround(std::clamp(dx, -32767.0, 32767.0))),
                        static_cast<int>(std::lround(std::clamp(dy, -32767.0, 32767.0)))});
                }
                out = movement(std::move(scaled));
                out.valid = out.valid && finite;
                break;
            }
            case K::LimitMovement:
            {
                int totalX = 0, totalY = 0;
                for (const MoveStep& step : inputs[0].movement.steps) { totalX += step.dx; totalY += step.dy; }
                const double distance = std::hypot(static_cast<double>(totalX), static_cast<double>(totalY));
                const double maximum = std::max(0.0, b);
                const double factor = distance > maximum ? maximum / distance : 1.0;
                Movement limited;
                limited.pixelScaleX=inputs[0].movement.pixelScaleX;limited.pixelScaleY=inputs[0].movement.pixelScaleY;
                int prefixX = 0, prefixY = 0, previousX = 0, previousY = 0;
                for (const MoveStep& step : inputs[0].movement.steps)
                {
                    prefixX += step.dx; prefixY += step.dy;
                    const int nextX = static_cast<int>(std::trunc(prefixX * factor));
                    const int nextY = static_cast<int>(std::trunc(prefixY * factor));
                    limited.steps.push_back({nextX - previousX, nextY - previousY});
                    previousX = nextX; previousY = nextY;
                }
                out = movement(std::move(limited));
                break;
            }
            case K::Pulse:
            {
                out = boolean(false);
                out.valid = a > 0.0 && std::isfinite(context.elapsedSeconds);
                if (out.valid)
                {
                    double phase = std::fmod(context.elapsedSeconds, a);
                    if (phase < 0.0) phase += a;
                    out.boolean = phase / a < std::clamp(b, 0.0, 1.0);
                }
                break;
            }
            case K::FilterClasses:
                out=filtered({parameter(settings,"class_a"),parameter(settings,"class_b"),parameter(settings,"class_b_enabled"),0},[&]{return filterStream(inputs[0].detections,[&](const Detection& d){return d.classId==static_cast<int>(parameter(settings,"class_a"))||
                    (parameter(settings,"class_b_enabled")!=0&&d.classId==static_cast<int>(parameter(settings,"class_b")));});});
                break;
            case K::FilterConfidence:
                out=filtered({b,0,0,0},[&]{return filterStream(inputs[0].detections,[&](const Detection& d){return d.confidence>=b;});});break;
            case K::FilterSize:
                out=filtered({b,inputs[2].number,0,0},[&]{return filterStream(inputs[0].detections,[&](const Detection& d){return d.width*d.height>=b&&d.width*d.height<=inputs[2].number;});});break;
            case K::FilterCircle:
            {
                const double percent=parameter(settings,"radius_percent");
                const double radius=percent>0?std::min(inputs[0].detections->width,inputs[0].detections->height)*0.5*percent/100.0:std::max(0.0,inputs[2].number);
                out=filtered({inputs[1].point.x,inputs[1].point.y,radius,parameter(settings,"enabled")},[&]{return parameter(settings,"enabled")==0?inputs[0].detections:filterStream(inputs[0].detections,[&](const Detection& d){return std::hypot(d.x+d.width*0.5-inputs[1].point.x,d.y+d.height*0.5-inputs[1].point.y)<=radius;});});break;
            }
            case K::PreferClass:
                out=filtered({parameter(settings,"class_id"),parameter(settings,"enabled"),0,0},[&]{
                    const int cls=static_cast<int>(parameter(settings,"class_id"));
                    const bool present=std::any_of(inputs[0].detections->objects.begin(),inputs[0].detections->objects.end(),[&](const Detection& d){return d.classId==cls;});
                    return present&&parameter(settings,"enabled")!=0?filterStream(inputs[0].detections,[&](const Detection& d){return d.classId==cls;}):inputs[0].detections;
                });
                break;
            case K::TargetClass:out=integer(inputs[0].target.classId);out.valid=out.valid&&inputs[0].target.valid;break;
            case K::TargetConfidence:out=number(inputs[0].target.confidence);out.valid=inputs[0].target.valid;break;
            case K::TargetTrackId:out=integer(inputs[0].target.trackId);out.valid=out.valid&&inputs[0].target.valid;break;
            case K::TargetVelocity:out=point({inputs[0].target.velocityX,inputs[0].target.velocityY});out.valid=out.valid&&inputs[0].target.valid;break;
            case K::TargetMissedFrames:out=integer(inputs[0].target.missedFrames);out.valid=out.valid&&inputs[0].target.valid;break;
            case K::TargetAge:out=number(inputs[0].target.ageSeconds);out.valid=out.valid&&inputs[0].target.valid;break;
            case K::TargetPosition:out=point({inputs[0].target.x,inputs[0].target.y});out.valid=out.valid&&inputs[0].target.valid;break;
            case K::TargetSize:out=point({inputs[0].target.width,inputs[0].target.height});out.valid=out.valid&&inputs[0].target.valid;break;
            case K::CameraCompensation:
                out=point(inputs[0].point);
                if(parameter(settings,"enabled")!=0){out.valid=false;if(valid&&context.motionCompensation)out=point(context.motionCompensation(inputs[0].point));}
                break;
            case K::NormalizeFps:
            {
                if(parameter(settings,"enabled")==0){out=movement(inputs[0].movement);break;}
                Movement scaled=inputs[0].movement;
                const double base=parameter(settings,"base_fps"),factor=b>base?base/b:1;
                if(scaled.hasPreciseCounts)
                {
                    const int dx=static_cast<int>(std::trunc(std::clamp(scaled.preciseCounts.x*factor,-32767.0,32767.0)));
                    const int dy=static_cast<int>(std::trunc(std::clamp(scaled.preciseCounts.y*factor,-32767.0,32767.0)));
                    scaled.steps.clear();if(dx||dy)scaled.steps.push_back({dx,dy});scaled.hasPreciseCounts=false;
                    if(parameter(settings,"preserve_fraction")!=0){scaled.hasPreciseCounts=true;scaled.preciseCounts.x*=factor;scaled.preciseCounts.y*=factor;}
                }
                else for(auto& step:scaled.steps){step.dx=static_cast<int>(std::trunc(step.dx*factor));step.dy=static_cast<int>(std::trunc(step.dy*factor));}
                out=movement(std::move(scaled));break;
            }
            case K::KeyHeld:
            case K::KeyPressed:
            case K::KeyReleased:
            {
                auto& state=domain.events[nodeKey];
                if(state.tick&&state.tick!=domain.eventTick-1)state.reseed=true;
                state.tick=domain.eventTick;
                if(inputIsCaptured()){state.reseed=true;out=boolean(false);break;}
                if(!context.keyDown){state.reseed=true;out=boolean(false);out.valid=false;break;}
                const int key=static_cast<int>(parameter(settings,"virtual_key"));const bool held=key>0&&context.keyDown&&context.keyDown(key);
                if(inputIsCaptured()){state.reseed=true;out=boolean(false);break;}
                out=boolean(settings.kind==K::KeyHeld?held:!state.reseed&&
                    (settings.kind==K::KeyPressed?held&&!state.previous:!held&&state.previous));
                out.valid=static_cast<bool>(context.keyDown);state.previous=held;state.reseed=false;break;
            }
            case K::Toggle:
            {
                auto& state=domain.events[nodeKey];
                if(state.tick&&state.tick!=domain.eventTick-1)state.reseed=true;
                state.tick=domain.eventTick;
                if(inputIsCaptured()){state.reseed=true;out=boolean(false);break;}
                if(!valid){state.reseed=true;out=boolean(state.toggled);out.valid=false;break;}
                if(!state.reseed&&valid&&inputs[0].boolean&&!state.previous)state.toggled=!state.toggled;
                state.previous=valid&&inputs[0].boolean;state.reseed=false;out=boolean(state.toggled);break;
            }
            case K::CurrentFrame:out=frameValue(context.frame);break;
            case K::CaptureSource:
                actions.activeProviderKeys.push_back(nodeKey);
                out=frameValue(context.captureFrame?context.captureFrame(nodeKey,settings):context.frame);
                break;
            case K::Inference:
                out.type=T::Detections;out.valid=false;
                if(valid&&context.infer){actions.activeProviderKeys.push_back(nodeKey);out=streamValue(context.infer(nodeKey,inputs[0].frame,settings));}
                break;
            case K::DepthInference:
                out.type=T::Depth;out.valid=false;
                if(valid&&context.depth){actions.activeDepthKeys.push_back(nodeKey);out=frameValue(context.depth(nodeKey,inputs[0].frame,settings),T::Depth);}
                break;
            case K::DepthToMask:
                out.type=T::Mask;out.valid=false;
                if(valid&&context.depthToMask){actions.activeMaskKeys.push_back(nodeKey);out=maskValue(context.depthToMask(inputs[0].frame,settings));}
                break;
            case K::ApplyMask:
                out.type=T::Frame;out.valid=false;
                if(valid&&context.applyMask){actions.activeMaskKeys.push_back(nodeKey);out=frameValue(context.applyMask(inputs[0].frame,inputs[1].mask,settings));}
                break;
            case K::FilterMask:
                out.type=T::Detections;out.valid=false;
                if(valid&&context.filterMask){actions.activeMaskKeys.push_back(nodeKey);out=streamValue(context.filterMask(inputs[0].detections,inputs[1].mask,settings));}
                break;
            case K::FilterNms:
                out=filtered({b,parameter(settings,"class_agnostic"),parameter(settings,"max_candidates"),0},[&]{
                    auto result=std::make_shared<DetectionFrame>(*inputs[0].detections);
                    std::stable_sort(result->objects.begin(),result->objects.end(),[](const Detection& a,const Detection& b){return a.confidence>b.confidence;});
                    const auto maximum=static_cast<size_t>(parameter(settings,"max_candidates"));
                    if(maximum>0&&result->objects.size()>maximum)result->objects.resize(maximum);
                    std::vector<Detection> kept;kept.reserve(result->objects.size());
                    for(const auto& candidate:result->objects)
                    {
                        bool suppressed=false;
                        for(const auto& previous:kept)
                        {
                            if(parameter(settings,"class_agnostic")==0&&candidate.classId!=previous.classId)continue;
                            const double width=std::max(0.0,std::min(candidate.x+candidate.width,previous.x+previous.width)-std::max(candidate.x,previous.x));
                            const double height=std::max(0.0,std::min(candidate.y+candidate.height,previous.y+previous.height)-std::max(candidate.y,previous.y));
                            const double intersection=width*height,area=candidate.width*candidate.height+previous.width*previous.height-intersection;
                            if(area>0&&intersection/area>std::clamp(b,0.0,1.0)){suppressed=true;break;}
                        }
                        if(!suppressed)kept.push_back(candidate);
                    }
                    result->objects=std::move(kept);return DetectionStream(result);
                });
                break;
            case K::LimitDetections:
                out=filtered({b,0,0,0},[&]{
                    auto result=std::make_shared<DetectionFrame>(*inputs[0].detections);
                    std::stable_sort(result->objects.begin(),result->objects.end(),[](const Detection& a,const Detection& b){return a.confidence>b.confidence;});
                    const auto maximum=static_cast<size_t>(std::clamp(std::floor(b),0.0,100000.0));
                    if(maximum>0&&result->objects.size()>maximum)result->objects.resize(maximum);return DetectionStream(result);
                });
                break;
            case K::CollectDataset:
            case K::DatasetOutput:
                out=boolean(false);out.valid=false;
                if(valid&&context.collect){actions.activeCollectorKeys.push_back(nodeKey);out=boolean(context.collect(nodeKey,inputs[1].frame,inputs[2].detections,*outputSettings));}
                break;
            case K::RenderOverlay:
            case K::OverlayOutput:
                out=boolean(false);out.valid=false;
                if(valid&&context.render){actions.activeRenderKeys.push_back(nodeKey);out=boolean(context.render(nodeKey,inputs[1].frame,inputs[2].detections,inputs[3].mask,inputs[4].frame,*outputSettings));}
                break;
            case K::ScreenshotOutput:
                out=boolean(false);out.valid=false;
                if(valid&&context.screenshot){actions.activeScreenshotKeys.push_back(nodeKey);out=boolean(context.screenshot(nodeKey,inputs[1].frame,settings));}
                break;
            case K::VisionOutput:
                if(valid&&inputs[0].boolean)
                {
                    if(!actions.visionDetections)actions.visionDetections=inputs[1].detections;
                    else if(inputs[1].detections&&actions.visionDetections!=inputs[1].detections)
                    {
                        auto combined=std::make_shared<DetectionFrame>(*actions.visionDetections);
                        if(combined->version==inputs[1].detections->version&&combined->generation==inputs[1].detections->generation)
                            for(const auto& detection:inputs[1].detections->objects){if(combined->objects.size()>=100000)break;combined->objects.push_back(detection);}
                        actions.visionDetections=std::move(combined);
                    }
                }
                out=boolean(valid&&inputs[0].boolean);break;
            case K::ControlActive:out=boolean(context.controlActive);break;
            case K::RerouteBoolean:
            case K::RerouteNumber:
            case K::RerouteInteger:
            case K::RerouteString:
            case K::RerouteMonitor:
            case K::RerouteMonitors:
            case K::ReroutePoint:
            case K::RerouteDetections:
            case K::RerouteTarget:
            case K::RerouteMovement:
            case K::RerouteFrame:
            case K::RerouteDepth:
            case K::RerouteMask:
            case K::RerouteOverlay:
            case K::RerouteLabels:
            case K::RerouteMotion:
            case K::RerouteCalibration:
                out = std::move(inputs[0]);
                break;
            default: if(variableSet(settings.kind))out=std::move(inputs[0]);else out.valid = false; break;
            }
            }
            catch(const std::exception& error){out.valid=false;out.error=error.what();}
            catch(...){out.valid=false;out.error="Node provider failed.";}
            out.valid = out.valid && valid;
            if(!valid)for(std::size_t pin=0;pin<def.dataInputs;++pin)
                if(!inputs[pin].valid&&!inputs[pin].error.empty()){out.error=inputs[pin].error;break;}
            if(context.nodeError){const auto error=context.nodeError(nodeKey);if(!error.empty()){out.error=error;out.valid=false;}}
            if(!out.valid&&eventNode(settings.kind))domain.events[nodeKey].reseed=true;
            if(!out.valid&&out.boolean)
            {
                if(settings.kind==K::VisionOutput)actions.visionDetections.reset();
                if(settings.kind==K::AimOutput)actions.aim=false;
                if(settings.kind==K::AutoShootOutput)actions.autoShoot=false;
                if(settings.kind==K::RecoilOutput||settings.kind==K::RecoilProfileOutput||settings.kind==K::SharedRecoilOutput)actions.recoil=false;
            }
            values[i] = std::move(out);
            return values[i];
        };
    const auto evaluateRequested=[&](int graph,int requested){
        auto& pending=domain.pending;std::size_t depth=1;pending[0]={graph,requested};
        domain.evaluations[graph].visiting[requested]=1;
        while(depth)
        {
            const auto current=pending[depth-1];auto& frame=domain.evaluations[current.first];
            if(frame.evaluated[current.second]){frame.visiting[current.second]=0;--depth;continue;}
            requiredDependency={-1,-1};evaluateNode(current.first,current.second);
            if(requiredDependency.first>=0)
            {
                auto& dependency=domain.evaluations[requiredDependency.first];
                if(dependency.visiting[requiredDependency.second]||depth==pending.size())
                {
                    auto& value=frame.values[current.second];value.valid=false;value.error="Shared calibration dependency cycle.";
                    const auto& def=definition(program->graphs[current.first].nodes[current.second].node.kind);if(!def.outputs.empty())value.type=def.outputs[0].type;
                    frame.evaluated[current.second]=1;
                }
                else{dependency.visiting[requiredDependency.second]=1;pending[depth++]=requiredDependency;}
            }
            else{frame.visiting[current.second]=0;--depth;}
        }
    };
    for(int g=0;g<static_cast<int>(program->graphs.size());++g)if(inDomain(g))
    {
        const auto& nodes=program->graphs[g].nodes;
        for(std::size_t i=0;i<nodes.size();++i)
            if(configurationOnly?((nodes[i].active||(sharedFrame&&nodes[i].node.kind==K::CaptureSource))&&(nodes[i].node.kind==K::CaptureSource||nodes[i].node.kind==K::Inference))||nodes[i].node.kind==K::ImagePreview:isSink(nodes[i].node.kind))evaluateRequested(g,static_cast<int>(i));
    }
    for(int g=0;g<static_cast<int>(program->graphs.size());++g)if(inDomain(g))
    {
        const auto& nodes=program->graphs[g].nodes;auto& values=domain.evaluations[g].values;auto& evaluated=domain.evaluations[g].evaluated;auto& readInputs=domain.evaluations[g].readInputs;
        if(configurationOnly)
        {
            for(std::size_t i=0;i<nodes.size();++i)if(evaluated[i])
            {
                const auto type=values[i].type;
                if(values[i].valid&&(type==T::Boolean||type==T::Float||type==T::Integer||type==T::String||type==T::Point||type==T::Monitor||type==T::Monitors))
                {nextConfiguration->values.emplace(g*NodeKeyStride+nodes[i].node.id,values[i]);nextConfiguration->readInputs.emplace(g*NodeKeyStride+nodes[i].node.id,readInputs[i]);}
                if(nodes[i].node.kind==K::CaptureSource&&!values[i].error.empty())nextConfiguration->errors[g*NodeKeyStride+nodes[i].node.id]=values[i].error;
            }
            for(const auto& provider:actions.configuredProviders)nextConfiguration->providers.emplace(provider.runtimeKey,provider);
        }
        if(monitorSignals)
        {
            completedAt=std::chrono::steady_clock::now();
            // A Vision provider can finish after the
            // sampling interval has elapsed. Publish that completed result now.
            if(vision&&completedAt-domain.sampledAt>=std::chrono::milliseconds(33))sample=true;
            if(sample){telemetry.graphs[g].reserve(nodes.size());telemetry.graphSampledAt[g]=completedAt;}
        }
        if (monitorSignals)
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                auto& signal=domain.signals[g][i];signal.readInputs|=readInputs[i];
                const bool available=evaluated[i]&&values[i].valid&&values[i].error.empty()&&!nodes[i].node.disabled;
                signal.active=available&&signalIsActive(values[i]);
                if(available)
                {
                    auto stamp=signalStamp(values[i]);const auto kind=nodes[i].node.kind;
                    const bool action=signal.active&&((kind==K::AimOutput&&std::any_of(actions.movement.steps.begin(),actions.movement.steps.end(),[](const MoveStep& step){return step.dx||step.dy;}))||
                        ((kind==K::RecoilOutput||kind==K::RecoilProfileOutput||kind==K::SharedRecoilOutput)&&actions.recoilStrength!=0)||kind==K::CollectDataset||kind==K::DatasetOutput||kind==K::ScreenshotOutput);
                    // Compiled nodes are in dependency order. Propagate new
                    // input events even when a transform returns the same
                    // number or point for a newly acquired frame.
                    stamp^=readInputs[i];
                    for(std::size_t pin=0;pin<definition(nodes[i].node.kind).inputs.size();++pin)
                        if(nodes[i].inputs[pin]>=0&&(readInputs[i]&(std::uint64_t{1}<<pin)))stamp^=domain.signals[g][nodes[i].inputs[pin]].sequence+0x9e3779b97f4a7c15ull+(stamp<<6)+(stamp>>2);
                    if(nodes[i].variableSource>=0)stamp^=domain.signals[g][nodes[i].variableSource].sequence+0x9e3779b97f4a7c15ull+(stamp<<6)+(stamp>>2);
                    if(!signal.available||signal.stamp!=stamp||action)
                    {++signal.sequence;signal.signaledAt=completedAt;signal.signalActive=signal.active;}
                    signal.stamp=stamp;
                }
                signal.available=available;
                if(sample)
                {
                    if(!evaluated[i])
                    {
                        values[i].valid=false;const auto& def=definition(nodes[i].node.kind);
                        if(!def.outputs.empty())values[i].type=def.outputs[0].type;
                    }
                    telemetry.graphs[g].push_back({nodes[i].node.id,std::move(values[i]),signal.sequence,signal.signaledAt,signal.readInputs,signal.active,signal.signalActive});
                    signal.readInputs=0;
                }
            }
    }
    for(auto& frame:domain.evaluations)for(auto& value:frame.values)value=Value{};
    if(monitorSignals)domain.signalProgram=program;
    if (sample)
    {
        std::lock_guard<std::mutex> lock(telemetryMutex_);
        for(int graph=0;graph<static_cast<int>(program->graphs.size());++graph)if(inDomain(graph))
        {telemetry_.graphs[graph]=std::move(telemetry.graphs[graph]);telemetry_.graphSampledAt[graph]=telemetry.graphSampledAt[graph];telemetry_.graphRevisions[graph]=program->revision;}
        if(vision)
        {
            telemetry_.actions.visionDetections=actions.visionDetections;
            telemetry_.actions.activeProviderKeys=actions.activeProviderKeys;
            telemetry_.actions.activeDepthKeys=actions.activeDepthKeys;
            telemetry_.actions.activeMaskKeys=actions.activeMaskKeys;
            telemetry_.actions.activeScreenshotKeys=actions.activeScreenshotKeys;
            telemetry_.actions.activeCollectorKeys=actions.activeCollectorKeys;
            telemetry_.actions.activeRenderKeys=actions.activeRenderKeys;
        }
        else
        {
            telemetry_.actions.aim=actions.aim;telemetry_.actions.autoShoot=actions.autoShoot;telemetry_.actions.recoil=actions.recoil;
            telemetry_.actions.aimPoint=actions.aimPoint;telemetry_.actions.recoilStrength=actions.recoilStrength;
            telemetry_.actions.recoilPixelDeltaY=actions.recoilPixelDeltaY;
            telemetry_.actions.shootTarget=actions.shootTarget;telemetry_.actions.movement=actions.movement;
            telemetry_.actions.activePredictionKeys=actions.activePredictionKeys;telemetry_.actions.activeWindKeys=actions.activeWindKeys;telemetry_.actions.activeSelectionKeys=actions.activeSelectionKeys;
        }
        telemetry_.actions.valid=actions.valid;telemetry_.actions.revision=actions.revision;telemetry_.sampledAt=completedAt;
        domain.sampledAt=completedAt;
    }
    if(configurationOnly)
    {
        const auto previous=std::atomic_load(&configurationSnapshot_);bool captureChanged=!previous;
        const auto sameSource=[](const Node& a,const Node& b){
            if(a.id!=b.id||a.kind!=b.kind||a.textParameters!=b.textParameters)return false;
            const auto equal=[](const Node& left,const Node& right){for(const auto& entry:left.parameters)if(entry.first!="capture_fps"&&parameter(right,entry.first)!=entry.second)return false;return true;};
            return equal(a,b)&&equal(b,a);
        };
        if(previous)
        {
            for(const auto& entry:nextConfiguration->providers)if(entry.second.kind==K::CaptureSource)
            {const auto old=previous->providers.find(entry.first);captureChanged|=old==previous->providers.end()||!sameSource(old->second,entry.second);}
            for(const auto& entry:previous->providers)if(entry.second.kind==K::CaptureSource)captureChanged|=nextConfiguration->providers.count(entry.first)==0;
        }
        // Native capture invalidates old observations before new source settings
        // become visible to the independent frame worker.
        if(captureChanged&&context.captureConfigurationChanged)context.captureConfigurationChanged();
        std::lock_guard<std::mutex> lock(telemetryMutex_);configurationErrors_=nextConfiguration->errors;
        std::atomic_store(&configurationSnapshot_,std::shared_ptr<const ConfigurationSnapshot>(std::move(nextConfiguration)));
    }
    return actions;
}
Telemetry Service::telemetry() const
{
    std::lock_guard<std::mutex> lock(telemetryMutex_);
    auto result=telemetry_;
    for(const auto& issue:configurationErrors_)
    {
        const int graph=issue.first/NodeKeyStride,nodeId=issue.first%NodeKeyStride;if(graph<0||graph>=static_cast<int>(MaxGraphs))continue;
        auto& samples=result.graphs[graph];auto found=std::find_if(samples.begin(),samples.end(),[&](const NodeValue& value){return value.nodeId==nodeId;});
        if(found==samples.end()){samples.push_back({});found=std::prev(samples.end());found->nodeId=nodeId;}
        found->value.valid=false;found->value.error=issue.second;found->active=found->signalActive=false;
        result.graphSampledAt[graph]=std::chrono::steady_clock::now();result.graphRevisions[graph]=revision();
    }
    return result;
}
void Service::setTelemetryEnabled(bool enabled)
{
    telemetryEnabled_.store(enabled,std::memory_order_relaxed);
    if(!enabled)setPreviewNodes({});
}
void Service::requestTelemetry(std::chrono::milliseconds duration)
{
    duration=std::clamp(duration,std::chrono::milliseconds(0),std::chrono::milliseconds(5000));
    remoteTelemetryUntil_.store((std::chrono::steady_clock::now()+duration).time_since_epoch().count(),std::memory_order_relaxed);
}
void Service::setPreviewNodes(std::vector<int> nodes)
{
    if(nodes.size()>MaxLivePreviews)nodes.resize(MaxLivePreviews);
    std::sort(nodes.begin(),nodes.end());nodes.erase(std::unique(nodes.begin(),nodes.end()),nodes.end());
    previewTouched_.store(std::chrono::steady_clock::now().time_since_epoch().count(),std::memory_order_relaxed);
    const auto previous=std::atomic_load(&previewNodes_);
    if(!previous||*previous!=nodes)
    {
        std::atomic_store(&previewNodes_,std::make_shared<const std::vector<int>>(std::move(nodes)));
        const auto current=std::atomic_load(&previewNodes_);std::lock_guard<std::mutex> lock(telemetryMutex_);
        for(std::size_t graph=0;graph<MaxGraphs;++graph)for(auto& node:telemetry_.graphs[graph])if(!std::binary_search(current->begin(),current->end(),previewKey(static_cast<int>(graph),node.nodeId)))node.value.preview.reset();
    }
}
std::shared_ptr<const std::vector<int>> Service::previewNodes() const
{
    const auto touched=std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(previewTouched_.load(std::memory_order_relaxed)));
    if(!telemetryEnabled_.load(std::memory_order_relaxed)||std::chrono::steady_clock::now()-touched>std::chrono::milliseconds(500))return {};
    return std::atomic_load(&previewNodes_);
}
Service& service() { static Service instance; return instance; }

bool serialize(const Workspace& workspace, std::string& text, std::string& error)
{
    if (!validate(workspace, error)) return false;
    std::ostringstream out;
    out << std::setprecision(17) << "SUNONE_NODE_GRAPH " << CurrentFormatVersion << '\n' << "PAGES " << workspace.graphs.size() << '\n';
    for (const Graph& graph : workspace.graphs)
    {
        out << "GRAPH " << static_cast<int>(graph.kind) << ' ' << graph.panX << ' ' << graph.panY << ' '
            << graph.zoom << ' ' << graph.nodes.size() << ' ' << graph.links.size() << ' ' << graph.groups.size() << ' ' << graph.ties.size() << '\n';
        out << "PAGE " << graph.workflow << ' ' << graph.active << ' ' << graph.enabled << ' ' << std::quoted(graph.workflowName) << ' ' << std::quoted(graph.pageName) << ' ' << graph.workflowIcon << '\n';
        for (const Node& n : graph.nodes)
        {
            out << "NODE " << n.id << ' ' << static_cast<int>(n.kind) << ' ' << n.x << ' ' << n.y << ' ' << n.value
                << ' ' << (n.bypassed ? 1 : 0) << ' ' << (n.collapsed ? 1 : 0) << ' ' << n.parameters.size() << ' ' << n.textParameters.size() << ' ' << (n.disabled?1:0)
                << ' ' << n.width << ' ' << n.height << ' ' << n.inputValues.size() << ' ' << n.inputTexts.size() << '\n';
            for(const auto& entry:n.parameters)out << "PARAM " << std::quoted(entry.first) << ' ' << entry.second << '\n';
            for(const auto& entry:n.textParameters)out << "TEXT " << std::quoted(entry.first) << ' ' << std::quoted(entry.second) << '\n';
            for(const auto& entry:n.inputValues)out << "INPUT " << entry.first << ' ' << entry.second << '\n';
            for(const auto& entry:n.inputTexts)out << "INPUT_TEXT " << entry.first << ' ' << std::quoted(entry.second) << '\n';
        }
        for (const Link& l : graph.links)
        {
            out << "LINK " << l.fromNode << ' ' << l.fromPort << ' ' << l.toNode << ' ' << l.toPort << ' ' << l.ties.size();
            for(const int tie:l.ties)out << ' ' << tie;
            out << ' ' << l.bend.roundness << ' ' << l.bend.twist << '\n';
        }
        for (const Group& group : graph.groups)
        {
            out << "GROUP " << group.id << ' ' << std::quoted(group.title) << ' ' << group.x << ' ' << group.y << ' '
                << group.width << ' ' << group.height << ' ' << group.color << ' ' << group.nodes.size();
            for (int member : group.nodes) out << ' ' << member;
            out << '\n';
        }
        for(const auto& tie:graph.ties)out << "TIE " << tie.id << ' ' << tie.x << ' ' << tie.y << ' ' << tie.angle << '\n';
    }
    out << "END\n";
    auto serialized=out.str();
    if(serialized.size()>MaxWorkspaceBytes){error="Node workspace exceeds the 4 MiB file limit.";return false;}
    text = std::move(serialized);
    return true;
}
bool deserialize(const std::string& text, Workspace& workspace, std::string& error)
{
    error.clear();
    if (text.size() > MaxWorkspaceBytes) { error = "Node workspace exceeds the 4 MiB file limit."; return false; }
    std::istringstream in(text);
    std::string token;
    int version = 0;
    auto malformed = [&]() { error = "Invalid or unsupported node workspace file."; return false; };
    if (!(in >> token >> version) || token != "SUNONE_NODE_GRAPH" || version<1 || version>CurrentFormatVersion) return malformed();
    Workspace candidate;
    candidate.sourceVersion=version;
    std::size_t pages=version>=4?GraphCount:3;
    if(version>=14){if(!(in>>token>>pages)||token!="PAGES"||pages>MaxGraphs)return malformed();candidate.graphs.resize(pages);}
    for (std::size_t i=0;i<pages;++i)
    {
        Graph& graph = candidate.graphs[i];
        int kind = -1;
        std::size_t nodeCount = 0, linkCount = 0, groupCount = 0, tieCount = 0;
        if (!(in >> token >> kind >> graph.panX >> graph.panY >> graph.zoom >> nodeCount >> linkCount) ||
            token != "GRAPH" || kind<0 || kind>3 || (version<14&&kind!=i) || nodeCount > MaxNodes || linkCount > MaxLinks) return malformed();
        if (version >= 2 && (!(in >> groupCount) || groupCount > MaxGroups)) return malformed();
        if(version>=5&&(!(in>>tieCount)||tieCount>MaxWireTies))return malformed();
        graph.kind = static_cast<GraphKind>(kind);
        if(version>=14)
        {
            int active=-1,enabled=-1;if(!(in>>token>>graph.workflow>>active>>enabled>>std::quoted(graph.workflowName)>>std::quoted(graph.pageName))||token!="PAGE"||active<0||active>1||enabled<0||enabled>1)return malformed();graph.active=active!=0;graph.enabled=enabled!=0;
            if(version>=15&&!(in>>graph.workflowIcon))return malformed();
        }
        for (std::size_t n = 0; n < nodeCount; ++n)
        {
            Node node;
            int nodeKind = -1;
            if (!(in >> token >> node.id >> nodeKind >> node.x >> node.y >> node.value) ||
                token != "NODE" || nodeKind < 0 || nodeKind >= static_cast<int>(K::Count)) return malformed();
            node.kind = static_cast<K>(nodeKind);
            if (version >= 2)
            {
                int bypassed = -1,collapsed = -1;
                if (!(in >> bypassed >> collapsed) || bypassed < 0 || bypassed > 1 || collapsed < 0 || collapsed > 1) return malformed();
                node.bypassed = bypassed != 0;
                node.collapsed = collapsed != 0;
            }
            if(version>=3)
            {
                std::size_t numeric=0,strings=0,inputValues=0,inputTexts=0;
                if(!(in>>numeric>>strings)||numeric>64||strings>16)return malformed();
                if(version>=6){int disabled=-1;if(!(in>>disabled)||disabled<0||disabled>1)return malformed();node.disabled=disabled!=0;}
                if(version>=7&&!(in>>node.width>>node.height))return malformed();
                if(version>=11&&(!(in>>inputValues>>inputTexts)||inputValues>MaxDataPins||inputTexts>MaxDataPins))return malformed();
                std::uint64_t exposedInputs=0;
                if(version==9&&!(in>>exposedInputs))return malformed();
                const auto& definition_=definition(node.kind);
                if(version==9&&definition_.inputs.size()<64&&(exposedInputs>>definition_.inputs.size())!=0)return malformed();
                if(version<10&&node.height>0&&node.height<Layout::minimumHeight(node))
                {
                    std::size_t oldInputs=0;
                    for(std::size_t pin=0;pin<definition_.inputs.size();++pin)
                        oldInputs+=!definition_.inputs[pin].option||(exposedInputs&(std::uint64_t{1}<<pin))!=0;
                    const float panel=std::min(Layout::naturalSettingsHeight(node),definition_.parameters.size()+definition_.textParameters.size()>8?96.0f:64.0f);
                    const float oldMinimum=Layout::HeaderHeight+10+Layout::PinSpacing*static_cast<float>(std::max(oldInputs,definition_.outputs.size()))+26+(Layout::hasSettings(node)?panel+8:0);
                    if(!Layout::isReroute(node.kind)&&node.height>=oldMinimum)node.height=Layout::up(Layout::minimumHeight(node));
                }
                for(std::size_t p=0;p<numeric;++p)
                {
                    std::string key;double value=0;
                    if(!(in>>token>>std::quoted(key)>>value)||token!="PARAM"||key.size()>128||!node.parameters.emplace(key,value).second)return malformed();
                }
                for(std::size_t p=0;p<strings;++p)
                {
                    std::string key,value;
                    if(!(in>>token>>std::quoted(key)>>std::quoted(value))||token!="TEXT"||key.size()>128||value.size()>4096||!node.textParameters.emplace(key,value).second)return malformed();
                }
                for(std::size_t p=0;p<inputValues;++p)
                {
                    int pin=-1;double value=0;
                    if(!(in>>token>>pin>>value)||token!="INPUT"||!node.inputValues.emplace(pin,value).second)return malformed();
                }
                for(std::size_t p=0;p<inputTexts;++p)
                {
                    int pin=-1;std::string value;
                    if(!(in>>token>>pin>>std::quoted(value))||token!="INPUT_TEXT"||value.size()>4096||!node.inputTexts.emplace(pin,value).second)return malformed();
                }
            }
            if(node.kind==K::DepthInference)node.parameters.erase("depth_colormap");
            graph.nodes.push_back(node);
        }
        for (std::size_t l = 0; l < linkCount; ++l)
        {
            Link link;
            if (!(in >> token >> link.fromNode >> link.fromPort >> link.toNode >> link.toPort) || token != "LINK") return malformed();
            if(version>=5)
            {
                std::size_t count=0;if(!(in>>count)||count>MaxTiesPerLink)return malformed();
                for(std::size_t t=0;t<count;++t){int tie=0;if(!(in>>tie))return malformed();link.ties.push_back(tie);}
            }
            if(version>=13&&!(in>>link.bend.roundness>>link.bend.twist))return malformed();
            graph.links.push_back(link);
        }
        for (std::size_t g = 0; g < groupCount; ++g)
        {
            Group group;
            std::size_t memberCount = 0;
            if (!(in >> token >> group.id >> std::quoted(group.title) >> group.x >> group.y >> group.width >> group.height >> group.color >> memberCount) ||
                token != "GROUP" || memberCount > MaxNodes) return malformed();
            for (std::size_t m = 0; m < memberCount; ++m)
            {
                int member = 0;
                if (!(in >> member)) return malformed();
                group.nodes.push_back(member);
            }
            graph.groups.push_back(std::move(group));
        }
        for(std::size_t t=0;t<tieCount;++t)
        {
            WireTie tie;if(!(in>>token>>tie.id>>tie.x>>tie.y>>tie.angle)||token!="TIE")return malformed();
            graph.ties.push_back(tie);
        }
    }
    if(version<4)candidate.graphs[3]=generatedStarterGraphs().graphs[3];
    for(std::size_t i=0;i<candidate.graphs.size();++i)if(!candidate.graphs[i].workflow)candidate.graphs[i].workflow=static_cast<int>(i)+1;
    if (!(in >> token) || token != "END" || (in >> token)) return malformed();
    if(version<16)for(auto& graph:candidate.graphs)
    {
        for(auto& link:graph.links)if(link.toPort>=1)
            for(const auto& node:graph.nodes)if(node.id==link.toNode&&node.kind==K::SpeedCurve){++link.toPort;break;}
        for(auto& node:graph.nodes)
        {
            const int addedRows=node.kind==K::SpeedCurve?2:node.kind==K::ScopeCheck?1:0;
            if(!addedRows||node.height<=0)continue;
            const float minimum=Layout::minimumHeight(node);
            if(node.height>=Layout::up(minimum-addedRows*Layout::PinSpacing)&&node.height<minimum)node.height=Layout::up(minimum);
        }
    }
    if(version<17)
    {
        for(auto& graph:candidate.graphs)migrateStarterLayout(graph);
        for(auto& graph:candidate.graphs)for(auto& node:graph.nodes)
            if(node.kind==K::RecoilOutput&&node.height>0&&node.height<Layout::minimumHeight(node))node.height=Layout::up(Layout::minimumHeight(node));
        migrateRecoilCalibration(candidate);
    }
    if(version<19)for(auto& graph:candidate.graphs)for(auto& node:graph.nodes)
        if(node.kind==K::NormalizeFps&&node.height>0&&node.height<Layout::minimumHeight(node))node.height=Layout::up(Layout::minimumHeight(node));
    if (!validate(candidate, error)) return false;
    if(version<18)
    {
        for(auto& graph:candidate.graphs)expandProcessingNodes(graph);
        if(!validate(candidate,error))return false;
    }
    if(version>=3&&version<19){shareMouseCalibration(candidate);if(!validate(candidate,error))return false;}
    if(version<12)
    {
        compactOutputNodes(candidate.graphs[3]);
        if(!validate(candidate,error))return false;
    }
    if(version<20){migrateWorkflowPages(candidate);if(!validate(candidate,error))return false;}
    workspace = std::move(candidate);
    return true;
}
bool Service::load(const std::string& path, std::string& error)
{
    error.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) && !ec) return true;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() < 0 || in.tellg() > static_cast<std::streamoff>(MaxWorkspaceBytes))
    { error = "Cannot read node workspace: " + path; return false; }
    std::string text(static_cast<std::size_t>(in.tellg()), '\0');
    in.seekg(0);
    if (!text.empty() && !in.read(&text[0], static_cast<std::streamsize>(text.size())))
    { error = "Cannot read node workspace: " + path; return false; }
    Workspace candidate;
    return deserialize(text, candidate, error) && publish(candidate, error);
}
bool Service::save(const std::string& path, std::string& error) const
{
    std::string text;
    if (!serialize(workspace(), text, error)) return false;
    const std::filesystem::path target(path), temp(path + ".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size())) || !out.flush())
        { error = "Cannot write node workspace: " + path; return false; }
        out.close();
        if (!out) { error = "Cannot finish writing node workspace: " + path; return false; }
    }
    // A same-directory replacement never exposes a partially written graph.
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    { error = "Cannot replace node workspace: " + path; return false; }
#else
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) { error = "Cannot replace node workspace: " + path; return false; }
#endif
    error.clear();
    return true;
}
}
