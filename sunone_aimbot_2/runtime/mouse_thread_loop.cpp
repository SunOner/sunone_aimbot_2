#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

#include "capture.h"
#include "mouse.h"
#include "AimbotTarget.h"
#include "config.h"
#include "detection_buffer.h"
#include "runtime/input_state.h"
#include "keyboard/graph_key_state.h"
#include "runtime/thread_loops.h"
#include "nodes/node_graph.h"
#include "nodes/vision_outputs.h"
#include "nodes/vision_runtime.h"

namespace
{
constexpr int kPredictedOnlyMoveGraceFrames = 3;
constexpr double kPredictedOnlyMoveGraceSec =
    static_cast<double>(kPredictedOnlyMoveGraceFrames) / 60.0;
constexpr int kPredictedOnlyMoveStalePadMs = 16;

double trackerFrameIntervalSec(int captureFpsValue)
{
    return 1.0 / std::clamp(
        static_cast<double>((captureFpsValue > 0) ? captureFpsValue : 60), 15.0, 500.0);
}

bool allowPredictedOnlyMove(
    int activeTrackId, bool hasActiveTarget,
    const LockedTargetInfo& lockInfo, int captureFpsValue)
{
    if (activeTrackId != lockInfo.trackId || !hasActiveTarget || lockInfo.missedFrames <= 0)
        return false;
    const double frameDtSec = trackerFrameIntervalSec(captureFpsValue);
    const double missedSec = static_cast<double>(lockInfo.missedFrames) * frameDtSec;
    return missedSec <= kPredictedOnlyMoveGraceSec + frameDtSec * 0.51;
}

int trackerStaleTimeoutMs(int captureFpsValue)
{
    const int frameBasedMs = 2000 / std::max(1, captureFpsValue);
    const int graceBasedMs = static_cast<int>(kPredictedOnlyMoveGraceSec * 1000.0 + 0.5) +
        kPredictedOnlyMoveStalePadMs;
    return std::clamp(std::max(frameBasedMs, graceBasedMs), 25, 180);
}

struct SelectionState
{
    MultiTargetTracker tracker;
    std::optional<AimbotTarget> target;
    int trackId = -1;
    std::int64_t version = -1;
    bool observed = false;
    bool moveEligible = false;
    double confidence = 0.0;
    double velocityX = 0.0, velocityY = 0.0;
    int missedFrames = 0;
    std::chrono::steady_clock::time_point updatedAt{};
};
}

void createInputDevices();
void assignInputDevices();

