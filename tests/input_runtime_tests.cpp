#include "mouse.h"
#include "capture.h"
#include "config.h"
#include "detection_buffer.h"
#include "runtime/input_state.h"
#include "runtime/thread_loops.h"
#include "nodes/node_graph.h"
#include "nodes/node_config_bridge.h"
#include "nodes/vision_outputs.h"
#include "keyboard/graph_key_state.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

// Link the production mouse loop/worker without opening capture or real input.
Config config{};
std::mutex configMutex;
std::mutex inputDevicesMutex;
MouseThread* globalMouseThread = nullptr;
std::unique_ptr<IMouseInput> activeMouseInputOwner;
std::atomic<bool> shouldExit{ false };
std::atomic<bool> aiming{ false };
std::atomic<bool> graphAimActive{ false };
std::atomic<bool> editorInputCaptured{ false };
std::atomic<bool> shooting{ false };
std::atomic<bool> zooming{ false };
std::atomic<bool> input_method_changed{ false };
std::atomic<bool> detection_resolution_changed{ false };
std::atomic<int> captureFps{ 60 };
std::mutex g_trackerDebugMutex;
std::vector<TrackDebugInfo> g_trackerDebugTracks;
int g_trackerLockedId = -1;

void createInputDevices() { throw std::runtime_error("Unexpected real device creation"); }
void assignInputDevices() { throw std::runtime_error("Unexpected real device assignment"); }

// Drawing publication is covered by vision_outputs_tests; keep this worker
// fixture independent of native rendering and image/data-collection services.
namespace NodeVision
{
std::mutex drawingMutex;
std::condition_variable drawingChanged;
NodeGraph::Drawing testDrawing;
int drawingCalls=0;
bool publishDrawingAtRevision(int,const NodeGraph::Drawing& drawing,NodeGraph::Point,const NodeGraph::Node&,std::uint64_t,std::uint64_t,bool)
{std::lock_guard<std::mutex> lock(drawingMutex);testDrawing=drawing;++drawingCalls;drawingChanged.notify_all();return true;}
void retainControlDrawings(const std::vector<int>& keys)
{if(keys.empty()){std::lock_guard<std::mutex> lock(drawingMutex);testDrawing.reset();}}
std::string nodeError(int){return {};}
}

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct FakeState
{
    std::mutex mutex;
    std::condition_variable cv;
    int moves = 0;
    int lastDx = 0;
    int lastDy = 0;
    int downs = 0;
    int ups = 0;
    bool blockMoves = false;
    bool unblock = false;
};

class FakeInput final : public IMouseInput
{
public:
    explicit FakeInput(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
    const char* name() const override { return "TEST"; }
    bool isOpen() const override { return true; }
    bool move(int dx, int dy) override
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        ++state_->moves;
        state_->lastDx = dx;
        state_->lastDy = dy;
        state_->cv.notify_all();
        state_->cv.wait(lock, [&] { return !state_->blockMoves || state_->unblock; });
        return true;
    }
    bool leftDown() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->downs;
        state_->cv.notify_all();
        return true;
    }
    bool leftUp() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->ups;
        state_->cv.notify_all();
        return true;
    }

private:
    std::shared_ptr<FakeState> state_;
};

template <typename Predicate>
void waitFor(const std::shared_ptr<FakeState>& state, Predicate predicate, const char* message)
{
    std::unique_lock<std::mutex> lock(state->mutex);
    require(state->cv.wait_for(lock, 2s, predicate), message);
}

void unblock(const std::shared_ptr<FakeState>& state)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    state->unblock = true;
    state->cv.notify_all();
}

struct UnblockOnExit
{
    std::shared_ptr<FakeState> state;
    ~UnblockOnExit() { unblock(state); }
};

struct MouseLoop
{
    std::thread thread;
    explicit MouseLoop(MouseThread& mouse)
        : thread([&mouse] { mouseThreadFunction(mouse); }) {}
    ~MouseLoop()
    {
        shouldExit = true;
        detectionBuffer.cv.notify_all();
        thread.join();
    }
};

void resetFixture()
{
    {std::lock_guard<std::mutex> lock(NodeVision::drawingMutex);NodeVision::drawingCalls=0;NodeVision::testDrawing.reset();}
    shouldExit = false;
    aiming = true;
    graphAimActive = false;
    editorInputCaptured = false;
    shooting = false;
    zooming = false;
    input_method_changed = false;
    detection_resolution_changed = false;
    if (detectionBuffer.isPaused())
        detectionBuffer.togglePaused();
    detectionBuffer.clear();

    config = Config{};
    config.detection_resolution = 320;
    config.fovX = 90;
    config.fovY = 90;
    config.minSpeedMultiplier = 1.0;
    config.maxSpeedMultiplier = 1.0;
    config.auto_shoot = true;
    config.bScope_multiplier = 1.0f;
    config.wind_mouse_enabled = false;
    config.kalman_enabled = false;
    config.kalman_reset_timeout_sec = 0.5f;
    config.speedCurveExponent = 1.0f;
    config.body_y_offset = 0.5f;
    config.head_y_offset = 0.5f;
    config.class_player = 0;
    config.class_head = 1;
    config.active_game = "TEST";
    config.game_profiles.emplace("TEST", Config::GameProfile{ "TEST", 1.0, 1.0, 1.0, false, 90.0 });
    auto workspace=NodeGraph::defaults();
    NodeGraph::migrateLegacySettings(workspace,config,true);
    std::string graphError;
    require(NodeGraph::service().publish(workspace,graphError),"Could not reset graph fixture");
}

void setNodeParameter(NodeGraph::NodeKind kind,const char* key,double value)
{
    auto workspace=NodeGraph::service().workspace();
    for(auto& graph:workspace.graphs)for(auto& node:graph.nodes)if(node.kind==kind)node.parameters[key]=value;
    std::string error;
    require(NodeGraph::service().publish(workspace,error),"Could not publish node parameter");
}

std::unique_ptr<MouseThread> makeMouse(IMouseInput* input)
{
    return std::make_unique<MouseThread>(320,input);
}

void publishTarget()
{
    require(detectionBuffer.setForGeneration(
        detectionBuffer.currentGeneration(), { cv::Rect(140, 120, 40, 80) }, { 0 }, { 0.9f }, Clock::now()),
        "Fresh detection must be accepted");
}

