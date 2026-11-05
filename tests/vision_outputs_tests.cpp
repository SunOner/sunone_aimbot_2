#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "nodes/vision_outputs.h"
#include "nodes/vision_runtime.h"
#include "nodes/preview_image.h"
#include "scr/data_collector.h"
#include "runtime/input_state.h"
#include "detection_buffer.h"
#include "mouse.h"
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <limits>

// The real output helpers operate on CPU frames here without starting capture,
// devices, an overlay window, CUDA, or the inference provider.
MouseThread* globalMouseThread = nullptr;
DetectionBuffer detectionBuffer;
std::atomic<bool> graphAimActive{true};
std::mutex g_trackerDebugMutex;
std::vector<TrackDebugInfo> g_trackerDebugTracks;
int g_trackerLockedId = -1;
std::vector<std::pair<double,double>> MouseThread::getFuturePositions(){return {};}
std::vector<std::pair<double,double>> MouseThread::getWindDebugTrail(){return {};}
namespace OtherTools
{
std::string TrimAscii(std::string value)
{
    const auto first=value.find_first_not_of(" \t\n\r\f\v");
    if(first==std::string::npos)return {};
    return value.substr(first,value.find_last_not_of(" \t\n\r\f\v")-first+1);
}
}
namespace NodeVision
{
std::map<int,std::string> testErrors;
std::shared_ptr<const FrameStorage> storage(const NodeGraph::FrameStream& frame)
{return frame&&frame->storage?std::static_pointer_cast<const FrameStorage>(frame->storage):nullptr;}
cv::Mat cpuImage(const NodeGraph::FrameStream& frame)
{const auto native=storage(frame);return native?native->image:cv::Mat();}
cv::Mat sourceImage(const NodeGraph::FrameStream& frame)
{const auto native=storage(frame);return native?(native->source.empty()?native->image:native->source):cv::Mat();}
void reportError(int key,const std::string& error){testErrors[key]=error;}
}