void mouseThreadFunction(MouseThread& mouseThread)
{
    struct StopOnExit
    {
        MouseThread& mouse;
        ~StopOnExit() { graphAimActive = false; mouse.stop(); }
    } stopOnExit{ mouseThread };

    int lastVersion = -1;
    const auto runtimeStartedAt = std::chrono::steady_clock::now();
    NodeGraph::DetectionStream detections;
    std::chrono::steady_clock::time_point detectionTimestamp{};
    auto lastDetectionArrival = std::chrono::steady_clock::time_point::min();
    // Selection is state owned by Target Selection nodes. No tracker or target
    // algorithm executes unless an evaluated graph requests its selection mode.
    std::unordered_map<int, SelectionState> selectionStates;
    int appliedDetectionResolution = -1;
    std::uint64_t controlGeneration = detectionBuffer.currentGeneration();
    std::uint64_t graphRevision = 0;
    struct PredictionCache { NodeGraph::Point input; NodeGraph::Point output; std::int64_t version=-1; };
    std::unordered_map<int, PredictionCache> predictionCache;
    struct MotionCache {NodeGraph::Point input;NodeGraph::MotionData output;std::int64_t version=-1;};
    std::unordered_map<int,MotionCache> motionCache;
    double observationDelaySeconds=0;
    std::unordered_map<int, NodeGraph::Movement> windCache;
    bool inputWasCaptured = false;

    auto resetSelections = [&]() {
        selectionStates.clear();
        predictionCache.clear();
        motionCache.clear();
        windCache.clear();
        mouseThread.clearFuturePositions();
        mouseThread.resetPrediction();
        std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
        g_trackerDebugTracks.clear();
        g_trackerLockedId = -1;
    };

    while (!shouldExit)
    {
        bool hasNewDetection = false;
        int detectionResolution = 0;
        bool pausedNow = false;
        std::uint64_t observationGeneration = controlGeneration;

        {
            std::lock_guard<std::mutex> cfgLock(configMutex);
            detectionResolution = config.detection_resolution;
        }

        const bool aimingNow = aiming.load();

        {
            std::unique_lock<std::mutex> lock(detectionBuffer.mutex);
            detectionBuffer.cv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return detectionBuffer.version > lastVersion || shouldExit;
            });
            if (shouldExit)
                break;
            pausedNow = detectionBuffer.paused;
            observationGeneration = detectionBuffer.generation;
            if (detectionBuffer.version > lastVersion)
            {
                detectionTimestamp = detectionBuffer.frameTimestamp;
                lastVersion = detectionBuffer.version;
                hasNewDetection = true;
                lastDetectionArrival = std::chrono::steady_clock::now();
                auto frame=std::make_shared<NodeGraph::DetectionFrame>();
                frame->version=lastVersion;frame->generation=observationGeneration;frame->fresh=true;
                frame->timestampSeconds=std::chrono::duration<double>(detectionTimestamp.time_since_epoch()).count();
                frame->width=frame->height=detectionResolution;
                const auto count=std::min(detectionBuffer.boxes.size(),detectionBuffer.classes.size());
                frame->objects.reserve(count);
                for(std::size_t i=0;i<count;++i)
                {
                    const auto& box=detectionBuffer.boxes[i];
                    frame->objects.push_back({static_cast<double>(box.x),static_cast<double>(box.y),
                        static_cast<double>(box.width),static_cast<double>(box.height),detectionBuffer.classes[i],
                        i<detectionBuffer.confidences.size()?detectionBuffer.confidences[i]:0.0});
                }
                detections=std::move(frame);
            }
        }

        mouseThread.setDetectionGeneration(observationGeneration);
        if (observationGeneration != controlGeneration)
        {
            controlGeneration = observationGeneration;
            resetSelections();
            mouseThread.releaseMouse();
        }

        if (input_method_changed.exchange(false))
        {
            createInputDevices();
            assignInputDevices();
        }

        if(detectionResolution!=appliedDetectionResolution)
        {
            appliedDetectionResolution=detectionResolution;
            mouseThread.setCaptureResolution(detectionResolution);resetSelections();
        }

        const auto requestedRevision = NodeGraph::service().revision();
        if (requestedRevision != graphRevision)
        {
            resetSelections();
            mouseThread.releaseMouse();
            graphRevision = requestedRevision;
        }

        const bool inputCaptured = editorInputCaptured.load();
        if(inputCaptured!=inputWasCaptured)
        {
            resetSelections();
            inputWasCaptured=inputCaptured;
        }
        if (pausedNow)
        {
            graphAimActive = false;
            NodeGraph::Context pausedContext;
            pausedContext.inputCaptured = inputCaptured;
            pausedContext.captureFps = captureFps.load();
            NodeGraph::service().evaluate(pausedContext);NodeVision::retainControlDrawings({});
            mouseThread.clearQueuedMoves();
            mouseThread.releaseMouse();
            continue;
        }

        NodeGraph::Context context;
        context.inputCaptured = editorInputCaptured.load();
        context.inputCapturedNow = []{return editorInputCaptured.load();};
        context.aimHeld = aimingNow;
        context.shootHeld = shooting.load();
        context.zoomHeld = zooming.load();
        context.controlActive = graphAimActive.load();
        context.center = { detectionResolution * 0.5, detectionResolution * 0.5 };
        context.captureFps = captureFps.load();
        context.elapsedSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - runtimeStartedAt).count();
        context.detectionsAvailable = lastVersion >= 0;
        context.detections = detections;
        if(hasNewDetection)observationDelaySeconds=std::clamp(std::chrono::duration<double>(std::chrono::steady_clock::now()-detectionTimestamp).count(),0.0,0.35);
        context.observationDelaySeconds=observationDelaySeconds;

        context.selectTarget = [&](int nodeKey,const NodeGraph::DetectionStream& stream,bool lockTarget,const NodeGraph::Node& node) {
            NodeGraph::TargetData selected;
            if(!stream || stream->generation!=controlGeneration) return selected;
            auto& state=selectionStates[nodeKey];
            const int mode=node.kind==NodeGraph::NodeKind::TrackDetections?2:node.kind==NodeGraph::NodeKind::ChooseTarget?1:static_cast<int>(NodeGraph::parameter(node,"mode"));
            TargetSelectionSettings settings;
            settings.classPlayer=static_cast<int>(NodeGraph::parameter(node,"class_player"));
            settings.classHead=static_cast<int>(NodeGraph::parameter(node,"class_head"));
            settings.applyOffsets=NodeGraph::parameter(node,"legacy_offsets")!=0;
            settings.bodyOffset=NodeGraph::parameter(node,"body_y_offset");
            settings.headOffset=NodeGraph::parameter(node,"head_y_offset");
            settings.filterClasses=NodeGraph::parameter(node,"filter_classes")!=0;
            settings.preferHead=NodeGraph::parameter(node,"prefer_head")!=0;
            settings.mergeHeadBody=NodeGraph::parameter(node,"merge_head_body")!=0;
            settings.maxMissedFrames=static_cast<int>(NodeGraph::parameter(node,"max_missed_frames"));
            const bool disableHeadshot=NodeGraph::parameter(node,"disable_headshot")!=0;
            const auto now=std::chrono::steady_clock::now();
            if(state.version!=stream->version)
            {
                std::vector<cv::Rect> boxes;
                std::vector<int> classes;
                std::vector<float> confidences;
                boxes.reserve(stream->objects.size());classes.reserve(stream->objects.size());confidences.reserve(stream->objects.size());
                for(const auto& detection:stream->objects)
                {
                    boxes.emplace_back(static_cast<int>(std::lround(detection.x)),static_cast<int>(std::lround(detection.y)),
                        static_cast<int>(std::lround(detection.width)),static_cast<int>(std::lround(detection.height)));
                    classes.push_back(detection.classId);confidences.push_back(static_cast<float>(detection.confidence));
                }
                state.version=stream->version;state.updatedAt=lastDetectionArrival;
                const int previousTrackId=state.trackId;const bool hadTarget=state.target.has_value();
                const auto observedAt=std::chrono::steady_clock::time_point(
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(stream->timestampSeconds)));
                if(mode==2)
                {
                    state.tracker.update(boxes,classes,static_cast<int>(stream->width),static_cast<int>(stream->height),
                        disableHeadshot,lockTarget,settings,confidences,observedAt);
                    {
                        std::lock_guard<std::mutex> lock(g_trackerDebugMutex);
                        g_trackerDebugTracks=state.tracker.getDebugTracks();g_trackerLockedId=state.tracker.getLockedTrackId();
                    }
                    LockedTargetInfo info;
                    if(state.tracker.getLockedTarget(info))
                    {
                        state.target=info.target;state.trackId=info.trackId;state.observed=info.observedThisFrame;
                        state.confidence=info.confidence;state.velocityX=info.velocityX;state.velocityY=info.velocityY;state.missedFrames=info.missedFrames;
                        state.moveEligible=state.observed||allowPredictedOnlyMove(previousTrackId,hadTarget,info,captureFps.load());
                    }
                    else {state.target.reset();state.trackId=-1;state.observed=state.moveEligible=false;}
                }
                else
                {
                    std::unique_ptr<AimbotTarget> nearest(sortTargets(boxes,classes,static_cast<int>(stream->width),static_cast<int>(stream->height),disableHeadshot,settings));
                    state.target=nearest?std::optional<AimbotTarget>(*nearest):std::nullopt;
                    state.trackId=-1;state.observed=state.moveEligible=nearest!=nullptr;
                    state.velocityX=state.velocityY=0;state.missedFrames=0;state.confidence=0;
                    if(nearest)
                    {
                        double closest=std::numeric_limits<double>::max();
                        for(const auto& detection:stream->objects) if(detection.classId==nearest->classId)
                        {
                            const double offset=settings.applyOffsets?(detection.classId==settings.classHead?settings.headOffset:settings.bodyOffset):0.5;
                            const double distance=std::hypot(detection.x+detection.width*0.5-nearest->pivotX,detection.y+detection.height*offset-nearest->pivotY);
                            if(distance<closest){closest=distance;state.confidence=detection.confidence;}
                        }
                    }
                }
            }
            if(state.target&&now-state.updatedAt<=std::chrono::milliseconds(trackerStaleTimeoutMs(captureFps.load())))
            {
                const auto& target=*state.target;
                selected.point={target.pivotX,target.pivotY};
                selected.x=target.x;selected.y=target.y;selected.width=target.w;selected.height=target.h;
                selected.valid=state.observed||state.moveEligible;selected.observed=state.observed;
                selected.fresh=hasNewDetection&&state.moveEligible;selected.classId=target.classId;
                selected.confidence=state.confidence;selected.trackId=state.trackId;
                selected.velocityX=state.velocityX;selected.velocityY=state.velocityY;
                selected.missedFrames=state.missedFrames;
                selected.ageSeconds=std::max(0.0,std::chrono::duration<double>(now-state.updatedAt).count());
                selected.pointPrepared=settings.applyOffsets;
                if(node.kind==NodeGraph::NodeKind::Target&&NodeGraph::parameter(node,"legacy_camera_compensation")!=0)
                {
                    const auto delta=mouseThread.getMotionCompensationSince(detectionTimestamp);
                    selected.point.x-=delta.first;selected.point.y-=delta.second;
                }
            }
            else if(state.target){state.target.reset();state.trackId=-1;state.observed=state.moveEligible=false;}
            return selected;
        };
        context.inScope = [](const NodeGraph::TargetData& target,double multiplier,NodeGraph::Point center) {
            return target.valid&&target.observed&&std::abs(target.x+target.width*0.5-center.x)<target.width*multiplier*0.5&&
                std::abs(target.y+target.height*0.5-center.y)<target.height*multiplier*0.5;
        };
        context.predict = [&](int nodeKey,NodeGraph::Point point,const NodeGraph::Node& node,double lookahead,bool reset) {
            // A drawing branch may sample after the iteration that received
            // this observation. Consume each node's latest unseen version.
            const auto cached=predictionCache.find(nodeKey);
            if(lastVersion>=0&&(cached==predictionCache.end()||cached->second.version!=lastVersion))
            {
                auto predicted=mouseThread.graphPredict(nodeKey,point.x,point.y,detectionTimestamp,node,lookahead,reset);
                predictionCache[nodeKey]={point,{predicted.first,predicted.second},lastVersion};
                if(NodeGraph::parameter(node,"draw_futurePositions")!=0)
                    mouseThread.storeFuturePositions(mouseThread.graphFuturePositions(nodeKey,static_cast<int>(NodeGraph::parameter(node,"prediction_futurePositions"))));
            }
            else if(reset)
            {
                predictionCache.erase(nodeKey);
                mouseThread.graphPredict(nodeKey,point.x,point.y,detectionTimestamp,node,lookahead,true);
            }
            const auto found=predictionCache.find(nodeKey);
            return found==predictionCache.end()?point:NodeGraph::Point{
                point.x+found->second.output.x-found->second.input.x,
                point.y+found->second.output.y-found->second.input.y};
        };
        context.speedCurve = [&](NodeGraph::Point point,NodeGraph::Point center,double maximumDistance,const NodeGraph::Node& node) {
            return mouseThread.graphSpeedCurve(point.x,point.y,center,maximumDistance,node);
        };
        context.estimateMotion=[&](int key,NodeGraph::Point point,const NodeGraph::Node& node,bool reset){
            const auto cached=motionCache.find(key);
            if(lastVersion>=0&&(cached==motionCache.end()||cached->second.version!=lastVersion))motionCache[key]={point,mouseThread.graphEstimateMotion(key,point,detectionTimestamp,node,reset),lastVersion};
            else if(reset){motionCache.erase(key);mouseThread.graphEstimateMotion(key,point,detectionTimestamp,node,true);}
            const auto found=motionCache.find(key);
            if(found==motionCache.end())return NodeGraph::MotionData{point,{},NodeGraph::parameter(node,"kalman_velocity_damping"),false,false};
            auto motion=found->second.output;motion.position.x+=point.x-found->second.input.x;motion.position.y+=point.y-found->second.input.y;return motion;
        };
        context.futureTrail=[&](const NodeGraph::MotionData& motion,double seconds,const NodeGraph::Node& node){
            if(!hasNewDetection)return;
            const int frames=static_cast<int>(NodeGraph::parameter(node,"prediction_futurePositions"));
            const double dt=trackerFrameIntervalSec(captureFps.load());
            std::vector<std::pair<double,double>> points;points.reserve(frames);
            for(int i=1;i<=frames;++i){const auto p=NodeGraph::projectPoint(motion.position,motion.velocity,seconds+i*dt,motion.damping,motion.futureReady);points.emplace_back(p.x,p.y);}
            mouseThread.storeFuturePositions(points);
        };
        context.mouseDelta = [&](NodeGraph::Point point,double speed,NodeGraph::Point center,const NodeGraph::Node& node) {
            return mouseThread.graphMouseDelta(point.x,point.y,speed,center,node);
        };
        context.windMotion = [&](int nodeKey,const NodeGraph::Movement& movement,const NodeGraph::Node& node) {
            if(hasNewDetection)windCache[nodeKey]=mouseThread.graphWindMotion(nodeKey,movement,node);
            const auto found=windCache.find(nodeKey);
            return found==windCache.end()?NodeGraph::Movement{}:found->second;
        };
        context.motionCompensation = [&](NodeGraph::Point point) {
            const auto delta=mouseThread.getMotionCompensationSince(detectionTimestamp);
            return NodeGraph::Point{point.x-delta.first,point.y-delta.second};
        };
        context.keyDown = GraphInput::keyDown;
        context.draw=[&](int key,const NodeGraph::Drawing& drawing,NodeGraph::Point size,const NodeGraph::Node& node){
            return NodeVision::publishDrawingAtRevision(key,drawing,size,node,observationGeneration,graphRevision,true);
        };
        context.drawError=NodeVision::nodeError;

        const auto actions = NodeGraph::service().evaluate(context);
        NodeVision::retainControlDrawings(actions.activeRenderKeys);
        if(editorInputCaptured.load())
        {
            graphAimActive=false;mouseThread.clearQueuedMoves();mouseThread.releaseMouse();continue;
        }
        // If publication raced the pre-evaluation revision snapshot, discard
        // this iteration. The next iteration resets state before using it.
        if (actions.revision != graphRevision)
        {
            graphAimActive = false;
            resetSelections();
            mouseThread.releaseMouse();
            graphRevision = actions.revision;
            continue;
        }

        graphAimActive = actions.valid && actions.aim;

        if (actions.valid && actions.aim && hasNewDetection)
            for(const auto& movement:actions.movements)mouseThread.dispatchGraphMovement(movement);
        else if (!actions.valid || !actions.aim)
            mouseThread.clearQueuedMoves();

        // Stateful nodes belong to every evaluated branch, including conditions
        // in Auto Shoot/Recoil. An inactive Aim output cannot clear their history.
        mouseThread.retainGraphStates(actions.activePredictionKeys,actions.activeWindKeys);
        if(actions.activePredictionKeys.empty())mouseThread.clearFuturePositions();
        for(auto it=predictionCache.begin();it!=predictionCache.end();)
            if(std::find(actions.activePredictionKeys.begin(),actions.activePredictionKeys.end(),it->first)==actions.activePredictionKeys.end())it=predictionCache.erase(it);else++it;
        for(auto it=motionCache.begin();it!=motionCache.end();)
            if(std::find(actions.activePredictionKeys.begin(),actions.activePredictionKeys.end(),it->first)==actions.activePredictionKeys.end())it=motionCache.erase(it);else++it;
        for(auto it=windCache.begin();it!=windCache.end();)
            if(std::find(actions.activeWindKeys.begin(),actions.activeWindKeys.end(),it->first)==actions.activeWindKeys.end())it=windCache.erase(it);else++it;
        for(auto it=selectionStates.begin();it!=selectionStates.end();)
            if(std::find(actions.activeSelectionKeys.begin(),actions.activeSelectionKeys.end(),it->first)==actions.activeSelectionKeys.end())it=selectionStates.erase(it);else++it;

        // Scope and activation are explicit graph nodes. Runtime only enforces
        // current observed-target ownership and the capture generation gate.
        if (actions.valid && actions.autoShoot && actions.shootTarget.valid && actions.shootTarget.observed)
            mouseThread.pressMouse();
        else
            mouseThread.releaseMouse();

        if (actions.valid && actions.recoil && std::isfinite(actions.recoilStrength))
        {
            const int compensation = static_cast<int>(std::clamp(actions.recoilStrength, -1000.0, 1000.0));
            const double pixelsPerCount=actions.recoilStrength!=0?actions.recoilPixelDeltaY/actions.recoilStrength:0;
            mouseThread.dispatchGraphRecoil(compensation,pixelsPerCount);
        }
    }
    NodeVision::retainControlDrawings({});
}