void idleStopAndDeviceLifetime()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    auto device = std::make_unique<FakeInput>(state);
    // Repeatedly exercise the stop/wait boundary. CTest also bounds hangs.
    for (int i = 0; i < 200; ++i)
    {
        auto mouse = makeMouse(device.get());
        mouse->stop();
        mouse->stop();
    }
    auto mouse = makeMouse(device.get());
    mouse->pressMouse();
    mouse->stop();
    device.reset();
    mouse->dispatchGraphMovement(NodeGraph::Movement{{{1,1}}});
    require(state->downs == 1 && state->ups == 1 && state->moves == 0,
        "Stop must release and detach input before its owner is destroyed");
}

void stopDiscardsBacklog()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    state->blockMoves = true;
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    UnblockOnExit cleanup{ state };
    mouse->dispatchGraphMovement(NodeGraph::Movement{ { { 12, 0 } } });
    waitFor(state, [&] { return state->moves == 1; }, "Worker did not start the first move");
    for (int i = 0; i < 4; ++i)
        mouse->dispatchGraphMovement(NodeGraph::Movement{ { { 12, 0 } } });
    mouse->requestStop();
    unblock(state);
    mouse->stop();
    require(state->moves == 1, "Stop must discard the backlog, not drain it");
}

void deviceSwitchReleasesOldButton()
{
    resetFixture();
    auto oldState = std::make_shared<FakeState>();
    auto newState = std::make_shared<FakeState>();
    FakeInput oldDevice(oldState);
    FakeInput newDevice(newState);
    auto mouse = makeMouse(&oldDevice);
    mouse->pressMouse();
    mouse->setMouseInput(&newDevice);
    require(oldState->ups == 1, "Switch must release the old device");
    mouse->pressMouse();
    mouse->stop();
    require(newState->downs == 1 && newState->ups == 1, "Press ownership must reset for the new device");
}

void splitMouseConversionMatchesCombined()
{
    resetFixture();auto mouse=makeMouse(nullptr);
    for(int size:{160,320,640})for(double sensitivity:{0.0,0.12,1.75})for(double speed:{-1.0,0.0,0.05,1.7,20.0})for(bool scaled:{false,true})for(int fps:{20,60,144})
    {
        auto node=NodeGraph::defaultNode(NodeGraph::NodeKind::MouseDelta,1);
        node.parameters["fov_x"]=110;node.parameters["fov_y"]=75;node.parameters["sens"]=sensitivity;
        node.parameters["yaw"]=0.031;node.parameters["pitch"]=0.027;node.parameters["fov_scaled"]=scaled;node.parameters["base_fov"]=100;node.parameters["legacy_fps"]=1;
        mouse->setCaptureResolution(size);captureFps=fps;
        const NodeGraph::Point center{size*0.5,size*0.5},point{center.x+0.173*size,center.y-0.119*size};
        const auto original=mouse->graphMouseDelta(point.x,point.y,speed,center,node);
        auto split=NodeGraph::applyMouseSpeed(NodeGraph::pixelsToCounts({point.x-center.x,point.y-center.y},NodeGraph::mouseCalibration(node,{static_cast<double>(size),static_cast<double>(size)})),speed);
        if(fps>30)split=NodeGraph::applyMouseSpeed(split,30.0/fps);
        require(split.hasPreciseCounts==original.hasPreciseCounts&&split.steps.size()==original.steps.size(),"Split conversion changed zero sensitivity or quantization");
        require(split.preciseCounts.x==original.preciseCounts.x&&split.preciseCounts.y==original.preciseCounts.y&&split.pixelScaleX==original.pixelScaleX&&split.pixelScaleY==original.pixelScaleY,"Split conversion changed precise counts or calibration metadata");
        for(std::size_t i=0;i<split.steps.size();++i)require(split.steps[i].dx==original.steps[i].dx&&split.steps[i].dy==original.steps[i].dy,"Split conversion rounded counts before applying speed");
    }
    captureFps=60;
}
void captureGeometryUsesOnlyNodeTuning()
{
    resetFixture();auto mouse=makeMouse(nullptr);auto node=NodeGraph::defaultNode(NodeGraph::NodeKind::MouseDelta,1);
    node.parameters["sens"]=1;node.parameters["yaw"]=node.parameters["pitch"]=1;
    const auto before=mouse->graphMouseDelta(240,160,1,{160,160},node);
    config.fovX=1;config.game_profiles.at("TEST").sens=100;
    mouse->setCaptureResolution(640);
    const auto after=mouse->graphMouseDelta(240,160,1,{160,160},node);
    require(before.hasPreciseCounts&&after.hasPreciseCounts&&std::abs(after.preciseCounts.x*2-before.preciseCounts.x)<1e-9,
        "Capture geometry did not scale the explicit node conversion");
    require(after.pixelScaleX==before.pixelScaleX*2,"Movement calibration did not follow capture geometry");
}
void motionFeedbackUsesOnlyGraphCalibration()
{
    resetFixture();auto state=std::make_shared<FakeState>();FakeInput input(state);auto mouse=makeMouse(&input);
    auto profile=NodeGraph::defaultNode(NodeGraph::NodeKind::MouseCalibration,1);
    profile.parameters["fov_x"]=110;profile.parameters["fov_y"]=75;profile.parameters["sens"]=1.75;
    profile.parameters["yaw"]=0.031;profile.parameters["pitch"]=0.027;
    profile.parameters["fov_scaled"]=1;profile.parameters["base_fov"]=100;
    const auto calibration=NodeGraph::mousePixelCalibration(profile,{320,320});
    const auto since=Clock::now();mouse->dispatchGraphRecoil(5,calibration.y);
    require(std::abs(mouse->getMotionCompensationSince(since).second-5*1.75*0.027*1.1*320/75)<1e-9,
        "Recoil motion feedback no longer matches the legacy profile conversion");
    NodeGraph::Movement calibrated;calibrated.steps={{4,2}};calibrated.pixelScaleX=2;calibrated.pixelScaleY=3;
    config.game_profiles.at("TEST").sens=1000;mouse->dispatchGraphMovement(calibrated);
    waitFor(state,[&]{return state->moves==2;},"Graph movement did not dispatch");
    mouse->stop();const auto motion=mouse->getMotionCompensationSince(since);
    require(motion.first==8&&std::abs(motion.second-(6+5*calibration.y))<1e-9,"Motion feedback ignored graph calibration or read global tuning");
}

void autoShootHoldsThenReleasesWhenDisabled()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    MouseLoop loop(*mouse);
    publishTarget();
    waitFor(state, [&] { return state->downs == 1; }, "Auto-shoot did not press");
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        require(!state->cv.wait_for(lock, 10ms, [&] { return state->ups != 0; }),
            "Polling timeout must not release a still-current target");
    }
    {
        setNodeParameter(NodeGraph::NodeKind::AutoShootEnabled,"enabled",0);
    }
    waitFor(state, [&] { return state->ups == 1; }, "Disabling auto-shoot must release its press");
}

