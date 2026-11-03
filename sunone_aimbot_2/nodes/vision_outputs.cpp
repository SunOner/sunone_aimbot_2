#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "nodes/vision_outputs.h"
#include "nodes/vision_runtime.h"
#include "scr/data_collector.h"
#include "runtime/input_state.h"
#include "detection_buffer.h"
#include "mouse.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#ifdef USE_CUDA
#include "depth/depth_anything_trt.h"
#include "tensorrt/nvinf.h"
#endif

namespace NodeVision
{
namespace
{
using namespace NodeGraph;
using Clock=std::chrono::steady_clock;
struct DepthState
{
    std::mutex mutex;
    std::string modelPath;
    std::string lastError;
    FrameStream cached;
    Clock::time_point updatedAt{};
    Clock::time_point attemptedAt{};
#ifdef USE_CUDA
    depth_anything::DepthAnythingTrt model;
#endif
};
struct MaskHoldState
{
    cv::Mat ttl;
    std::uint64_t generation=0;
    std::int64_t version=-1;
    std::map<std::string,double> settings;
};
struct MaskBuildState
{
    std::mutex mutex;
    MaskStream cached;
    Clock::time_point updatedAt{};
    std::map<std::string,double> settings;
};
struct ScreenshotState
{
    Clock::time_point savedAt{};
    std::uint64_t sequence=0;
    std::map<std::string,double> parameters;
    std::map<std::string,std::string> textParameters;
};
std::mutex statesMutex;
std::unordered_map<int,std::shared_ptr<DepthState>> depthStates;
std::unordered_map<int,MaskHoldState> maskHoldStates;
std::unordered_map<int,std::shared_ptr<MaskBuildState>> maskBuildStates;
std::map<int,std::shared_ptr<const OverlayPacket>> renderPackets;
std::unordered_map<int,ScreenshotState> screenshotStates;

int visionKey(const Node& node){return node.runtimeKey?node.runtimeKey:static_cast<int>(GraphKind::Vision)*1000001+node.id;}
FrameStream frameError(const FrameStream& input,const std::string& error)
{
    auto output=input?std::make_shared<FrameData>(*input):std::make_shared<FrameData>();
    output->storage.reset();output->gpu=false;output->error=error;return output;
}
MaskStream maskError(const FrameStream& input,const std::string& error)
{
    auto output=std::make_shared<MaskData>();
    if(input){output->width=input->width;output->height=input->height;output->version=input->version;output->generation=input->generation;output->timestampSeconds=input->timestampSeconds;}
    output->error=error;return output;
}
FrameStream withImage(const FrameStream& source,const cv::Mat& image)
{
    auto native=std::make_shared<FrameStorage>();native->image=image;
    native->source=sourceImage(source);
    auto output=std::make_shared<FrameData>(*source);output->storage=native;output->gpu=false;
    output->width=image.cols;output->height=image.rows;output->error.clear();return output;
}
std::shared_ptr<const MaskStorage> maskStorage(const MaskStream& mask)
{return mask&&mask->storage?std::static_pointer_cast<const MaskStorage>(mask->storage):nullptr;}
cv::Mat normalizedMask(const MaskStream& mask,cv::Size size)
{
    const auto native=maskStorage(mask);
    if(!native||native->mask.empty())return {};
    cv::Mat output=native->mask;
    if(output.type()!=CV_8UC1)output.convertTo(output,CV_8UC1);
    if(output.size()!=size)cv::resize(output,output,size,0,0,cv::INTER_NEAREST);
    return output;
}
cv::Mat heldMask(const cv::Mat& current,const Node& node,std::int64_t version,std::uint64_t generation)
{
    const int hold=std::clamp(static_cast<int>(parameter(node,"depth_mask_hold_frames")),0,1000);
    if(hold==0)return current;
    std::lock_guard<std::mutex> lock(statesMutex);auto& state=maskHoldStates[visionKey(node)];
    if(state.generation!=generation||state.ttl.size()!=current.size()||state.settings!=node.parameters)
    {state=MaskHoldState{};state.settings=node.parameters;}
    state.generation=generation;
    if(state.ttl.empty())state.ttl=cv::Mat::zeros(current.size(),CV_16UC1);
    if(state.version!=version)
    {cv::subtract(state.ttl,cv::Scalar(1),state.ttl);state.ttl.setTo(cv::Scalar(hold),current);state.version=version;}
    cv::Mat result;cv::compare(state.ttl,cv::Scalar(0),result,cv::CMP_GT);return result;
}
}

NodeGraph::FrameStream inferDepth(int nodeKey,const FrameStream& frame,const Node& node)
{
    if(!frame||!frame->storage)return frameError(frame,"Depth inference requires an image.");
#ifndef USE_CUDA
    const std::string error="Depth inference requires the CUDA/TensorRT backend in this build.";
    reportError(nodeKey,error);return frameError(frame,error);
#else
    std::shared_ptr<DepthState> state;
    {
        std::lock_guard<std::mutex> lock(statesMutex);
        auto& entry=depthStates[nodeKey];if(!entry)entry=std::make_shared<DepthState>();state=entry;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto now=Clock::now();
    const std::string modelPath=textParameter(node,"model_path");
    try
    {
        if(state->modelPath!=modelPath)
        {state->model.reset();state->cached.reset();state->lastError.clear();state->attemptedAt={};state->modelPath=modelPath;}
        if(!state->model.ready()&&!state->lastError.empty()&&now-state->attemptedAt<std::chrono::seconds(2))
        {reportError(nodeKey,state->lastError);return frameError(frame,state->lastError);}
        state->attemptedAt=now;
        if(modelPath.empty())throw std::runtime_error("Choose a depth model path on this node.");
        if(!state->model.ready())
        {
            state->model.reset();state->cached.reset();
            if(!state->model.initialize(modelPath,gLogger))throw std::runtime_error(state->model.lastError());
            state->lastError.clear();
        }
        const double fps=parameter(node,"depth_fps");
        const bool sameGeneration=state->cached&&state->cached->generation==frame->generation&&state->cached->width==frame->width&&state->cached->height==frame->height;
        if(sameGeneration&&fps>0&&std::chrono::duration<double>(now-state->updatedAt).count()<1.0/fps)return state->cached;
        const cv::Mat image=cpuImage(frame);
        if(image.empty())throw std::runtime_error("Could not read pixels for depth inference.");
        cv::Mat depth=state->model.predictDepth(image);
        if(depth.empty())throw std::runtime_error(state->model.lastError().empty()?"Depth inference returned an empty image.":state->model.lastError());
        state->cached=withImage(frame,depth);state->updatedAt=now;state->lastError.clear();reportError(nodeKey,{});return state->cached;
    }
    catch(const std::exception& error){state->lastError=error.what();reportError(nodeKey,error.what());return frameError(frame,error.what());}
#endif
}

NodeGraph::MaskStream depthToMask(const FrameStream& depth,const Node& node)
{
    const int nodeKey=visionKey(node);
    try
    {
        if(!depth||!depth->storage)throw std::runtime_error(depth&&!depth->error.empty()?depth->error:"Depth to Mask requires a depth image.");
        std::shared_ptr<MaskBuildState> state;
        {std::lock_guard<std::mutex> lock(statesMutex);auto& entry=maskBuildStates[visionKey(node)];if(!entry)entry=std::make_shared<MaskBuildState>();state=entry;}
        std::lock_guard<std::mutex> stateLock(state->mutex);
        const auto now=Clock::now();const double requestedFps=parameter(node,"depth_mask_fps"),fps=requestedFps>0?requestedFps:5;
        if(state->cached&&state->cached->generation==depth->generation&&state->cached->width==depth->width&&state->cached->height==depth->height&&
            state->settings==node.parameters&&std::chrono::duration<double>(now-state->updatedAt).count()<1.0/fps)
        {reportError(nodeKey,{});return state->cached;}
        cv::Mat pixels=cpuImage(depth),normalized;
        if(pixels.empty()||pixels.channels()!=1)throw std::runtime_error("Depth to Mask requires a single-channel depth image.");
        if(pixels.type()==CV_8UC1)normalized=pixels;else cv::normalize(pixels,normalized,0,255,cv::NORM_MINMAX,CV_8UC1);
        const int percent=std::clamp(static_cast<int>(parameter(node,"depth_mask_near_percent")),0,100);
        const bool invert=parameter(node,"depth_mask_invert")!=0;
        int histogram[256]={};for(int y=0;y<normalized.rows;++y){const auto* row=normalized.ptr<std::uint8_t>(y);for(int x=0;x<normalized.cols;++x)++histogram[row[x]];}
        const int wanted=std::max(1,normalized.rows*normalized.cols*percent/100);
        int threshold=invert?255:0,total=0;
        for(int i=0;i<256;++i){const int value=invert?255-i:i;total+=histogram[value];if(total>=wanted){threshold=value;break;}}
        cv::Mat binary;
        if(percent==0)binary=cv::Mat::zeros(normalized.size(),CV_8UC1);
        else cv::compare(normalized,threshold,binary,invert?cv::CMP_GE:cv::CMP_LE);
        const int expand=std::clamp(static_cast<int>(parameter(node,"depth_mask_expand")),0,128);
        if(expand)cv::dilate(binary,binary,cv::getStructuringElement(cv::MORPH_ELLIPSE,cv::Size(2*expand+1,2*expand+1)));
        auto native=std::make_shared<MaskStorage>();native->mask=binary;
        auto output=std::make_shared<MaskData>();output->storage=native;output->width=binary.cols;output->height=binary.rows;
        output->version=depth->version;output->generation=depth->generation;output->timestampSeconds=depth->timestampSeconds;
        state->cached=output;state->settings=node.parameters;state->updatedAt=now;
        reportError(nodeKey,{});return output;
    }
    catch(const std::exception& error){reportError(nodeKey,error.what());return maskError(depth,error.what());}
}

NodeGraph::FrameStream applyMask(const FrameStream& frame,const MaskStream& mask,const Node& node)
{
    const int nodeKey=visionKey(node);
    if(parameter(node,"enabled")==0)return frame;
    try
    {
        if(!frame||!frame->storage||!mask||!mask->storage)throw std::runtime_error("Apply Mask requires both an image and a mask.");
        if(frame->generation!=mask->generation)throw std::runtime_error("Image and mask belong to different capture generations.");
        const cv::Mat image=cpuImage(frame);cv::Mat binary=normalizedMask(mask,image.size());
        if(image.empty()||binary.empty())throw std::runtime_error("Apply Mask received empty pixels.");
        cv::Mat output=image.clone();output.setTo(cv::Scalar::all(0),heldMask(binary,node,frame->version,frame->generation));
        reportError(nodeKey,{});return withImage(frame,output);
    }
    catch(const std::exception& error){reportError(nodeKey,error.what());return frameError(frame,error.what());}
}

NodeGraph::DetectionStream filterMask(const DetectionStream& detections,const MaskStream& mask,const Node& node)
{
    if(!detections||parameter(node,"enabled")==0)return detections;
    const int key=visionKey(node);
    if(!mask||!mask->storage||mask->generation!=detections->generation)
    {reportError(key,"Filter Mask requires a mask from the same capture generation.");return {};}
    try
    {
        const cv::Mat current=normalizedMask(mask,cv::Size(static_cast<int>(detections->width),static_cast<int>(detections->height)));
        if(current.empty())throw std::runtime_error("Filter Mask received empty mask pixels.");
        const cv::Mat suppression=heldMask(current,node,detections->version,detections->generation);
        auto output=std::make_shared<DetectionFrame>(*detections);output->objects.clear();
        for(const auto& detection:detections->objects)
        {
            const int left=static_cast<int>(std::floor(std::clamp(detection.x,0.0,static_cast<double>(suppression.cols))));
            const int top=static_cast<int>(std::floor(std::clamp(detection.y,0.0,static_cast<double>(suppression.rows))));
            const int right=static_cast<int>(std::ceil(std::clamp(detection.x+detection.width,0.0,static_cast<double>(suppression.cols))));
            const int bottom=static_cast<int>(std::ceil(std::clamp(detection.y+detection.height,0.0,static_cast<double>(suppression.rows))));
            const cv::Rect clipped(left,top,std::max(0,right-left),std::max(0,bottom-top));
            if(clipped.empty()||cv::countNonZero(suppression(clipped))==0)output->objects.push_back(detection);
        }
        reportError(key,{});return output;
    }
    catch(const std::exception& error){reportError(key,error.what());return {};}
}

bool collectDataset(int nodeKey,const FrameStream& frame,const DetectionStream& detections,const Node& node)
{
    if(!frame||!frame->storage)return false;
    Config settings{};settings.collect_data_while_playing=true;
    settings.collect_only_when_aimbot_running=parameter(node,"collect_only_when_aimbot_running")!=0;
    settings.collect_only_when_targets_present=parameter(node,"collect_only_when_targets_present")!=0;
    settings.collect_save_every_n_frames=static_cast<int>(parameter(node,"collect_save_every_n_frames"));
    settings.collect_jpeg_quality=static_cast<int>(parameter(node,"collect_jpeg_quality"));
    settings.collect_output_dir=textParameter(node,"collect_output_dir");
    settings.auto_label_data=parameter(node,"auto_label_data")!=0;
    settings.auto_label_min_conf=static_cast<float>(parameter(node,"auto_label_min_conf"));
    settings.auto_label_max_boxes=static_cast<int>(parameter(node,"auto_label_max_boxes"));
    settings.auto_label_record_classes=textParameter(node,"auto_label_record_classes");
    if(settings.auto_label_data&&(!detections||detections->version!=frame->version||detections->generation!=frame->generation))return false;
    std::vector<cv::Rect> boxes;std::vector<int> classes;std::vector<float> confidences;
    if(detections)for(const auto& detection:detections->objects)
    {
        boxes.emplace_back(static_cast<int>(detection.x),static_cast<int>(detection.y),static_cast<int>(detection.width),static_cast<int>(detection.height));
        classes.push_back(detection.classId);confidences.push_back(static_cast<float>(detection.confidence));
    }
    try
    {
        const std::string modelName=detections&&!detections->modelName.empty()?detections->modelName:textParameter(node,"model_name");
        const cv::Mat image=cpuImage(frame);
        if(image.empty())throw std::runtime_error("Could not read pixels for this dataset node.");
        const bool saved=cvm::CollectDataSampleForNode(nodeKey,{},modelName.c_str(),image,boxes,classes,confidences,
            graphAimActive.load(),settings,parameter(node,"collect_cooldown_ms"));
        const auto status=cvm::GetDataCollectionNodeUiState(nodeKey).status;
        reportError(nodeKey,!saved&&status.find("failed")!=std::string::npos?status:std::string());return saved;
    }
    catch(const std::exception& error){reportError(nodeKey,error.what());return false;}
}

bool screenshot(int nodeKey,const FrameStream& frame,const Node& node)
{
    try
    {
        if(!frame||!frame->storage)throw std::runtime_error("Screenshot requires an image.");
        const auto now=Clock::now();std::uint64_t sequence=0;
        {
            std::lock_guard<std::mutex> lock(statesMutex);auto& state=screenshotStates[nodeKey];
            if(state.parameters!=node.parameters||state.textParameters!=node.textParameters)
            {state.savedAt={};state.parameters=node.parameters;state.textParameters=node.textParameters;}
            const double cooldown=std::clamp(parameter(node,"cooldown_ms"),0.0,60000.0)/1000.0;
            if(state.savedAt.time_since_epoch().count()&&std::chrono::duration<double>(now-state.savedAt).count()<cooldown)
            {reportError(nodeKey,{});return false;}
            state.savedAt=now;sequence=++state.sequence;
        }
        const cv::Mat input=cpuImage(frame);cv::Mat image;
        if(input.empty())throw std::runtime_error("Could not read pixels for this screenshot node.");
        if(input.channels()==4)cv::cvtColor(input,image,cv::COLOR_BGRA2BGR);
        else if(input.channels()==1||input.channels()==3)image=input;
        else throw std::runtime_error("Screenshot requires a grayscale, BGR, or BGRA image.");
        std::string format=textParameter(node,"format");
        std::transform(format.begin(),format.end(),format.begin(),[](unsigned char value){return static_cast<char>(std::tolower(value));});
        if(format=="jpeg")format="jpg";
        if(format!="png"&&format!="jpg")throw std::runtime_error("Screenshot format must be png or jpg.");
        const auto directory=std::filesystem::u8path(textParameter(node,"output_dir"));
        if(directory.empty())throw std::runtime_error("Choose an output folder for this screenshot node.");
        std::filesystem::create_directories(directory);
        const auto timestamp=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const auto path=directory/(std::to_string(timestamp)+"_n"+std::to_string(nodeKey)+"_s"+std::to_string(sequence)+"."+format);
        const int quality=std::clamp(static_cast<int>(parameter(node,"quality")),1,100);
        const std::vector<int> options=format=="jpg"?std::vector<int>{cv::IMWRITE_JPEG_QUALITY,quality}:
            std::vector<int>{cv::IMWRITE_PNG_COMPRESSION,static_cast<int>(std::lround((100-quality)*9.0/99.0))};
        if(!cv::imwrite(path.string(),image,options))throw std::runtime_error("Screenshot image write failed.");
        reportError(nodeKey,{});return true;
    }
    catch(const std::exception& error){reportError(nodeKey,error.what());return false;}
}

bool publishOverlay(int nodeKey,const FrameStream& frame,const DetectionStream& detections,const MaskStream& mask,const FrameStream& depth,const Node& node)
{return publishOverlayAtRevision(nodeKey,frame,detections,mask,depth,node,NodeGraph::service().revision());}

bool publishOverlayAtRevision(int nodeKey,const FrameStream& frame,const DetectionStream& detections,const MaskStream& mask,const FrameStream& depth,const Node& node,std::uint64_t evaluatedRevision)
{
    if(!frame||!frame->storage||evaluatedRevision!=NodeGraph::service().revision())return false;
    auto packet=std::make_shared<OverlayPacket>();packet->nodeKey=nodeKey;packet->graphRevision=evaluatedRevision;packet->frame=frame;packet->depth=depth;
    packet->detections=detections;packet->mask=mask;packet->style=node;packet->publishedAt=Clock::now();
    if(globalMouseThread)
    {
        if(parameter(node,"game_overlay_draw_future")!=0)packet->futurePositions=globalMouseThread->getFuturePositions();
        if(parameter(node,"game_overlay_draw_wind_tail")!=0)packet->windTrail=globalMouseThread->getWindDebugTrail();
    }
    if(parameter(node,"game_overlay_draw_boxes")!=0)
    {
        std::lock_guard<std::mutex> lock(g_trackerDebugMutex);packet->tracks=g_trackerDebugTracks;packet->lockedTrackId=g_trackerLockedId;
    }
    std::lock_guard<std::mutex> lock(statesMutex);renderPackets[nodeKey]=std::move(packet);return true;
}

std::vector<std::shared_ptr<const OverlayPacket>> overlayPackets()
{
    std::vector<std::shared_ptr<const OverlayPacket>> packets;const auto now=Clock::now();
    const auto revision=NodeGraph::service().revision();
    const auto generation=detectionBuffer.currentGeneration();
    // Keep the generation gate outside statesMutex, matching publication's
    // lock order. Pause/reconfiguration cannot expose a previous capture here.
    detectionBuffer.withCurrentGeneration(generation,[&]{
        std::lock_guard<std::mutex> lock(statesMutex);
        for(const auto& entry:renderPackets)
        {
            const auto& packet=entry.second;
            if(packet->graphRevision==revision&&packet->frame&&packet->frame->generation==generation&&
                now-packet->publishedAt<std::chrono::seconds(2))packets.push_back(packet);
        }
    });
    if(revision!=NodeGraph::service().revision())packets.clear();
    return packets;
}

void retainVisionOutputs(const std::vector<int>& depthKeys,const std::vector<int>& collectorKeys,const std::vector<int>& renderKeys)
{
    cvm::RetainDataCollectionNodes(collectorKeys);
    std::lock_guard<std::mutex> lock(statesMutex);
    for(auto it=depthStates.begin();it!=depthStates.end();)if(std::find(depthKeys.begin(),depthKeys.end(),it->first)==depthKeys.end())it=depthStates.erase(it);else++it;
    for(auto it=renderPackets.begin();it!=renderPackets.end();)if(std::find(renderKeys.begin(),renderKeys.end(),it->first)==renderKeys.end())it=renderPackets.erase(it);else++it;
}
void retainVisionOutputs(const Actions& actions)
{
    auto collectorKeys=actions.activeCollectorKeys,screenshotKeys=actions.activeScreenshotKeys;
    // A false trigger/enable wire does not delete a node's cadence. Keep that
    // state while the output remains registered; remove it on actual deletion,
    // bypass, parameter disable, or an unreachable output branch instead.
    for(const auto& node:NodeGraph::service().providerNodes(NodeKind::CollectDataset))collectorKeys.push_back(visionKey(node));
    for(const auto& node:NodeGraph::service().providerNodes(NodeKind::DatasetOutput))collectorKeys.push_back(visionKey(node));
    for(const auto& node:NodeGraph::service().providerNodes(NodeKind::ScreenshotOutput))screenshotKeys.push_back(visionKey(node));
    retainVisionOutputs(actions.activeDepthKeys,collectorKeys,actions.activeRenderKeys);
    std::lock_guard<std::mutex> lock(statesMutex);
    for(auto it=maskHoldStates.begin();it!=maskHoldStates.end();)
        if(std::find(actions.activeMaskKeys.begin(),actions.activeMaskKeys.end(),it->first)==actions.activeMaskKeys.end())it=maskHoldStates.erase(it);else++it;
    for(auto it=maskBuildStates.begin();it!=maskBuildStates.end();)
        if(std::find(actions.activeMaskKeys.begin(),actions.activeMaskKeys.end(),it->first)==actions.activeMaskKeys.end())it=maskBuildStates.erase(it);else++it;
    for(auto it=screenshotStates.begin();it!=screenshotStates.end();)
        if(std::find(screenshotKeys.begin(),screenshotKeys.end(),it->first)==screenshotKeys.end())it=screenshotStates.erase(it);else++it;
}
}
