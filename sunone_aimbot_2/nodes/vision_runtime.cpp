#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include "nodes/vision_runtime.h"
#include "nodes/vision_outputs.h"
#include "nodes/preview_image.h"
#include "runtime/input_state.h"
#include "detection_buffer.h"
#include "config.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <unordered_map>

extern std::atomic<bool> capture_method_changed;
extern std::atomic<bool> capture_fps_changed;
extern std::atomic<bool> capture_borders_changed;
extern std::atomic<bool> capture_cursor_changed;
extern std::atomic<bool> capture_window_changed;
extern std::atomic<bool> detection_resolution_changed;
extern std::atomic<bool> detector_model_changed;
extern std::atomic<int> captureFps;

namespace NodeVision
{
namespace
{
std::atomic<std::int64_t> nextFrameVersion{1};
thread_local std::unordered_map<int,std::string> errors;
double seconds(std::chrono::steady_clock::time_point time)
{return std::chrono::duration<double>(time.time_since_epoch()).count();}
bool hasParameter(const NodeGraph::Node& node,const char* key)
{
    const auto& specs=NodeGraph::definition(node.kind).parameters;
    return std::any_of(specs.begin(),specs.end(),[&](const NodeGraph::ParameterSpec& spec){return std::string(spec.key)==key;});
}
std::string text(const NodeGraph::Node& node,const char* key)
{return NodeGraph::textParameter(node,key);}
}
std::shared_ptr<const FrameStorage> storage(const NodeGraph::FrameStream& frame)
{
    return frame&&frame->storage?std::static_pointer_cast<const FrameStorage>(frame->storage):nullptr;
}
NodeGraph::FrameStream makeFrame(const cv::Mat& image,const cv::Mat& source,
    std::chrono::steady_clock::time_point timestamp,std::uint64_t generation)
{
    auto native=std::make_shared<FrameStorage>();
    native->image=image.clone();
    if(!source.empty())native->source=source.clone();
    auto frame=std::make_shared<NodeGraph::FrameData>();
    frame->storage=std::move(native);frame->width=image.cols;frame->height=image.rows;
    frame->version=nextFrameVersion.fetch_add(1);frame->generation=generation;
    frame->timestampSeconds=seconds(timestamp);return frame;
}
#ifdef USE_CUDA
NodeGraph::FrameStream makeFrameGpu(const cv::cuda::GpuMat& image,
    std::chrono::steady_clock::time_point timestamp,std::uint64_t generation)
{
    auto native=std::make_shared<FrameStorage>();native->gpu=image.clone();
    auto frame=std::make_shared<NodeGraph::FrameData>();
    frame->storage=std::move(native);frame->width=image.cols;frame->height=image.rows;frame->gpu=true;
    frame->version=nextFrameVersion.fetch_add(1);frame->generation=generation;
    frame->timestampSeconds=seconds(timestamp);return frame;
}
#endif
cv::Mat cpuImage(const NodeGraph::FrameStream& frame)
{
    const auto native=storage(frame);if(!native)return {};
    std::lock_guard<std::mutex> lock(native->cpuMutex);
#ifdef USE_CUDA
    if(native->image.empty()&&!native->gpu.empty())native->gpu.download(native->image);
#endif
    return native->image;
}
cv::Mat sourceImage(const NodeGraph::FrameStream& frame)
{
    const auto native=storage(frame);if(!native)return {};
    return native->source.empty()?cpuImage(frame):native->source;
}
std::string resolveModelPath(const NodeGraph::Node& node)
{
    const auto model=std::filesystem::u8path(text(node,"model_path"));
    if(model.empty())return {};
    if(model.is_absolute()||std::filesystem::exists(model))return model.u8string();
    return (std::filesystem::path("models")/model).u8string();
}
void reportError(int nodeKey,const std::string& error){errors[nodeKey]=error;}

bool syncCaptureConfiguration(bool force)
{
    static std::mutex syncMutex;
    static std::uint64_t appliedRevision=0;
    static bool captureActive=false;
    static std::chrono::steady_clock::time_point checkedAt{};
    std::lock_guard<std::mutex> sync(syncMutex);
    const auto revision=NodeGraph::service().revision();
    const auto now=std::chrono::steady_clock::now();const bool dynamic=NodeGraph::service().hasDynamicProviders();
    if(!force&&revision==appliedRevision&&(!dynamic||now-checkedAt<std::chrono::milliseconds(16)))return captureActive;
    checkedAt=now;
    // Invalidate observations before providers reload after an execution edit.
    // An in-flight evaluation is rejected by its captured revision below.
    if(appliedRevision&&revision!=appliedRevision)detectionBuffer.clear();
    auto sources=NodeGraph::service().providerNodes(NodeGraph::NodeKind::CaptureSource);
    auto models=NodeGraph::service().providerNodes(NodeGraph::NodeKind::Inference);
    if(dynamic)
    {
        NodeGraph::Context context;context.elapsedSeconds=seconds(now);context.captureFps=captureFps.load();
        context.captureConfigurationChanged=[]{detectionBuffer.clear();};
        context.inputCaptured=editorInputCaptured.load();context.inputCapturedNow=[]{return editorInputCaptured.load();};
        context.aimHeld=aiming.load();context.shootHeld=shooting.load();context.zoomHeld=zooming.load();context.controlActive=graphAimActive.load();
        context.keyDown=[](int key){return !editorInputCaptured.load()&&key>0&&key<256&&(GetAsyncKeyState(key)&0x8000)!=0;};
        {std::lock_guard<std::mutex> cfg(configMutex);context.center={config.detection_resolution*0.5,config.detection_resolution*0.5};}
        sources.clear();models.clear();for(auto& node:NodeGraph::service().evaluateConfiguration(context))
            if(node.kind==NodeGraph::NodeKind::CaptureSource)sources.push_back(std::move(node));else if(node.kind==NodeGraph::NodeKind::Inference)models.push_back(std::move(node));
    }
    const NodeGraph::Node* source=nullptr;
    for(const auto& item:sources)if(NodeGraph::parameter(item,"enabled")!=0){source=&item;break;}
    const bool wasActive=captureActive;captureActive=source!=nullptr;appliedRevision=revision;
    if(!captureActive)
    {
        if(wasActive)detectionBuffer.clear();
        return false;
    }
    bool restart=false,resolution=false,fps=false,borders=false,cursor=false,window=false;
    {
        std::lock_guard<std::mutex> cfg(configMutex);
        auto assign=[&](auto& target,const auto& value,bool& changed){if(target!=value){target=value;changed=true;}};
        assign(config.capture_method,text(*source,"capture_method"),restart);
        assign(config.capture_target,text(*source,"capture_target"),restart);
        assign(config.capture_monitor_device,text(*source,"monitor_device"),restart);
        assign(config.capture_window_title,text(*source,"capture_window_title"),window);
        assign(config.virtual_camera_name,text(*source,"virtual_camera_name"),restart);
        assign(config.udp_ip,text(*source,"udp_ip"),restart);
        auto integer=[&](const char* key,int& target,bool& flag){if(hasParameter(*source,key))assign(target,static_cast<int>(NodeGraph::parameter(*source,key)),flag);};
        auto boolean=[&](const char* key,bool& target,bool& flag){if(hasParameter(*source,key))assign(target,NodeGraph::parameter(*source,key)!=0,flag);};
        integer("detection_resolution",config.detection_resolution,resolution);
        integer("capture_fps",config.capture_fps,fps);
        int monitorIndex=hasParameter(*source,"monitor_idx")?static_cast<int>(NodeGraph::parameter(*source,"monitor_idx")):config.monitor_idx;
        if(!config.capture_monitor_device.empty())
        {
            const auto monitors=NodeGraph::systemMonitors();
            if(monitors)for(const auto& monitor:*monitors)if(monitor.device==config.capture_monitor_device)
                monitorIndex=config.capture_method=="winrt"?monitor.winrtIndex:monitor.index;
        }
        assign(config.monitor_idx,monitorIndex,restart);
        integer("virtual_camera_width",config.virtual_camera_width,restart);
        integer("virtual_camera_heigth",config.virtual_camera_heigth,restart);
        integer("udp_port",config.udp_port,restart);
        boolean("capture_borders",config.capture_borders,borders);
        boolean("capture_cursor",config.capture_cursor,cursor);
#ifdef USE_CUDA
        boolean("capture_use_cuda",config.capture_use_cuda,restart);
#endif
        for(const auto& model:models)
        {
            if(NodeGraph::parameter(model,"enabled")==0)continue;
            const auto modelName=text(model,"model_path"),backend=text(model,"backend");
            if(!modelName.empty())config.ai_model=modelName;
            if(!backend.empty())config.backend=backend;
#ifndef USE_CUDA
            if(hasParameter(model,"dml_device_id"))config.dml_device_id=static_cast<int>(NodeGraph::parameter(model,"dml_device_id"));
#endif
            break;
        }
        // Depth is a graph computation. Capture only maintains a provider and
        // never masks pixels before the graph's Apply Mask node.
        config.depth_inference_enabled=false;
        config.depth_mask_enabled=false;
    }
    if(restart||resolution||window)detectionBuffer.clear();
    if(restart||!wasActive)capture_method_changed=true;
    if(resolution)detection_resolution_changed=true;
    if(fps)capture_fps_changed=true;
    if(borders)capture_borders_changed=true;
    if(cursor)capture_cursor_changed=true;
    if(window)capture_window_changed=true;
    return true;
}

NodeGraph::Actions processFrame(const NodeGraph::FrameStream& frame,const Infer& infer)
{
    NodeGraph::Actions inactive;
    if(!frame||!detectionBuffer.withCurrentGeneration(frame->generation,[]{}))return inactive;
    const auto evaluatedRevision=NodeGraph::service().revision();
    const auto current=[&]{return NodeGraph::service().revision()==evaluatedRevision&&
        detectionBuffer.withCurrentGeneration(frame->generation,[]{});};
    errors.clear();
    NodeGraph::Context context;
    context.useConfigurationSnapshot=true;
    context.inputCaptured=editorInputCaptured.load();
    context.inputCapturedNow=[]{return editorInputCaptured.load();};
    context.frame=frame;context.controlActive=!context.inputCaptured&&graphAimActive.load();
    context.aimHeld=!context.inputCaptured&&aiming.load();context.shootHeld=!context.inputCaptured&&shooting.load();context.zoomHeld=!context.inputCaptured&&zooming.load();
    context.center={frame->width*0.5,frame->height*0.5};
    context.captureFps=captureFps.load();
    context.elapsedSeconds=seconds(std::chrono::steady_clock::now());
    context.captureFrame=[frame](int,const NodeGraph::Node& node){
        return NodeGraph::parameter(node,"enabled")!=0?frame:NodeGraph::FrameStream{};
    };
    context.infer=[&](int key,const NodeGraph::FrameStream& input,const NodeGraph::Node& node){
        try{return infer(key,input,node);}
        catch(const std::exception& error){reportError(key,error.what());return NodeGraph::DetectionStream{};}
    };
    context.depth=inferDepth;context.depthToMask=depthToMask;context.applyMask=applyMask;
    context.preview=[&](const NodeGraph::Value& image,const NodeGraph::DetectionStream& detections,const NodeGraph::Node& node){
        return current()?makePreview(image,detections,node):NodeGraph::Preview{};
    };
    context.filterMask=filterMask;
    context.collect=[&](int key,const NodeGraph::FrameStream& input,const NodeGraph::DetectionStream& detections,const NodeGraph::Node& node){
        return current()&&collectDataset(key,input,detections,node);
    };
    context.screenshot=[&](int key,const NodeGraph::FrameStream& input,const NodeGraph::Node& node){
        return !editorInputCaptured.load()&&current()&&screenshot(key,input,node);
    };
    context.render=[&](int key,const NodeGraph::FrameStream& input,const NodeGraph::DetectionStream& detections,
        const NodeGraph::MaskStream& mask,const NodeGraph::FrameStream& depth,const NodeGraph::Node& node){
        return current()&&publishOverlayAtRevision(key,input,detections,mask,depth,node,evaluatedRevision);
    };
    context.keyDown=[](int key){return !editorInputCaptured.load()&&key>0&&key<256&&(GetAsyncKeyState(key)&0x8000)!=0;};
    context.nodeError=[](int key){const auto found=errors.find(key);return found==errors.end()?std::string{}:found->second;};
    const auto actions=NodeGraph::service().evaluateVision(context);
    if(!current()||actions.revision!=evaluatedRevision)return inactive;
    retainVisionOutputs(actions);
    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    std::vector<float> confidences;
    if(actions.visionDetections)
    {
        for(const auto& item:actions.visionDetections->objects)
        {
            const double x=std::clamp(item.x,0.0,static_cast<double>(frame->width));
            const double y=std::clamp(item.y,0.0,static_cast<double>(frame->height));
            const double right=std::clamp(item.x+item.width,x,static_cast<double>(frame->width));
            const double bottom=std::clamp(item.y+item.height,y,static_cast<double>(frame->height));
            if(right<=x||bottom<=y)continue;
            boxes.emplace_back(static_cast<int>(x),static_cast<int>(y),std::max(1,static_cast<int>(right-x)),std::max(1,static_cast<int>(bottom-y)));
            classes.push_back(item.classId);confidences.push_back(static_cast<float>(item.confidence));
        }
    }
    const auto timestamp=std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(frame->timestampSeconds)));
    // A disconnected primary output publishes an empty observation immediately.
    // Auxiliary collection/render outputs can continue without controlling aim.
    detectionBuffer.setForGeneration(frame->generation,boxes,classes,confidences,timestamp);
    return actions;
}
}