void loopExitReleasesButton()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    {
        MouseLoop loop(*mouse);
        publishTarget();
        waitFor(state, [&] { return state->downs == 1; }, "Auto-shoot did not press before exit");
    }
    require(state->ups == 1, "Leaving the mouse loop must release its press");
    mouse->dispatchGraphMovement(NodeGraph::Movement{{{1,1}}});
    require(state->moves == 0, "Stopped mouse loop must not dispatch more input");
}

void pauseRejectsLateResultsAndReleasesButton()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    MouseLoop loop(*mouse);
    publishTarget();
    waitFor(state, [&] { return state->downs == 1; }, "Auto-shoot did not press before pause");
    const auto oldGeneration = detectionBuffer.currentGeneration();
    const auto oldFrameTime = Clock::now();
    detectionBuffer.togglePaused();
    waitFor(state, [&] { return state->ups == 1; }, "Pause must release our press");
    require(!detectionBuffer.setForGeneration(oldGeneration, { cv::Rect(140, 120, 40, 80) }, { 0 }, {}, Clock::now()),
        "In-flight result must be rejected during pause");
    detectionBuffer.togglePaused();
    require(!detectionBuffer.setForGeneration(oldGeneration, { cv::Rect(140, 120, 40, 80) }, { 0 }, {}, Clock::now()),
        "In-flight result must remain invalid after resume");
    require(!detectionBuffer.setForGeneration(detectionBuffer.currentGeneration(),
        { cv::Rect(140, 120, 40, 80) }, { 0 }, {}, oldFrameTime),
        "A frame captured before pause must not be relabeled as fresh");
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        require(!state->cv.wait_for(lock, 10ms, [&] { return state->downs > 1; }),
            "Resume must not resurrect the previous target");
    }
    publishTarget();
    waitFor(state, [&] { return state->downs == 2; }, "Fresh detection must work after resume");
}

void explicitPauseIsIdempotent()
{
    DetectionBuffer buffer;const auto generation=buffer.currentGeneration();
    buffer.setPaused(false);require(buffer.currentGeneration()==generation,"Setting the existing pause state invalidated frames");
    buffer.setPaused(true);require(buffer.isPaused()&&buffer.currentGeneration()==generation+1,"Explicit pause did not invalidate the generation");
    buffer.setPaused(true);require(buffer.currentGeneration()==generation+1,"Repeated MCP pause toggled or invalidated frames again");
    buffer.setPaused(false);require(!buffer.isPaused()&&buffer.currentGeneration()==generation+2,"Explicit resume did not invalidate the paused generation");
}

void pauseResumeInvalidatesQueuedMoves()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    state->blockMoves = true;
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    UnblockOnExit cleanup{ state };
    mouse->dispatchGraphMovement(NodeGraph::Movement{ { { 12, 0 } } });
    waitFor(state, [&] { return state->moves == 1; }, "Worker did not start a move");
    for (int i = 0; i < 4; ++i)
        mouse->dispatchGraphMovement(NodeGraph::Movement{ { { 12, 0 } } });
    unblock(state);
    detectionBuffer.togglePaused();
    int movesAtPause=0;
    {std::lock_guard<std::mutex> lock(state->mutex);movesAtPause=state->moves;}
    // Commands still carry the previous observation generation, including
    // commands submitted while paused. They must remain invalid after resume.
    for(int i=0;i<4;++i)mouse->dispatchGraphMovement(NodeGraph::Movement{{{12,0}}});
    detectionBuffer.togglePaused();
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        require(!state->cv.wait_for(lock, 10ms, [&] { return state->moves > movesAtPause; }),
            "Queued moves from before pause must not execute after resume");
    }
    mouse->stop();
}

void publicationDoesNotWaitForSlowInput()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    state->blockMoves = true;
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    UnblockOnExit cleanup{ state };
    const auto generation = detectionBuffer.currentGeneration();
    mouse->dispatchGraphMovement(NodeGraph::Movement{ { { 12, 0 } } });
    waitFor(state, [&] { return state->moves == 1; }, "Worker did not start a move");

    std::promise<bool> published;
    auto completion = published.get_future();
    std::thread inference([&] {
        published.set_value(detectionBuffer.setForGeneration(generation, {}, {}, {}, Clock::now()));
    });
    const bool ready = completion.wait_for(2s) == std::future_status::ready;
    // Always unblock and join before checking, including on a regression.
    unblock(state);
    inference.join();
    require(ready && completion.get(), "Slow input must not block inference publication");
    mouse->stop();
}

void verifyGraphEditReleasesButton(const std::function<void(NodeGraph::Graph&, int)>& edit)
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    MouseLoop loop(*mouse);
    publishTarget();
    waitFor(state, [&] { return state->downs == 1; }, "Starter graph did not press");

    auto workspace = NodeGraph::service().workspace();
    auto& graph = workspace.graphs[static_cast<std::size_t>(NodeGraph::GraphKind::AutoShoot)];
    int sinkId = 0;
    for (const auto& node : graph.nodes)
    {
        if (node.kind == NodeGraph::NodeKind::AutoShootOutput)
            sinkId = node.id;
    }
    require(sinkId != 0, "Starter graph needs auto-shoot output");
    edit(graph, sinkId);
    std::string error;
    require(NodeGraph::service().publish(workspace, error), "Disabled auto-shoot graph must compile");
    bool released = false;
    for (int i = 0; i < 30 && !released; ++i)
    {
        // Keep the target current so a stale-target timeout cannot pass this test.
        publishTarget();
        std::unique_lock<std::mutex> lock(state->mutex);
        released = state->cv.wait_for(lock, 2ms, [&] { return state->ups == 1; });
    }
    require(released, "Changing output graph must release its press with a current target");
    for (int i = 0; i < 10; ++i)
    {
        publishTarget();
        std::unique_lock<std::mutex> lock(state->mutex);
        require(!state->cv.wait_for(lock, 2ms, [&] { return state->downs > 1; }),
            "Disabled graph must stay released while fresh targets continue");
    }
}

void graphOutputReleasesButton()
{
    verifyGraphEditReleasesButton([](NodeGraph::Graph& graph, int sinkId) {
        int constantId = 1;
        for (const auto& node : graph.nodes)
            constantId = std::max(constantId, node.id + 1);
        graph.nodes.push_back({ constantId, NodeGraph::NodeKind::Boolean, 0.0f, 0.0f, 0.0 });
        for (auto& link : graph.links)
            if (link.toNode == sinkId && link.toPort == 0)
                link.fromNode = constantId;
    });
}