namespace
{
using namespace NodeGraph;
namespace fs=std::filesystem;
constexpr int VisionBase=static_cast<int>(GraphKind::Vision)*1000001;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
FrameStream frame(const cv::Mat& pixels,std::int64_t version=1,std::uint64_t generation=1)
{
    auto native=std::make_shared<NodeVision::FrameStorage>();native->image=pixels.clone();native->source=native->image;
    auto result=std::make_shared<FrameData>();result->storage=native;result->width=pixels.cols;result->height=pixels.rows;
    result->version=version;result->generation=generation;return result;
}
MaskStream mask(const cv::Mat& pixels,std::int64_t version=1,std::uint64_t generation=1)
{
    auto native=std::make_shared<NodeVision::MaskStorage>();native->mask=pixels.clone();
    auto result=std::make_shared<MaskData>();result->storage=native;result->width=pixels.cols;result->height=pixels.rows;
    result->version=version;result->generation=generation;return result;
}
DetectionStream detections(std::int64_t version=1,std::uint64_t generation=1)
{
    auto result=std::make_shared<DetectionFrame>();result->version=version;result->generation=generation;
    result->fresh=true;result->width=2;result->height=2;result->modelName="connected_detector.onnx";
    result->objects={{0,0,1,1,4,0.95},{1,1,1,1,7,0.8}};return result;
}
const cv::Mat& pixels(const MaskStream& value)
{return std::static_pointer_cast<const NodeVision::MaskStorage>(value->storage)->mask;}

void previewsConvertPixelsWithoutChangingSources()
{
    auto settings=defaultNode(NodeKind::ImagePreview,1);Value value;value.type=ValueType::Frame;
    value.frame=frame(cv::Mat(900,1600,CV_8UC3,cv::Scalar(10,20,230)));
    const auto image=NodeVision::makePreview(value,{},settings);
    require(image&&image->width==320&&image->height==180&&image->sourceWidth==1600&&image->sourceHeight==900,"Preview did not preserve aspect ratio or bound its pixel count");
    require(image->rgba[0]==230&&image->rgba[1]==20&&image->rgba[2]==10&&image->rgba[3]==255,"Frame preview uses the wrong channel order");
    require(NodeVision::cpuImage(value.frame).at<cv::Vec3b>(0,0)==cv::Vec3b(10,20,230),"Preview mutated capture pixels");
    value.frame=frame(cv::Mat(2,2,CV_8UC4,cv::Scalar(10,20,230,0)));
    require(NodeVision::makePreview(value,{},settings)->rgba[3]==255,"A capture frame with zero BGRA alpha became invisible");
    value.type=ValueType::Depth;value.frame=frame((cv::Mat_<float>(2,2)<<1,2,3,std::numeric_limits<float>::quiet_NaN()));settings.parameters["colormap"]=1;
    const auto depth=NodeVision::makePreview(value,{},settings);
    require(depth&&depth->rgba[0]==0&&depth->rgba[8]==255&&depth->rgba[12]==0,"Depth normalization mishandled its finite range or NaN pixels");
    settings.parameters["colormap"]=0;const auto color=NodeVision::makePreview(value,{},settings);
    require(color&&color->rgba!=depth->rgba,"Depth color choices do not change the preview");
    value.type=ValueType::Mask;value.frame.reset();value.mask=mask((cv::Mat_<std::uint8_t>(2,2)<<0,255,255,0));
    const auto selected=NodeVision::makePreview(value,{},settings);require(selected&&selected->rgba[0]==0&&selected->rgba[4]==255,"Mask preview is not rendered as grayscale");
    value.type=ValueType::Frame;value.mask.reset();value.frame=frame(cv::Mat(32,32,CV_8UC3,cv::Scalar(10,20,30)));
    const auto plain=NodeVision::makePreview(value,{},settings),boxes=NodeVision::makePreview(value,detections(),settings),stale=NodeVision::makePreview(value,detections(9),settings);
    require(boxes->rgba!=plain->rgba&&stale->rgba==plain->rgba,"Preview boxes were omitted or mixed detections from a different frame");
}
void masksOperateOnRealPixels()
{
    auto depth=frame((cv::Mat_<std::uint8_t>(2,2)<<0,10,200,255));
    Node node=defaultNode(NodeKind::DepthToMask,11);
    node.parameters["depth_mask_near_percent"]=25;node.parameters["depth_mask_expand"]=0;node.parameters["depth_mask_invert"]=0;
    auto selected=NodeVision::depthToMask(depth,node);
    require(selected&&selected->storage&&cv::countNonZero(pixels(selected))==1&&pixels(selected).at<std::uint8_t>(0,0)==255,
        "Depth percentile did not select the expected near pixel");
    node.parameters["depth_mask_invert"]=1;
    auto farMask=NodeVision::depthToMask(depth,node);
    require(cv::countNonZero(pixels(farMask))==1&&pixels(farMask).at<std::uint8_t>(1,1)==255,"Inverted percentile did not select the far pixel");
    node.parameters["depth_mask_near_percent"]=0;
    require(cv::countNonZero(pixels(NodeVision::depthToMask(depth,node)))==0,"Zero percentile must produce an empty mask");
    node.parameters["depth_mask_near_percent"]=25;node.parameters["depth_mask_invert"]=0;node.parameters["depth_mask_expand"]=1;
    require(cv::countNonZero(pixels(NodeVision::depthToMask(depth,node)))>1,"Mask expansion did not change real pixels");
    const auto original=frame(cv::Mat(2,2,CV_8UC3,cv::Scalar::all(255)));
    Node apply=defaultNode(NodeKind::ApplyMask,12);apply.parameters["enabled"]=1;
    const auto masked=NodeVision::applyMask(original,selected,apply);
    require(masked&&masked->storage&&NodeVision::cpuImage(masked).at<cv::Vec3b>(0,0)==cv::Vec3b(0,0,0),"Apply Mask did not erase masked image pixels");
    require(NodeVision::cpuImage(masked).at<cv::Vec3b>(1,1)==cv::Vec3b(255,255,255),"Apply Mask erased an unmasked image pixel");
    require(NodeVision::cpuImage(original).at<cv::Vec3b>(0,0)==cv::Vec3b(255,255,255),"Apply Mask mutated its input frame");
    require(NodeVision::sourceImage(masked).at<cv::Vec3b>(0,0)==cv::Vec3b(255,255,255),"Apply Mask lost the original source frame");
    apply.parameters["enabled"]=0;
    require(NodeVision::applyMask(original,{},apply)==original,"Disabled Apply Mask did not bypass a missing mask");
    apply.parameters["enabled"]=1;apply.parameters["depth_mask_hold_frames"]=2;
    NodeVision::applyMask(original,selected,apply);
    const auto next=frame(NodeVision::cpuImage(original),2);
    const auto clear=mask(cv::Mat::zeros(2,2,CV_8UC1));
    require(NodeVision::cpuImage(NodeVision::applyMask(next,clear,apply)).at<cv::Vec3b>(0,0)==cv::Vec3b(0,0,0),"Apply Mask ignored its hold-frame setting");
    require(NodeVision::cpuImage(NodeVision::applyMask(frame(NodeVision::cpuImage(original),3),clear,apply)).at<cv::Vec3b>(0,0)==cv::Vec3b(255,255,255),"Apply Mask hold did not expire");
}

void depthMaskCadenceIsPerNodeAndEditsAreImmediate()
{
    Node first=defaultNode(NodeKind::DepthToMask,15);first.parameters["depth_mask_near_percent"]=25;
    first.parameters["depth_mask_fps"]=1;first.parameters["depth_mask_expand"]=0;first.parameters["depth_mask_invert"]=0;
    Node second=first;second.id=16;
    const auto before=frame((cv::Mat_<std::uint8_t>(2,2)<<0,10,200,255));
    const auto after=frame((cv::Mat_<std::uint8_t>(2,2)<<255,10,200,0),2);
    const auto cached=NodeVision::depthToMask(before,first);
    require(NodeVision::depthToMask(after,first)==cached,"Depth mask ignored its FPS cadence");
    require(pixels(NodeVision::depthToMask(after,second)).at<std::uint8_t>(1,1)==255,"Depth mask nodes shared a cached output");
    first.parameters["depth_mask_invert"]=1;
    require(pixels(NodeVision::depthToMask(after,first)).at<std::uint8_t>(0,0)==255,"Editing mask settings did not invalidate its cached output immediately");
}

void maskStateIsIndependentAndPruned()
{
    const auto occupied=mask((cv::Mat_<std::uint8_t>(2,2)<<255,0,0,0));
    const auto clear=mask(cv::Mat::zeros(2,2,CV_8UC1));
    Node first=defaultNode(NodeKind::FilterMask,20);first.parameters["enabled"]=1;first.parameters["depth_mask_hold_frames"]=2;
    Node second=first;second.id=21;
    require(NodeVision::filterMask(detections(),occupied,first)->objects.size()==1,"Mask filtering ignored the connected pixels");
    Node fractionalFilter=first;fractionalFilter.id=22;fractionalFilter.parameters["depth_mask_hold_frames"]=0;
    auto fractional=std::make_shared<DetectionFrame>(*detections());fractional->objects.front().x=-0.5;
    require(NodeVision::filterMask(fractional,occupied,fractionalFilter)->objects.size()==1,"Fractional detection bounds skipped an intersecting mask pixel");
    require(NodeVision::filterMask(detections(2),clear,first)->objects.size()==1,"Mask hold did not survive the next detection frame");
    require(NodeVision::filterMask(detections(2),clear,second)->objects.size()==2,"Separate Filter Mask nodes shared a hold state");
    require(NodeVision::filterMask(detections(3),clear,first)->objects.size()==2,"Mask hold did not expire at its own frame limit");
    NodeVision::filterMask(detections(4),occupied,first);
    Actions active;active.activeMaskKeys={VisionBase+first.id};NodeVision::retainVisionOutputs(active);
    require(NodeVision::filterMask(detections(5),clear,first)->objects.size()==1,"Active Filter Mask state was discarded during retention");
    NodeVision::retainVisionOutputs(Actions{});
    require(NodeVision::filterMask(detections(6),clear,first)->objects.size()==2,"Removed Filter Mask state survived retention");
    require(!NodeVision::filterMask(detections(7,2),clear,first),"Filter Mask accepted a different capture generation");
}

struct TemporaryDataset
{
    fs::path path=fs::temp_directory_path()/("sunone_vision_outputs_test_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDataset(){fs::create_directory(path);}
    ~TemporaryDataset()
    {
        std::error_code error;const auto absolute=fs::weakly_canonical(path,error);
        const auto temp=fs::weakly_canonical(fs::temp_directory_path(),error);
        if(!error&&absolute.parent_path()==temp&&absolute.filename().string().rfind("sunone_vision_outputs_test_",0)==0)fs::remove_all(absolute,error);
    }
};
void collectorUsesIndependentCadenceAndConnectedModel()
{
    TemporaryDataset temporary;cvm::ResetDataCollectionRuntime();
    Node first=defaultNode(NodeKind::CollectDataset,30);first.parameters["enabled"]=0;
    first.parameters["collect_only_when_aimbot_running"]=0;first.parameters["collect_only_when_targets_present"]=0;
    first.parameters["collect_save_every_n_frames"]=2;first.parameters["collect_cooldown_ms"]=0;
    first.parameters["auto_label_data"]=1;first.parameters["auto_label_min_conf"]=0.5;
    first.textParameters["collect_output_dir"]=temporary.path.string();first.textParameters["model_name"]="unconnected_model.onnx";
    Node second=first;second.id=31;const auto image=frame(cv::Mat(2,2,CV_8UC3,cv::Scalar(20,40,60)));
    require(!NodeVision::collectDataset(VisionBase+first.id,image,detections(),first),"Collector ignored its frame cadence");
    require(!NodeVision::collectDataset(VisionBase+second.id,image,detections(),second),"Collectors shared a cadence counter");
    const auto image2=frame(NodeVision::cpuImage(image),2);
    require(NodeVision::collectDataset(VisionBase+first.id,image2,detections(2),first),"First graph collector did not save its second frame");
    require(NodeVision::collectDataset(VisionBase+second.id,image2,detections(2),second),"Second graph collector did not save independently");
    const auto images=temporary.path/"connected_detector"/"images",labels=temporary.path/"connected_detector"/"labels";
    require(fs::is_directory(images)&&fs::is_directory(labels),"Collector ignored the connected detector's model identity");
    require(std::distance(fs::directory_iterator(images),fs::directory_iterator())==2,"Two collectors overwrote each other's image filenames");
    require(std::distance(fs::directory_iterator(labels),fs::directory_iterator())==2,"Two collectors overwrote each other's label filenames");
    for(const auto& entry:fs::directory_iterator(labels))
    {std::ifstream input(entry.path());std::string contents((std::istreambuf_iterator<char>(input)),{});require(contents.find("4 ")!=std::string::npos&&contents.find("7 ")!=std::string::npos,"Collector labels lost connected detections");}
    for(int id:{first.id,second.id})
    {const auto status=cvm::GetDataCollectionNodeUiState(VisionBase+id);require(status.observed_frame_count==2&&status.saved_image_count==1&&status.saved_label_count==1,"Collector statistics leaked across nodes");}
    cvm::ResetDataCollectionNodeCounters(VisionBase+second.id);
    const auto reset=cvm::GetDataCollectionNodeUiState(VisionBase+second.id);
    require(reset.observed_frame_count==0&&reset.attempted_sample_count==0&&reset.saved_image_count==0&&reset.saved_label_count==0,"Node counter reset left stale values");
    require(cvm::GetDataCollectionNodeUiState(VisionBase+first.id).saved_image_count==1,"Counter reset affected another collector");
    require(!NodeVision::collectDataset(VisionBase+second.id,frame(NodeVision::cpuImage(image),3),detections(3),second),"Reset changed the recording cadence");
    require(NodeVision::collectDataset(VisionBase+second.id,frame(NodeVision::cpuImage(image),4),detections(4),second),"Recording did not resume after a counter reset");
    require(cvm::GetDataCollectionNodeUiState(VisionBase+second.id).saved_image_count==1&&std::distance(fs::directory_iterator(images),fs::directory_iterator())==3,"Reset reused a sample filename or lost new counters");
    require(!NodeVision::collectDataset(VisionBase+first.id,frame(NodeVision::cpuImage(image),3),detections(2),first),"Collector accepted labels from a different frame");
    Actions active;active.activeCollectorKeys={VisionBase+second.id};NodeVision::retainVisionOutputs(active);
    require(cvm::GetDataCollectionNodeUiState(VisionBase+first.id).observed_frame_count==0,"Deleted collector retained its cadence state");
}

void packetsKeepIndependentImmutableStyles()
{
    const auto image=frame(cv::Mat(2,2,CV_8UC3,cv::Scalar::all(255)));
    Node first=defaultNode(NodeKind::RenderOverlay,40),second=defaultNode(NodeKind::RenderOverlay,41);
    first.parameters["game_overlay_box_r"]=12;second.parameters["game_overlay_box_r"]=240;
    require(NodeVision::publishOverlay(VisionBase+first.id,image,detections(),{},{},first),"First overlay did not publish");
    require(NodeVision::publishOverlay(VisionBase+second.id,image,detections(),{},{},second),"Second overlay did not publish");
    first.parameters["game_overlay_box_r"]=99;
    const auto packets=NodeVision::overlayPackets();require(packets.size()==2,"Overlay nodes overwrote one another");
    for(const auto& packet:packets)require(parameter(packet->style,"game_overlay_box_r")==
        (packet->nodeKey==VisionBase+first.id?12:240),"Overlay packet did not snapshot its own immutable style");
    Actions active;active.activeRenderKeys={VisionBase+second.id};NodeVision::retainVisionOutputs(active);
    const auto remaining=NodeVision::overlayPackets();require(remaining.size()==1&&remaining.front()->nodeKey==VisionBase+second.id,"Deleted overlay kept rendering a stale packet");
}

void screenshotsUseConnectedPixelsAndIndependentCadence()
{
    TemporaryDataset temporary;const auto directory=temporary.path/"screenshots";
    Node first=defaultNode(NodeKind::ScreenshotOutput,61);first.parameters["cooldown_ms"]=60000;first.parameters["quality"]=100;
    first.textParameters["output_dir"]=directory.string();first.textParameters["format"]="png";
    Node second=first;second.id=62;
    const auto image=frame(cv::Mat(2,2,CV_8UC3,cv::Scalar(20,40,60)));
    require(NodeVision::screenshot(VisionBase+first.id,image,first),"First screenshot node did not save");
    require(!NodeVision::screenshot(VisionBase+first.id,image,first),"Screenshot node ignored its cooldown");
    require(NodeVision::screenshot(VisionBase+second.id,image,second),"Screenshot nodes shared a cooldown");
    require(std::distance(fs::directory_iterator(directory),fs::directory_iterator())==2,"Screenshot nodes overwrote one another's image files");
    for(const auto& entry:fs::directory_iterator(directory))
        require(cv::imread(entry.path().string()).at<cv::Vec3b>(0,0)==cv::Vec3b(20,40,60),"Screenshot ignored the connected frame pixels");
    first.textParameters["format"]="jpg";
    require(NodeVision::screenshot(VisionBase+first.id,image,first),"Editing screenshot settings did not apply immediately");
    require(std::any_of(fs::directory_iterator(directory),fs::directory_iterator(),[](const fs::directory_entry& entry){return entry.path().extension()==".jpg";}),"Screenshot ignored its selected image format");
    Actions active;active.activeScreenshotKeys={VisionBase+second.id};NodeVision::retainVisionOutputs(active);
    require(NodeVision::screenshot(VisionBase+first.id,image,first),"Deleted screenshot node retained its cooldown state");
    Node invalid=first;invalid.id=63;invalid.textParameters["format"]="invalid";
    require(!NodeVision::screenshot(VisionBase+invalid.id,image,invalid)&&!NodeVision::testErrors[VisionBase+invalid.id].empty(),"Screenshot save failure did not publish a node error");
    Node registered=first;registered.id=64;
    auto workspace=NodeGraph::service().workspace();auto& graph=workspace.graphs[3];
    graph.nodes.push_back(registered);auto gate=defaultNode(NodeKind::Boolean,65);gate.value=0;graph.nodes.push_back(gate);
    graph.links.push_back({65,0,64,0});graph.links.push_back({1,0,64,1});std::string error;
    require(NodeGraph::service().publish(workspace,error),error.c_str());
    require(NodeVision::screenshot(VisionBase+registered.id,image,registered),"Registered screenshot output did not save");
    NodeVision::retainVisionOutputs(Actions{});
    require(!NodeVision::screenshot(VisionBase+registered.id,image,registered),"Releasing the trigger erased a registered screenshot node's cooldown");
}

void unsupportedDepthHasVisibleError()
{
    const auto result=NodeVision::inferDepth(VisionBase+50,frame(cv::Mat(2,2,CV_8UC3)),defaultNode(NodeKind::DepthInference,50));
    require(result&&!result->storage&&result->error.find("CUDA")!=std::string::npos,"Unsupported depth silently passed through the source image");
    require(!NodeVision::testErrors[VisionBase+50].empty(),"Unsupported depth did not publish its node diagnostic");
}

void overlaySafetyRejectsOldFramesAndGraphs()
{
    NodeVision::retainVisionOutputs(Actions{});
    const auto node=defaultNode(NodeKind::RenderOverlay,13);
    const auto image=frame(cv::Mat(2,2,CV_8UC3),1,detectionBuffer.currentGeneration());
    require(NodeVision::publishOverlay(VisionBase+node.id,image,{},{},{},node),"Current overlay packet did not publish");
    require(NodeVision::overlayPackets().size()==1,"Current overlay packet was unexpectedly hidden");
    detectionBuffer.togglePaused();require(NodeVision::overlayPackets().empty(),"Paused capture retained visible overlay packets");
    detectionBuffer.togglePaused();require(NodeVision::overlayPackets().empty(),"Previous capture generation remained visible after resume");
    const auto resumed=frame(cv::Mat(2,2,CV_8UC3),2,detectionBuffer.currentGeneration());
    require(NodeVision::publishOverlay(VisionBase+node.id,resumed,{},{},{},node),"Resumed overlay packet did not publish");
    const auto oldRevision=service().revision();const auto saved=service().workspace();auto updated=saved;
    auto& graph=updated.graphs[static_cast<std::size_t>(GraphKind::Vision)];
    std::vector<int> removed;
    for(const auto& item:graph.nodes)if(item.kind==NodeKind::RenderOverlay||item.kind==NodeKind::OverlayOutput)removed.push_back(item.id);
    graph.nodes.erase(std::remove_if(graph.nodes.begin(),graph.nodes.end(),[](const Node& item){return item.kind==NodeKind::RenderOverlay||item.kind==NodeKind::OverlayOutput;}),graph.nodes.end());
    graph.links.erase(std::remove_if(graph.links.begin(),graph.links.end(),[&](const Link& link){return std::find(removed.begin(),removed.end(),link.fromNode)!=removed.end()||std::find(removed.begin(),removed.end(),link.toNode)!=removed.end();}),graph.links.end());
    for(auto& group:graph.groups)group.nodes.erase(std::remove_if(group.nodes.begin(),group.nodes.end(),[&](int id){return std::find(removed.begin(),removed.end(),id)!=removed.end();}),group.nodes.end());
    std::string error;require(service().publish(updated,error),"Could not publish a graph after removing Render Overlay");
    require(NodeVision::overlayPackets().empty(),"A packet from the old graph remained visible after deleting Render Overlay");
    require(!NodeVision::publishOverlayAtRevision(VisionBase+node.id,resumed,{},{},{},node,oldRevision),"An old graph evaluation published after its output was removed");
    require(service().publish(saved,error),"Could not restore the isolated graph fixture");
}
}

int main()
{
    try
    {
        detectionBuffer.generation=1;
        previewsConvertPixelsWithoutChangingSources();
        masksOperateOnRealPixels();depthMaskCadenceIsPerNodeAndEditsAreImmediate();maskStateIsIndependentAndPruned();collectorUsesIndependentCadenceAndConnectedModel();
        packetsKeepIndependentImmutableStyles();screenshotsUseConnectedPixelsAndIndependentCadence();unsupportedDepthHasVisibleError();overlaySafetyRejectsOldFramesAndGraphs();NodeVision::retainVisionOutputs(NodeGraph::Actions{});
        std::cout<<"Vision output regressions passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"Vision output regression failed: "<<error.what()<<'\n';return 1;}
}
