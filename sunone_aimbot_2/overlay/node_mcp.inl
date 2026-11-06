// Included by node_editor.cpp: remote edits share the editor's transactions,
// geometry normalization, gesture ownership, Undo and persistence.
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <roapi.h>
#include <iomanip>
#include <sstream>
#include <type_traits>

namespace
{
namespace McpJson
{
using namespace winrt::Windows::Data::Json;
struct Apartment
{
    HRESULT initialized=RoInitialize(RO_INIT_MULTITHREADED);
    ~Apartment(){if(SUCCEEDED(initialized))RoUninitialize();}
};
void put(JsonObject& target,const wchar_t* key,const std::string& value){target.Insert(key,JsonValue::CreateStringValue(winrt::to_hstring(value)));}
void put(JsonObject& target,const wchar_t* key,const char* value){put(target,key,std::string(value));}
void put(JsonObject& target,const wchar_t* key,bool value){target.Insert(key,JsonValue::CreateBooleanValue(value));}
template<class T,std::enable_if_t<std::is_arithmetic_v<T>&&!std::is_same_v<T,bool>,int> =0>
void put(JsonObject& target,const wchar_t* key,T value){target.Insert(key,JsonValue::CreateNumberValue(static_cast<double>(value)));}
void put(JsonObject& target,const wchar_t* key,const JsonObject& value){target.Insert(key,value.as<IJsonValue>());}
void put(JsonObject& target,const wchar_t* key,const JsonArray& value){target.Insert(key,value.as<IJsonValue>());}
std::string text(const JsonObject& object,const wchar_t* key,const wchar_t* fallback=L""){return winrt::to_string(object.GetNamedString(key,fallback));}
std::string encode(const JsonObject& object){return winrt::to_string(object.Stringify());}
int identifier(double value,bool allowZero=false)
{
    if(!std::isfinite(value)||std::trunc(value)!=value||value<(allowZero?0:1)||value>1000000)throw std::invalid_argument("IDs must be whole numbers from 1 to 1000000 (or 0 for an optional filter).");
    return static_cast<int>(value);
}
JsonObject error(const std::string& message,const char* code="invalid_request"){JsonObject result;put(result,L"ok",false);put(result,L"error",message);put(result,L"code",code);return result;}
std::string version(const std::string& document)
{
    std::uint64_t hash=14695981039346656037ull;for(unsigned char byte:document){hash^=byte;hash*=1099511628211ull;}
    std::ostringstream out;out<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;return out.str();
}
JsonObject snapshot()
{
    std::string document,issue;if(!serialize(service().workspace(),document,issue))throw std::runtime_error(issue);
    JsonObject result;put(result,L"ok",true);put(result,L"document",document);put(result,L"version",version(document));
    put(result,L"execution_revision",service().revision());put(result,L"load_failed",editor.loadFailed);put(result,L"load_error",editor.loadError);
    put(result,L"saved",!editor.dirty);return result;
}
JsonObject catalog(const JsonObject& command)
{
    JsonArray nodes;
    std::vector<int> kinds;
    const bool filtered=command.HasKey(L"kinds");
    if(filtered)for(const auto& kind:command.GetNamedArray(L"kinds"))kinds.push_back(identifier(kind.GetNumber(),true));
    for(const auto& definition:definitions())
    {
        if(filtered&&std::find(kinds.begin(),kinds.end(),static_cast<int>(definition.kind))==kinds.end())continue;
        JsonObject item;put(item,L"kind",static_cast<int>(definition.kind));put(item,L"title",definition.title);put(item,L"category",definition.category);put(item,L"description",definition.description);
        put(item,L"can_bypass",canBypass(definition.kind));
        put(item,L"reroute",reroute(definition.kind));put(item,L"shared_calibration_source",definition.kind==NodeKind::MouseProfile);
        put(item,L"shared_calibration_reader",calibrationIdentifier(definition.kind)&&definition.kind!=NodeKind::MouseProfile);
        const auto original=defaultNode(definition.kind,1);JsonObject initial,parameters,textParameters,inputValues,inputTexts;
        put(initial,L"value",original.value);
        for(const auto& entry:original.parameters)put(parameters,winrt::to_hstring(entry.first).c_str(),entry.second);
        for(const auto& entry:original.textParameters)put(textParameters,winrt::to_hstring(entry.first).c_str(),entry.second);
        for(const auto& entry:original.inputValues)put(inputValues,winrt::to_hstring(std::to_string(entry.first)).c_str(),entry.second);
        for(const auto& entry:original.inputTexts)put(inputTexts,winrt::to_hstring(std::to_string(entry.first)).c_str(),entry.second);
        put(initial,L"parameters",parameters);put(initial,L"text_parameters",textParameters);put(initial,L"input_values",inputValues);put(initial,L"input_texts",inputTexts);put(item,L"defaults",initial);
        for(bool output:{false,true})
        {
            JsonArray pins;const auto& source=output?definition.outputs:definition.inputs;
            for(std::size_t index=0;index<source.size();++index)
            {
                JsonObject pin;put(pin,L"index",index);put(pin,L"name",source[index].name);put(pin,L"type",typeName(source[index].type));put(pin,L"option",source[index].option);
                if(source[index].defaultParameter)put(pin,L"default_parameter",source[index].defaultParameter);
                pins.Append(pin);
            }
            put(item,output?L"outputs":L"inputs",pins);
        }
        JsonArray numbers,texts;
        for(const auto& spec:definition.parameters)
        {
            JsonObject field;put(field,L"key",spec.key);put(field,L"label",spec.label);put(field,L"default",spec.defaultValue);put(field,L"min",spec.minValue);put(field,L"max",spec.maxValue);
            put(field,L"type",spec.kind==ParameterKind::Boolean?"bool":spec.kind==ParameterKind::Integer?"int":"float");
            if(spec.choices)put(field,L"choices",spec.choices);numbers.Append(field);
        }
        for(const auto& spec:definition.textParameters)
        {JsonObject field;put(field,L"key",spec.key);put(field,L"label",spec.label);put(field,L"default",spec.defaultValue);put(field,L"max_length",spec.maxLength);texts.Append(field);}
        put(item,L"parameters",numbers);put(item,L"text_parameters",texts);nodes.Append(item);
    }
    JsonObject result,limits;put(result,L"ok",true);put(result,L"nodes",nodes);put(result,L"format_version",CurrentFormatVersion);
    put(limits,L"nodes_per_workflow",MaxNodes);put(limits,L"links_per_workflow",MaxLinks);put(limits,L"groups_per_workflow",MaxGroups);put(limits,L"workflows",MaxGraphs);put(limits,L"file_bytes",MaxWorkspaceBytes);put(limits,L"grid",GridSize);
    put(limits,L"path_points",MaxPathPoints);put(limits,L"drawing_commands",MaxDrawingCommands);put(limits,L"drawing_vertices",MaxDrawingVertices);put(result,L"limits",limits);return result;
}
JsonObject liveValue(const Value& value)
{
    JsonObject result;put(result,L"type",typeName(value.type));put(result,L"valid",value.valid);put(result,L"error",value.error);
    if(value.preview)
    {
        JsonObject preview;put(preview,L"type",typeName(value.preview->sourceType));put(preview,L"width",value.preview->sourceWidth);put(preview,L"height",value.preview->sourceHeight);
        if(value.preview->width==0)put(preview,L"text",value.preview->text);
        put(result,L"preview",preview);
    }
    switch(value.type)
    {
    case ValueType::Boolean:put(result,L"value",value.boolean);break;
    case ValueType::Float:if(std::isfinite(value.number))put(result,L"value",value.number);break;
    case ValueType::Integer:put(result,L"value",value.integer);break;
    case ValueType::String:put(result,L"value",value.text);break;
    case ValueType::Point:put(result,L"x",value.point.x);put(result,L"y",value.point.y);break;
    case ValueType::Points:
    {
        JsonArray points;if(value.points)for(std::size_t i=0;i<std::min<std::size_t>(64,value.points->size());++i)
        {JsonObject point;put(point,L"x",(*value.points)[i].x);put(point,L"y",(*value.points)[i].y);points.Append(point);}
        put(result,L"count",value.points?value.points->size():0);put(result,L"points",points);break;
    }
    case ValueType::Drawing:
        put(result,L"commands",value.drawing?value.drawing->commands.size():0);put(result,L"vertices",value.drawing?value.drawing->vertices:0);break;
    case ValueType::Detections:if(value.detections){put(result,L"count",value.detections->objects.size());put(result,L"frame_version",value.detections->version);put(result,L"timestamp_seconds",value.detections->timestampSeconds);}break;
    case ValueType::Frame:case ValueType::Depth:if(value.frame){put(result,L"width",value.frame->width);put(result,L"height",value.frame->height);put(result,L"frame_version",value.frame->version);put(result,L"timestamp_seconds",value.frame->timestampSeconds);put(result,L"gpu",value.frame->gpu);}break;
    case ValueType::Mask:if(value.mask){put(result,L"width",value.mask->width);put(result,L"height",value.mask->height);put(result,L"frame_version",value.mask->version);}break;
    case ValueType::Target:put(result,L"target_valid",value.target.valid);put(result,L"observed",value.target.observed);put(result,L"class_id",value.target.classId);put(result,L"track_id",value.target.trackId);put(result,L"confidence",value.target.confidence);put(result,L"x",value.target.point.x);put(result,L"y",value.target.point.y);break;
    case ValueType::Movement:{long long x=0,y=0;for(const auto& step:value.movement.steps){x+=step.dx;y+=step.dy;}put(result,L"steps",value.movement.steps.size());put(result,L"dx",x);put(result,L"dy",y);break;}
    case ValueType::Motion:put(result,L"x",value.motion.position.x);put(result,L"y",value.motion.position.y);put(result,L"velocity_x",value.motion.velocity.x);put(result,L"velocity_y",value.motion.velocity.y);put(result,L"ready",value.motion.ready);break;
    case ValueType::Monitor:if(value.monitor){put(result,L"name",value.monitor->name);put(result,L"index",value.monitor->index);put(result,L"width",value.monitor->width);put(result,L"height",value.monitor->height);}break;
    case ValueType::Monitors:if(value.monitors)put(result,L"count",value.monitors->size());break;
    case ValueType::Calibration:if(value.calibration){put(result,L"aim_enabled",value.calibration->aimEnabled);put(result,L"pixels_per_count_x",value.calibration->pixelsPerCount.x);put(result,L"pixels_per_count_y",value.calibration->pixelsPerCount.y);}break;
    default:break;
    }
    return result;
}
}
}
#include "node_mcp_application.inl"
std::string NodeEditor_McpCall(const std::string& request,bool userEditing)
{
    using namespace McpJson;
    thread_local Apartment apartment;
    try
    {
        if(request.size()>16*1024*1024)return encode(error("Request exceeds 16 MiB."));
        const auto command=JsonObject::Parse(winrt::to_hstring(request));const auto operation=text(command,L"op");
        if(operation=="read")return encode(snapshot());
        if(operation=="catalog")return encode(catalog(command));
        if(operation=="application")return encode(applicationSnapshot());
        if(operation=="model_export")return encode(modelExport(command,userEditing));
        if(operation=="application_edit"||operation=="application_action")
        {
            if(userEditing||editor.pointerMove||editor.keyboardMove||editor.resizeNode||editor.bendGraph>=0||editor.wireNode||editor.rightGesture||editor.panButton>=0)return encode(error("The user is editing. Retry after the current interaction.","busy"));
            if(operation=="application_edit")return encode(applicationEdit(command));
            std::string issue;if(!Overlay_AgentAction(text(command,L"action"),text(command,L"tab"),issue))return encode(error(issue));
            auto result=applicationSnapshot();put(result,L"queued",true);return encode(result);
        }
        if(operation=="sources")
        {
            const auto source=text(command,L"source",L"models");JsonObject result;JsonArray entries;put(result,L"ok",true);
            if(source=="models")
            {
                // Reuse the menu's discovery rules without disturbing an open
                // model picker or changing its selection/scroll state.
                struct RestoreMenu{ModelMenu previous=std::move(modelMenu);~RestoreMenu(){modelMenu=std::move(previous);}} restore;
#ifdef USE_CUDA
                constexpr bool engines=true;put(result,L"native_backend","TRT");
#else
                constexpr bool engines=false;put(result,L"native_backend","DML");
#endif
                refreshModelChoices(command.GetNamedBoolean(L"depth",false),engines);
                for(const auto& model:modelMenu.choices)
                {JsonObject item;put(item,L"name",model.name);put(item,L"path",model.path);put(item,L"legacy",model.legacy);entries.Append(item);}
                put(result,L"warning",modelMenu.error);
            }
            else if(source=="monitors")
            {
                if(const auto monitors=systemMonitors())for(const auto& monitor:*monitors)
                {JsonObject item;put(item,L"index",monitor.index);put(item,L"winrt_index",monitor.winrtIndex);put(item,L"name",monitor.name);put(item,L"device",monitor.device);put(item,L"primary",monitor.primary);put(item,L"x",monitor.x);put(item,L"y",monitor.y);put(item,L"width",monitor.width);put(item,L"height",monitor.height);entries.Append(item);}
            }
            else return encode(error("Unknown source list. Choose models or monitors."));
            put(result,L"entries",entries);return encode(result);
        }
        if(operation=="templates")
        {
            std::string document,issue;if(!serialize(defaults(),document,issue))return encode(error(issue));
            JsonObject result;put(result,L"ok",true);put(result,L"document",document);return encode(result);
        }
        if(operation=="diagnostics")
        {
            service().requestTelemetry(std::chrono::milliseconds(1000));
            const auto telemetry=service().telemetry();const auto workspace=service().workspaceMetadata();const auto revision=service().revision();const auto now=std::chrono::steady_clock::now();JsonArray graphs;
            const int requested=identifier(command.GetNamedNumber(L"workflow",0),true);
            const int requestedNode=identifier(command.GetNamedNumber(L"node",0),true);
            for(std::size_t g=0;g<workspace.graphs.size();++g)
            {
                const int id=workflowId(workspace.graphs[g],g);if(requested&&requested!=id)continue;
                JsonObject graph;JsonArray values;put(graph,L"workflow",id);
                put(graph,L"name",workspace.graphs[g].workflowName);put(graph,L"enabled",workspace.graphs[g].enabled);
                const auto sampled=telemetry.graphSampledAt[g];const bool hasSample=sampled.time_since_epoch().count()!=0;
                const double age=hasSample?std::chrono::duration<double,std::milli>(now-sampled).count():-1;
                put(graph,L"sample_age_ms",age);put(graph,L"current",hasSample&&age<=1000&&telemetry.graphRevisions[g]==revision);
                for(const auto& node:telemetry.graphs[g])if(!requestedNode||requestedNode==node.nodeId)
                {
                    JsonObject item;put(item,L"node",node.nodeId);put(item,L"active",node.active);put(item,L"signal_active",node.signalActive);put(item,L"read_inputs",std::to_string(node.readInputs));put(item,L"value",liveValue(node.value));
                    const bool evaluated=node.evaluatedAt.time_since_epoch().count()!=0;
                    put(item,L"evaluated",node.evaluated);put(item,L"cached",evaluated&&!node.evaluated);
                    put(item,L"value_age_ms",evaluated?std::chrono::duration<double,std::milli>(now-node.evaluatedAt).count():-1);values.Append(item);
                }
                put(graph,L"nodes",values);graphs.Append(graph);
            }
            if(requested&&graphs.Size()==0)return encode(error("Workflow not found.","not_found"));
            // A node's Boolean status alone cannot establish actual movement
            // or calibrated recoil. Report the already sampled output actions.
            JsonObject outputs,movement;put(outputs,L"scope","workspace");
            const auto domainCurrent=[&](bool vision){
                bool found=false;
                for(std::size_t g=0;g<workspace.graphs.size();++g)
                {
                    const auto& graph=workspace.graphs[g];if(!graph.active||!graph.enabled||(graph.kind==GraphKind::Vision)!=vision)continue;
                    found=true;const auto sampled=telemetry.graphSampledAt[g];
                    if(sampled.time_since_epoch().count()==0||now-sampled>std::chrono::seconds(1)||telemetry.graphRevisions[g]!=revision)return false;
                }
                return found;
            };
            put(outputs,L"control_current",domainCurrent(false));put(outputs,L"vision_current",domainCurrent(true));
            const auto& actions=telemetry.actions;put(outputs,L"aim",actions.aim);put(outputs,L"auto_shoot",actions.autoShoot);put(outputs,L"recoil",actions.recoil);
            put(outputs,L"recoil_strength",actions.recoilStrength);put(outputs,L"recoil_pixel_delta_y",actions.recoilPixelDeltaY);
            long long dx=0,dy=0;std::size_t steps=0;for(const auto& output:actions.movements)for(const auto& step:output.steps){dx+=step.dx;dy+=step.dy;++steps;}
            put(movement,L"dx",dx);put(movement,L"dy",dy);put(movement,L"steps",steps);put(movement,L"outputs",actions.movements.size());put(outputs,L"movement",movement);
            put(outputs,L"vision_detections",actions.visionDetections?actions.visionDetections->objects.size():0);
            put(outputs,L"active_providers",actions.activeProviderKeys.size());put(outputs,L"active_depth",actions.activeDepthKeys.size());put(outputs,L"active_masks",actions.activeMaskKeys.size());put(outputs,L"active_renders",actions.activeRenderKeys.size());
            JsonObject result;put(result,L"ok",true);put(result,L"graphs",graphs);put(result,L"outputs",outputs);put(result,L"execution_revision",revision);put(result,L"sampling_lease_ms",1000);return encode(result);
        }
        if(operation!="commit"&&operation!="history"&&operation!="focus")return encode(error("Unknown editor operation."));
        const auto current=snapshot();
        if(text(command,L"expected_version")!=text(current,L"version"))return encode(error("Version conflict. Read the current workspace and retry; no changes were applied.","version_conflict"));
        if(userEditing||editor.pointerMove||editor.keyboardMove||editor.resizeNode||editor.bendGraph>=0||editor.wireNode||editor.rightGesture||editor.panButton>=0)
            return encode(error("The user is editing the graph. Retry after the current interaction; no changes were applied.","busy"));
        if(operation=="focus")
        {
            const int workflow=identifier(command.GetNamedNumber(L"workflow"));const auto workspace=service().workspaceMetadata();const int graph=activePage(workspace,workflow);
            const int node=identifier(command.GetNamedNumber(L"node",0),true);if(graph<0||(node&&!findNode(service().workspace().graphs[graph],node)))return encode(error("Workflow or node not found.","not_found"));
            Overlay_OpenGraphNode(graph,node);return encode(snapshot());
        }
        if(operation=="history")
        {
            const bool redo=command.GetNamedBoolean(L"redo",false);
            if(redo?editor.redo.empty():editor.undo.empty())return encode(error(redo?"Nothing to redo.":"Nothing to undo.","empty_history"));
            history(redo);NodeEditor_Flush();return encode(snapshot());
        }
        Workspace candidate;std::string issue;
        if(!deserialize(text(command,L"document"),candidate,issue))return encode(error(issue));
        if(candidate.sourceVersion!=CurrentFormatVersion)return encode(error("Import old graphs through the editor before editing over MCP."));
        for(auto& graph:candidate.graphs){normalizeLayout(graph,false);syncGroupMembership(graph);}
        if(command.HasKey(L"arrange"))for(const auto& id:command.GetNamedArray(L"arrange"))
        {const int graph=activePage(candidate,identifier(id.GetNumber()));if(graph<0)return encode(error("Cannot arrange a missing workflow."));Layout::arrange(candidate.graphs[graph]);syncGroupMembership(candidate.graphs[graph]);}
        if(!validate(candidate,issue))return encode(error(issue));
        const bool dryRun=command.GetNamedBoolean(L"dry_run",false);
        if(dryRun)
        {
            std::string document;if(!serialize(candidate,document,issue))return encode(error(issue));JsonObject result;put(result,L"ok",true);put(result,L"applied",false);put(result,L"document",document);put(result,L"version",text(current,L"version"));return encode(result);
        }
        const bool recover=command.GetNamedBoolean(L"recover",false);
        if(editor.loadFailed&&!recover)return encode(error("Autosave is paused after a load error. Use import with recover=true to replace the invalid workspace explicitly."));
        const bool failed=editor.loadFailed;editor.loadFailed=false;
        if(!commitWorkflowEdit(std::move(candidate),issue)){editor.loadFailed=failed;return encode(error(issue));}
        editor.loadError.clear();
        for(std::size_t g=0;g<MaxGraphs;++g){clearSelection(static_cast<int>(g));editor.selectedGroup[g]=0;editor.canvasRoutes[g]=CanvasRoutes{};editor.focusSelection[g]=false;}
        editor.activeGraph=-1;NodeEditor_Flush();
        auto result=snapshot();put(result,L"applied",true);if(editor.saveFailed)put(result,L"warning",editor.message);return encode(result);
    }
    catch(const winrt::hresult_error&){return encode(error("Invalid JSON request or field type."));}
    catch(const std::exception& issue){return encode(error(issue.what()));}
}