void disconnectingOutputReleasesButton()
{
    for (const int port : { 0, 1 })
        verifyGraphEditReleasesButton([port](NodeGraph::Graph& graph, int sinkId) {
            graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
                [sinkId, port](const NodeGraph::Link& link) {
                    return link.toNode == sinkId && link.toPort == port;
                }), graph.links.end());
        });
}

void deletingOutputReleasesButton()
{
    verifyGraphEditReleasesButton([](NodeGraph::Graph& graph, int sinkId) {
        graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
            [sinkId](const NodeGraph::Node& node) { return node.id == sinkId; }), graph.nodes.end());
        graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
            [sinkId](const NodeGraph::Link& link) {
                return link.fromNode == sinkId || link.toNode == sinkId;
            }), graph.links.end());
        for (auto& group : graph.groups)
            group.nodes.erase(std::remove(group.nodes.begin(), group.nodes.end(), sinkId), group.nodes.end());
    });
}

void bypassingOutputReleasesButton()
{
    verifyGraphEditReleasesButton([](NodeGraph::Graph& graph, int sinkId) {
        for (auto& node : graph.nodes)
            if (node.id == sinkId)
                node.bypassed = true;
    });
}

void layoutOnlyEditKeepsHeldButton()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    MouseLoop loop(*mouse);
    publishTarget();
    waitFor(state, [&] { return state->downs == 1; }, "Starter graph did not press before layout edit");
    auto workspace = NodeGraph::service().workspace();
    const auto revision = NodeGraph::service().revision();
    workspace.graphs[1].nodes.front().x += 64.0f;
    workspace.graphs[1].nodes.front().collapsed = true;
    workspace.graphs[1].panX += 32.0f;
    workspace.graphs[1].groups.front().title = "Moved controls";
    std::string error;
    require(NodeGraph::service().publish(workspace, error), "Layout-only graph edit must compile");
    require(NodeGraph::service().revision() == revision, "Layout-only edit must preserve execution revision");
    for (int i = 0; i < 15; ++i)
    {
        publishTarget();
        std::unique_lock<std::mutex> lock(state->mutex);
        require(!state->cv.wait_for(lock, 2ms, [&] { return state->ups != 0 || state->downs != 1; }),
            "Moving or collapsing nodes must not interrupt the held output");
    }
}

void graphCoordinatesDriveMovement()
{
    resetFixture();
    auto workspace = NodeGraph::service().workspace();
    auto& graph = workspace.graphs[static_cast<std::size_t>(NodeGraph::GraphKind::Aim)];
    graph.groups.clear();
    graph.nodes = {
        { 1, NodeGraph::NodeKind::Boolean, 0.0f, 0.0f, 1.0 },
        { 2, NodeGraph::NodeKind::Number, 0.0f, 0.0f, 100.0 },
        { 3, NodeGraph::NodeKind::Number, 0.0f, 0.0f, 160.0 },
        { 4, NodeGraph::NodeKind::MakePoint },
        { 5, NodeGraph::NodeKind::AimOutput },
        { 6, NodeGraph::NodeKind::Number, 0.0f, 0.0f, 1.0 },
        { 7, NodeGraph::NodeKind::MouseDelta },
        { 8, NodeGraph::NodeKind::Center }
    };
    graph.links = {
        { 1, 0, 5, 0 }, { 2, 0, 4, 0 }, { 3, 0, 4, 1 },
        { 4, 0, 7, 0 }, { 6, 0, 7, 1 }, { 8, 0, 7, 2 }, { 7, 0, 5, 1 }
    };
    // Disable the other output directly, so no target selection is evaluated
    // anywhere in this workspace despite a detection being available.
    auto& shootGraph = workspace.graphs[static_cast<std::size_t>(NodeGraph::GraphKind::AutoShoot)];
    int disabledSource = 0, shootSink = 0;
    for (auto& node : shootGraph.nodes)
    {
        if (node.kind == NodeGraph::NodeKind::AutoShootEnabled)
        {
            node.kind = NodeGraph::NodeKind::Boolean;
            node.value = 0.0;
            node.parameters.clear();node.textParameters.clear();
            disabledSource = node.id;
        }
        if (node.kind == NodeGraph::NodeKind::AutoShootOutput)
            shootSink = node.id;
    }
    for (auto& link : shootGraph.links)
        if (link.toNode == shootSink && link.toPort == 0)
            link.fromNode = disabledSource;
    std::string error;
    require(NodeGraph::service().publish(workspace, error), "Custom coordinates graph must compile");

    // No aiming hotkey or target source: this graph intentionally enables a
    // point assembled by the user and drives its own movement conversion.
    aiming = false;
    config.tracker_enabled = true;
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    MouseLoop loop(*mouse);
    require(detectionBuffer.setForGeneration(detectionBuffer.currentGeneration(),
        { cv::Rect(220, 120, 40, 80) }, { 0 }, { 0.9f }, Clock::now()),
        "Custom graph target must be accepted");
    waitFor(state, [&] { return state->moves > 0; }, "Graph gate did not activate movement");
    std::lock_guard<std::mutex> lock(state->mutex);
    require(state->lastDx < 0 && state->lastDy == 0,
        "Graph coordinates must drive movement rather than the detected pivot");
    {
        std::lock_guard<std::mutex> trackerLock(g_trackerDebugMutex);
        require(g_trackerDebugTracks.empty(), "Disconnected selection nodes must not run a hidden tracker");
    }
}

void recoilGraphWorksWithoutTarget()
{
    resetFixture();
    aiming = false;
    shooting = true;
    zooming = true;
    config.easynorecoil = true;
    config.easynorecoilstrength = 3.0f;
    setNodeParameter(NodeGraph::NodeKind::RecoilEnabled,"enabled",1);
    setNodeParameter(NodeGraph::NodeKind::RecoilStrength,"strength",3);
    setNodeParameter(NodeGraph::NodeKind::MouseProfile,"sens",2);
    setNodeParameter(NodeGraph::NodeKind::MouseProfile,"pitch",0.03);
    setNodeParameter(NodeGraph::NodeKind::MouseProfile,"fov_y",80);
    config.game_profiles.at("TEST").sens=1000;
    auto state = std::make_shared<FakeState>();
    state->blockMoves=true;
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    const auto since=Clock::now();bool moved=false;
    {
        MouseLoop loop(*mouse);
        {std::unique_lock<std::mutex> lock(state->mutex);moved=state->cv.wait_for(lock,2s,[&]{return state->moves>0;});}
        shouldExit=true;unblock(state);
    }
    require(moved,"Recoil graph must run without an aim target");
    require(state->lastDx == 0 && state->lastDy == 3,
        "Recoil graph must use configured strength independently of inactive aim");
    require(std::abs(mouse->getMotionCompensationSince(since).second-3*2*0.03*320/80)<1e-9,
        "The control loop failed to send node calibration to recoil feedback");
}
void editorCaptureReleasesAndBlocksInput()
{
    resetFixture();
    auto state=std::make_shared<FakeState>();FakeInput device(state);auto mouse=makeMouse(&device);
    MouseLoop loop(*mouse);publishTarget();
    waitFor(state,[&]{return state->downs==1;},"Input capture fixture did not press");
    editorInputCaptured=true;detectionBuffer.cv.notify_all();
    waitFor(state,[&]{return state->ups==1;},"Opening the editor must release the held button");
    shooting=true;zooming=true;setNodeParameter(NodeGraph::NodeKind::RecoilEnabled,"enabled",1);
    setNodeParameter(NodeGraph::NodeKind::RecoilStrength,"strength",3);
    publishTarget();mouse->dispatchGraphMovement(NodeGraph::Movement{{{7,0}}});mouse->pressMouse();
    std::unique_lock<std::mutex> lock(state->mutex);const int moves=state->moves;
    require(!state->cv.wait_for(lock,25ms,[&]{return state->moves>moves||state->downs>1;}),
        "Editor gestures must not trigger aim, recoil, queued movement or button presses");
    require(!graphAimActive.load(),"The editor must clear active control state");
    lock.unlock();shooting=false;zooming=false;editorInputCaptured=false;publishTarget();
    waitFor(state,[&]{return state->downs==2;},"Control must resume after the editor releases input");
}
void drawingSamplesLatestMotionWhileEditorOwnsInput()
{
    resetFixture();using namespace NodeGraph;
    auto workspace=service().workspace();for(auto& graph:workspace.graphs){graph.nodes.clear();graph.links.clear();graph.groups.clear();graph.ties.clear();}
    auto& graph=workspace.graphs[0];graph.nodes={defaultNode(NodeKind::Detections,1),defaultNode(NodeKind::ChooseTarget,2),defaultNode(NodeKind::TargetPosition,3),
        defaultNode(NodeKind::MotionEstimate,4),defaultNode(NodeKind::MotionVelocity,5),defaultNode(NodeKind::DrawPoint,6),defaultNode(NodeKind::RenderDrawing,7)};
    graph.nodes[3].parameters["kalman_enabled"]=1;graph.nodes[3].parameters["kalman_warmup_frames"]=0;graph.nodes[6].parameters["game_overlay_max_fps"]=30;
    graph.links={{1,0,2,0},{2,0,3,0},{3,0,4,0},{4,0,5,0},{5,0,6,1},{6,0,7,1}};
    std::string error;require(service().publish(workspace,error),error.c_str());editorInputCaptured=true;
    auto state=std::make_shared<FakeState>();FakeInput device(state);auto mouse=makeMouse(&device);MouseLoop loop(*mouse);publishTarget();
    {
        std::unique_lock<std::mutex> lock(NodeVision::drawingMutex);
        require(NodeVision::drawingChanged.wait_for(lock,2s,[]{return NodeVision::drawingCalls>=1;}),"Opening the editor stopped the graph drawing worker");
    }
    require(detectionBuffer.setForGeneration(detectionBuffer.currentGeneration(),{cv::Rect(180,120,40,80)},{0},{0.9f},Clock::now()),"Drawing observation was rejected");
    {
        std::unique_lock<std::mutex> lock(NodeVision::drawingMutex);
        require(NodeVision::drawingChanged.wait_for(lock,2s,[]{return NodeVision::drawingCalls>=2;}),"Throttled drawing did not refresh");
        require(NodeVision::testDrawing&&NodeVision::testDrawing->commands[0].from.x>0,"A drawing sampled after frame arrival failed to update estimated velocity");
    }
    std::lock_guard<std::mutex> lock(state->mutex);require(state->moves==0&&state->downs==0,"Visual drawing injected physical controls while editing");
}

void explicitScopeNodeControlsShooting()
{
    resetFixture();
    auto workspace = NodeGraph::service().workspace();
    auto& graph = workspace.graphs[static_cast<std::size_t>(NodeGraph::GraphKind::AutoShoot)];
    int scopeId = 0;
    for (auto& node : graph.nodes)
    {
        if (node.kind == NodeGraph::NodeKind::BoxContainsPoint)
        {
            scopeId = node.id;
            node.kind = NodeGraph::NodeKind::Boolean;
            node.value = 1.0;
            node.parameters.clear();node.textParameters.clear();
        }
    }
    require(scopeId != 0, "Starter graph needs explicit scope check");
    graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
        [scopeId](const NodeGraph::Link& link) { return link.toNode == scopeId; }), graph.links.end());
    std::string error;
    require(NodeGraph::service().publish(workspace, error), "Explicit scope bypass graph must compile");
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    MouseLoop loop(*mouse);
    require(detectionBuffer.setForGeneration(detectionBuffer.currentGeneration(),
        { cv::Rect(220, 120, 40, 80) }, { 0 }, { 0.9f }, Clock::now()),
        "Scope graph target must be accepted");
    waitFor(state, [&] { return state->downs == 1; },
        "Replacing ScopeCheck must remove the scope condition instead of retaining a hidden gate");
}

void graphApplyDoesNotResurrectExpiredTarget()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    MouseLoop loop(*mouse);
    publishTarget();
    waitFor(state, [&] { return state->downs == 1; }, "Starter graph did not press before expiry");
    waitFor(state, [&] { return state->ups == 1; }, "Stale target must release shooting");
    std::string error;
    require(NodeGraph::service().publish(NodeGraph::service().workspace(), error), "Reapplying graph must compile");
    std::unique_lock<std::mutex> lock(state->mutex);
    require(!state->cv.wait_for(lock, 15ms, [&] { return state->downs > 1; }),
        "Reapplying graph must not relabel old detections as current targets");
}

void modularPredictionMatchesOriginalFilter()
{
    using namespace NodeGraph;
    for(bool enabled:{false,true})for(int warmup:{0,2,20})for(double damping:{0.0,0.08,3.0})
    {
        resetFixture();captureFps=60;auto mouse=makeMouse(nullptr);
        Workspace original;for(std::size_t g=0;g<GraphCount;++g)original.graphs[g].kind=static_cast<GraphKind>(g);
        auto& graph=original.graphs[0];
        for(auto entry:std::vector<std::pair<int,NodeKind>>{{1,NodeKind::Boolean},{2,NodeKind::Center},{3,NodeKind::Prediction},{4,NodeKind::PointX},{5,NodeKind::RecoilOutput},{6,NodeKind::ZoomHeld}})
            graph.nodes.push_back(defaultNode(entry.second,entry.first));
        auto& prediction=graph.nodes[2];prediction.parameters["lookahead"]=0.1;prediction.parameters["kalman_enabled"]=enabled;
        prediction.parameters["kalman_velocity_damping"]=damping;prediction.parameters["kalman_warmup_frames"]=warmup;
        prediction.parameters["kalman_additional_prediction_ms"]=-80;prediction.parameters["draw_futurePositions"]=1;
        graph.links={{2,0,3,0},{6,0,3,2},{3,0,4,0},{1,0,5,0},{4,0,5,1}};
        const auto pin=[](const char* key){const auto& inputs=definition(NodeKind::Prediction).inputs;for(std::size_t i=0;i<inputs.size();++i)if(inputs[i].defaultParameter&&std::string(inputs[i].defaultParameter)==key)return static_cast<int>(i);throw std::runtime_error("Missing prediction binding");};
        auto lead=defaultNode(NodeKind::Number,7);lead.value=0.1;auto decay=defaultNode(NodeKind::Number,8);decay.value=damping;
        graph.nodes.push_back(lead);graph.nodes.push_back(decay);
        graph.links.push_back({7,0,3,1});graph.links.push_back({8,0,3,pin("kalman_velocity_damping")});graph.links.push_back({1,0,3,pin("draw_futurePositions")});
        std::string text,error;require(serialize(original,text,error),"Cannot serialize the combined prediction fixture");
        text.replace(0,text.find('\n'),"SUNONE_NODE_GRAPH 17");Workspace expanded;
        require(deserialize(text,expanded,error),"An old prediction graph did not expand");
        auto& split=expanded.graphs[0];int estimator=0,position=0;
        for(const auto& node:split.nodes){if(node.kind==NodeKind::MotionEstimate)estimator=node.id;if(node.kind==NodeKind::MotionPosition)position=node.id;}
        split.nodes.push_back(defaultNode(NodeKind::SetMotion,90));split.nodes.push_back(defaultNode(NodeKind::GetMotion,91));split.nodes.push_back(defaultNode(NodeKind::RerouteMotion,92));
        for(auto& link:split.links)if(link.toNode==position&&link.toPort==0)link.fromNode=92;
        split.links.push_back({estimator,0,90,0});split.links.push_back({91,0,92,0});
        Service service;require(service.publish(expanded,error),"Expanded prediction graph did not compile");
        Context context;context.captureFps=60;context.observationDelaySeconds=0.035;
        auto observed=Clock::now();int updates=0;std::vector<Point> trail;
        context.estimateMotion=[&](int key,Point point,const Node& node,bool reset){++updates;return mouse->graphEstimateMotion(key,point,observed,node,reset);};
        context.futureTrail=[&](const MotionData& motion,double seconds,const Node& node){trail.clear();for(int i=1;i<=parameter(node,"prediction_futurePositions");++i)trail.push_back(projectPoint(motion.position,motion.velocity,seconds+i/60.0,motion.damping,motion.futureReady));};
        aim::AimKalman2D reference;aim::AimKalmanSettings settings;
        settings.enabled=enabled;settings.velocity_damping=damping;settings.warmup_frames=warmup;
        for(int frame=0;frame<24;++frame)
        {
            const double gap=frame==0?0:frame==14?0.7:frame==19?-0.016:0.016;
            observed+=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(gap));
            context.center={120+frame*1.7,140+std::sin(frame*0.3)*8};context.zoomHeld=frame==8;updates=0;
            reference.setSettings(settings);if(context.zoomHeld||gap<0||gap>0.5)reference.reset();
            const double seconds=0.1+0.035-0.08;
            const auto expected=reference.update(context.center.x,context.center.y,gap>0?std::clamp(gap,1.0/500,0.25):1.0/60,seconds);
            const auto actual=service.evaluate(context);
            require(actual.recoil&&updates==1,"Position, velocity and trail advanced the filter more than once per observation");
            require(std::abs(actual.recoilStrength-expected.predicted_x)<1e-8,"Modular projection changed Kalman, damping, warmup, delay or reset behavior");
            require(trail.size()==20,"Modular prediction lost future positions");
            for(std::size_t i=0;i<trail.size();++i)
            {const auto point=reference.predict(seconds+(i+1)/60.0);require(std::abs(point.first-trail[i].x)<1e-8&&std::abs(point.second-trail[i].y)<1e-8,"Modular prediction changed the future trail");}
        }
    }
}
void predictionNodesKeepIndependentState()
{
    resetFixture();
    config.kalman_enabled = true;
    config.kalman_process_noise_position = 40.0f;
    config.kalman_process_noise_velocity = 1800.0f;
    config.kalman_measurement_noise = 35.0f;
    config.kalman_velocity_damping = 0.08f;
    config.kalman_max_velocity = 20000.0f;
    config.kalman_warmup_frames = 0;
    config.predictionInterval = 0.1f;
    auto interleaved = makeMouse(nullptr);
    auto isolated = makeMouse(nullptr);
    const auto time = Clock::now();
    auto prediction=NodeGraph::defaultNode(NodeGraph::NodeKind::Prediction,1);
    prediction.parameters["kalman_warmup_frames"]=0;
    prediction.parameters["kalman_velocity_damping"]=0.08;
    prediction.parameters["kalman_compensate_detection_delay"]=0;
    interleaved->graphPredict(1, 100, 160, time,prediction,0.1,false);
    interleaved->graphPredict(2, 700, 160, time,prediction,0.1,false);
    isolated->graphPredict(1, 100, 160, time,prediction,0.1,false);
    const auto actual = interleaved->graphPredict(1, 115, 160, time + 16ms,prediction,0.1,false);
    const auto expected = isolated->graphPredict(1, 115, 160, time + 16ms,prediction,0.1,false);
    require(std::abs(actual.first - expected.first) < 0.000001 &&
        std::abs(actual.second - expected.second) < 0.000001,
        "Prediction node filters must not consume another node's observations");
}

void nodeProfilesIgnoreGlobalTuning()
{
    resetFixture();
    auto mouse=makeMouse(nullptr);
    auto a=NodeGraph::defaultNode(NodeGraph::NodeKind::MouseDelta,1),b=a;
    a.parameters["sens"]=b.parameters["sens"]=1;
    a.parameters["yaw"]=a.parameters["pitch"]=1;
    b.parameters["yaw"]=b.parameters["pitch"]=0.5;
    const auto first=mouse->graphMouseDelta(240,160,1,{160,160},a);
    const auto second=mouse->graphMouseDelta(240,160,1,{160,160},b);
    require(!first.steps.empty()&&!second.steps.empty()&&second.steps[0].dx>first.steps[0].dx*1.9,
        "Mouse Delta instances must have independent sensitivity");
    config.game_profiles.at("TEST").sens=100;
    config.fovX=1;
    const auto after=mouse->graphMouseDelta(240,160,1,{160,160},a);
    require(after.steps[0].dx==first.steps[0].dx&&after.pixelScaleX==first.pixelScaleX,
        "Node-owned profile must not read later global settings");
}

void cancelingAimQueueKeepsOtherWindState()
{
    resetFixture();
    auto mouse=makeMouse(nullptr);
    auto wind=NodeGraph::defaultNode(NodeGraph::NodeKind::WindMotion,1);
    wind.parameters["wind_G"]=0.05;wind.parameters["wind_W"]=0;
    wind.parameters["wind_M"]=10;wind.parameters["wind_D"]=100;
    NodeGraph::Movement input;input.steps.push_back({1,0});
    int moved=0;
    for(int i=0;i<40;++i)
    {
        const auto output=mouse->graphWindMotion(1000002,input,wind);
        for(const auto& step:output.steps)moved+=step.dx;
        // This cancellation happens for an inactive Aim output even while a
        // different graph continues using its own Wind node.
        mouse->clearQueuedMoves();
    }
    require(moved>0,"Canceling the aim queue must not erase another active Wind node's residual state");
}

void predictionInShootBranchSurvivesInactiveAim()
{
    resetFixture();
    using K=NodeGraph::NodeKind;
    auto workspace=NodeGraph::service().workspace();
    workspace.graphs[0].nodes.clear();workspace.graphs[0].links.clear();workspace.graphs[0].groups.clear();
    auto& graph=workspace.graphs[1];graph.nodes.clear();graph.links.clear();graph.groups.clear();
    for(const auto& entry:std::vector<std::pair<int,K>>{{1,K::Detections},{2,K::TargetNearest},{3,K::TargetObserved},
        {4,K::AimPoint},{5,K::Prediction},{6,K::PointX},{7,K::Greater},{8,K::Number},{9,K::AutoShootOutput}})
        graph.nodes.push_back(NodeGraph::defaultNode(entry.second,entry.first));
    graph.nodes[4].parameters["kalman_warmup_frames"]=0;
    graph.nodes[4].parameters["kalman_compensate_detection_delay"]=0;
    graph.nodes[4].parameters["lookahead"]=0.2;
    graph.nodes[7].value=1;
    graph.links={{1,0,2,0},{2,0,3,0},{2,0,4,0},{4,0,5,0},{5,0,6,0},{6,0,7,0},{8,0,7,1},{7,0,9,0},{2,0,9,1}};
    std::string error;require(NodeGraph::service().publish(workspace,error),"Shoot prediction graph must compile");
    auto state=std::make_shared<FakeState>();FakeInput input(state);auto mouse=makeMouse(&input);MouseLoop loop(*mouse);
    bool advanced=false;
    for(int frame=0;frame<20&&!advanced;++frame)
    {
        const int x=80+frame*5;
        require(detectionBuffer.setForGeneration(detectionBuffer.currentGeneration(),{cv::Rect(x,120,40,80)},{0},{0.9f},Clock::now()),"Moving observation must publish");
        std::this_thread::sleep_for(20ms);
        const auto telemetry=NodeGraph::service().telemetry();
        double raw=0,predicted=0;bool hasRaw=false,hasPrediction=false;
        for(const auto& sample:telemetry.graphs[1])
        {
            if(sample.nodeId==4&&sample.value.valid){raw=sample.value.point.x;hasRaw=true;}
            if(sample.nodeId==5&&sample.value.valid){predicted=sample.value.point.x;hasPrediction=true;}
        }
        advanced=hasRaw&&hasPrediction&&predicted>raw+1;
    }
    require(advanced,"Prediction used only by Auto Shoot must accumulate state while Aim output is inactive");
    require(!graphAimActive.load(),"Shoot-only prediction must not implicitly activate aiming");
}

void selectionNodesUseTheirOwnFilteredStreams()
{
    resetFixture();
    using K=NodeGraph::NodeKind;
    auto workspace=NodeGraph::service().workspace();
    for(int index=0;index<2;++index)
    {
        auto& graph=workspace.graphs[index];graph.nodes.clear();graph.links.clear();graph.groups.clear();
        for(const auto& entry:std::vector<std::pair<int,K>>{{1,K::Detections},{2,K::FilterClasses},{3,K::TargetTracker},
            {4,K::Boolean},{5,K::TargetObserved}}) graph.nodes.push_back(NodeGraph::defaultNode(entry.second,entry.first));
        graph.nodes[1].parameters["class_a"]=index;graph.nodes[1].parameters["class_b_enabled"]=0;
        graph.nodes[3].value=1;
        graph.links={{1,0,2,0},{2,0,3,0},{4,0,3,1},{3,0,5,0}};
        if(index==0)
        {
            for(const auto& entry:std::vector<std::pair<int,K>>{{6,K::AimPoint},{7,K::MouseDelta},{8,K::Number},{9,K::Center},{10,K::AimOutput}})
                graph.nodes.push_back(NodeGraph::defaultNode(entry.second,entry.first));
            graph.nodes[7].value=1;
            graph.links.insert(graph.links.end(),{{3,0,6,0},{6,0,7,0},{8,0,7,1},{9,0,7,2},{5,0,10,0},{7,0,10,1}});
        }
        else
        {
            graph.nodes.push_back(NodeGraph::defaultNode(K::AutoShootOutput,10));
            graph.links.insert(graph.links.end(),{{5,0,10,0},{3,0,10,1}});
        }
    }
    std::string error;require(NodeGraph::service().publish(workspace,error),"Independent selection graphs must compile");
    auto state=std::make_shared<FakeState>();FakeInput input(state);auto mouse=makeMouse(&input);MouseLoop loop(*mouse);
    bool matched=false;
    for(int frame=0;frame<20&&!matched;++frame)
    {
        require(detectionBuffer.setForGeneration(detectionBuffer.currentGeneration(),
            {cv::Rect(140,120,40,80),cv::Rect(200,120,20,30)},{0,1},{0.25f,0.95f},Clock::now()),"Class-separated detections must publish");
        std::this_thread::sleep_for(10ms);
        const auto telemetry=NodeGraph::service().telemetry();bool first=false,second=false;
        for(const auto& sample:telemetry.graphs[0])if(sample.nodeId==3&&sample.value.valid)
            first=sample.value.target.valid&&sample.value.target.classId==0&&std::abs(sample.value.target.confidence-0.25)<0.001;
        for(const auto& sample:telemetry.graphs[1])if(sample.nodeId==3&&sample.value.valid)
            second=sample.value.target.valid&&sample.value.target.classId==1&&std::abs(sample.value.target.confidence-0.95)<0.001;
        matched=first&&second;
    }
    require(matched,"Each selection node must consume its own filtered list, class and confidence");
}
void applicationReloadPreservesGraphSettings()
{
    resetFixture();Config current=config;current.capture_method="duplication_api";current.ai_model="graph.onnx";current.capture_fps=91;current.arduino_port="COM4";
    const auto path=std::filesystem::temp_directory_path()/("sunone_app_reload_"+std::to_string(Clock::now().time_since_epoch().count())+".ini");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove(path,error);}} cleanup{path};
    {std::ofstream file(path);file<<"capture_method = udp_capture\ndetection_resolution = 640\ncapture_fps = 30\nai_model = unrelated.onnx\nfovX = 120\narduino_port = COM99\ninput_method = ARDUINO\nbutton_exit = F10\nlocal_mcp_enabled = false\n[Games]\nOTHER = 10,0.05,0.05\n";}
    const auto previous=current;require(current.reloadApplicationSettings(path.string()),"Application reload failed");
    require(current.capture_method=="duplication_api"&&current.detection_resolution==320&&current.capture_fps==91&&current.ai_model=="graph.onnx"&&current.fovX==previous.fovX,"Reload imported old INI values into graph-owned runtime settings");
    require(current.arduino_port=="COM99"&&current.button_exit==std::vector<std::string>{"F10"}&&!current.sameInputDeviceSettings(previous),"Application reload lost device/hotkey changes");
    require(!current.local_mcp_enabled,"Application reload re-enabled disabled MCP access");
    auto uiOnly=current;uiOnly.overlay_opacity=252;require(uiOnly.sameInputDeviceSettings(current),"An appearance change requires a device reconnect");
}
void physicalKeyNodesRejectInjectedMouseButtons()
{
    struct Physical final:IMouseInput
    {
        bool left=false,f6=false;const char* name()const override{return "FAKE";}bool isOpen()const override{return true;}
        bool move(int,int)override{return true;}bool leftDown()override{return true;}bool leftUp()override{return true;}
        bool hasPhysicalButtonState()const override{return true;}
        bool keyPressed(const std::string& name)override{return name=="LeftMouseButton"?left:name=="F6"&&f6;}
    } input;
    require(!GraphInput::resolveKey(VK_LBUTTON,&input,true),"An injected mouse press feeds back into a physical key node");
    input.left=true;require(GraphInput::resolveKey(VK_LBUTTON,&input,false),"Key nodes do not read physical device buttons");
    input.f6=true;require(GraphInput::resolveKey(VK_F6,&input,false)&&GraphInput::resolveKey(VK_F7,&input,true),"Device keyboard input or Win32 fallback was lost");
    require(!GraphInput::resolveKey(0,&input,true)&&!GraphInput::resolveKey(256,&input,true),"Invalid virtual keys were accepted");
    editorInputCaptured=true;require(!GraphInput::keyDown(VK_LBUTTON),"Key nodes bypass menu input capture");editorInputCaptured=false;
}

}

int main()
{
    const std::pair<const char*, std::function<void()>> tests[] = {
        { "application reload preserves graph settings", applicationReloadPreservesGraphSettings },
        { "physical key nodes reject synthetic feedback", physicalKeyNodesRejectInjectedMouseButtons },
        { "idle stop and device lifetime", idleStopAndDeviceLifetime },
        { "stop discards backlog", stopDiscardsBacklog },
        { "device switch releases button", deviceSwitchReleasesOldButton },
        { "capture geometry uses node tuning", captureGeometryUsesOnlyNodeTuning },
        { "split mouse conversion preserves precision", splitMouseConversionMatchesCombined },
        { "motion feedback uses graph calibration", motionFeedbackUsesOnlyGraphCalibration },
        { "auto-shoot hold and disable", autoShootHoldsThenReleasesWhenDisabled },
        { "loop exit releases button", loopExitReleasesButton },
        { "pause rejects late inference", pauseRejectsLateResultsAndReleasesButton },
        { "explicit pause is idempotent", explicitPauseIsIdempotent },
        { "pause/resume cancels queued moves", pauseResumeInvalidatesQueuedMoves },
        { "publication independent of slow input", publicationDoesNotWaitForSlowInput },
        { "graph output releases button", graphOutputReleasesButton },
        { "disconnecting output releases button", disconnectingOutputReleasesButton },
        { "deleting output releases button", deletingOutputReleasesButton },
        { "bypassing output releases button", bypassingOutputReleasesButton },
        { "layout-only graph edit keeps held output", layoutOnlyEditKeepsHeldButton },
        { "graph coordinates drive movement", graphCoordinatesDriveMovement },
        { "recoil graph without target", recoilGraphWorksWithoutTarget },
        { "editor capture releases and blocks input", editorCaptureReleasesAndBlocksInput },
        { "drawing samples latest motion while editing", drawingSamplesLatestMotionWhileEditorOwnsInput },
        { "explicit scope node controls shooting", explicitScopeNodeControlsShooting },
        { "graph apply keeps expired target invalid", graphApplyDoesNotResurrectExpiredTarget },
        { "prediction nodes keep independent state", predictionNodesKeepIndependentState }
        ,{ "modular prediction preserves original filter", modularPredictionMatchesOriginalFilter }
        ,{ "node profiles ignore global tuning", nodeProfilesIgnoreGlobalTuning }
        ,{ "queue cancel keeps other wind state", cancelingAimQueueKeepsOtherWindState }
        ,{ "shoot prediction survives inactive aim", predictionInShootBranchSurvivesInactiveAim }
        ,{ "selection nodes use filtered streams", selectionNodesUseTheirOwnFilteredStreams }
    };
    try
    {
        for (const auto& test : tests)
        {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
