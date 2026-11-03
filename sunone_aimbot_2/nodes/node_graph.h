#ifndef SUNONE_NODE_GRAPH_H
#define SUNONE_NODE_GRAPH_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace NodeGraph
{
constexpr std::size_t MaxNodes = 128;
constexpr std::size_t MaxLinks = 256;
constexpr std::size_t MaxGroups = 32;
constexpr std::size_t MaxWireTies = 64;
constexpr std::size_t MaxTiesPerLink = 16;
constexpr std::size_t MaxPins = 64;
constexpr std::size_t MaxDataPins = 8;
constexpr std::size_t MaxLivePreviews = 4;
constexpr std::size_t GraphCount = 4;
constexpr std::size_t MaxGraphs = 64;
constexpr std::size_t MaxWorkflows = MaxGraphs;
constexpr int NodeKeyStride = 1000001;
constexpr std::size_t MaxWorkspaceBytes = 4 * 1024 * 1024;
constexpr int CurrentFormatVersion = 20;
constexpr std::int32_t MinInteger = -2147483647 - 1, MaxInteger = 2147483647;
enum class GraphKind { Aim, AutoShoot, Recoil, Vision };
enum class ValueType { Boolean, Float, Point, Detections, Target, Movement, Frame, Depth, Mask, Integer, String, Monitor, Monitors, OverlayStyle, LabelOptions, Motion, Calibration, Number = Float };
enum class NodeKind
{
    AimHeld, ShootHeld, ZoomHeld, TargetValid, TargetObserved,
    AutoShootEnabled, RecoilEnabled, RecoilStrength, Target, Center,
    TargetDistance, CaptureFps, Boolean, Number, And, Or, Not,
    Greater, Less, Add, Subtract, Multiply, Divide, Clamp,
    PointX, PointY, MakePoint, OffsetPoint, Distance, Prediction,
    AimOutput, AutoShootOutput, RecoilOutput,
    Detections, AimPoint, ScopeCheck, SpeedCurve, MouseDelta, WindMotion, AutoAimEnabled,
    DeadZone, LerpPoint, ScaleMovement, LimitMovement, Pulse,
    RerouteBoolean, RerouteNumber, ReroutePoint, RerouteDetections, RerouteTarget, RerouteMovement,
    FilterClasses, FilterConfidence, FilterSize, FilterCircle, PreferClass,
    TargetNearest, TargetTracker, TargetClass, TargetConfidence, TargetTrackId,
    TargetVelocity, TargetMissedFrames, TargetAge, TargetPosition, TargetSize,
    CameraCompensation, NormalizeFps, KeyHeld, KeyPressed, KeyReleased, Toggle,
    CaptureSource, Inference, DepthInference, DepthToMask, ApplyMask, FilterMask,
    FilterNms, LimitDetections, CollectDataset, RenderOverlay, VisionOutput,
    ControlActive, RerouteFrame, RerouteDepth, RerouteMask, ScreenshotOutput,
    Integer, ToFloat, ToInteger, RerouteInteger,
    IntegerAdd, IntegerSubtract, IntegerMultiply, IntegerDivide, IntegerClamp,
    String, ConcatString, IntegerToString, FloatToString,
    GetMonitors, MonitorAt, PrimaryMonitor, MonitorCount, MonitorIndex, MonitorName, MonitorSize, MonitorPosition, MonitorPrimary,
    SelectBoolean, SelectFloat, SelectInteger, SelectString, SelectPoint, SelectMonitor, SelectMonitors,
    RerouteString, RerouteMonitor, RerouteMonitors,
    GetBoolean, SetBoolean, GetFloat, SetFloat, GetInteger, SetInteger, GetString, SetString,
    GetPoint, SetPoint, GetMonitor, SetMonitor, GetMonitorsValue, SetMonitorsValue,
    GetDetections, SetDetections, GetTarget, SetTarget, GetMovement, SetMovement,
    GetFrame, SetFrame, GetDepth, SetDepth, GetMask, SetMask, ImagePreview,
    OverlayBoxes, OverlayFrame, OverlayMotion, OverlayIcon, OverlayDepth, OverlayCorrection, OverlayOutput,
    DatasetLabels, DatasetOutput, RerouteOverlay, RerouteLabels, GetOverlay, SetOverlay, GetLabels, SetLabels, CurrentFrame, MouseCalibration, RecoilProfileOutput,
    ChooseTarget, TrackDetections, BoxPoint, TargetAnchor, BoxContainsPoint,
    MotionEstimate, MotionPosition, MotionVelocity, MotionDamping, MotionReady, PredictionTime, ProjectPoint, FutureTrail,
    RerouteMotion, GetMotion, SetMotion,
    MouseProfile, SharedCalibration, CalibrationScale, PointDelta, PixelsToCounts, ApplyMouseSpeed,
    SharedMouseDelta, SharedRecoilOutput, RerouteCalibration, GetCalibration, SetCalibration, SharedCalibrationScale, Count,
    Float = Number, RerouteFloat = RerouteNumber
};
struct Point { double x = 0.0; double y = 0.0; };
struct CalibrationData
{
    Point degreesPerPixel,countsPerDegree,pixelsPerCount;
    bool aimEnabled=false;
    std::string profileName;
};
using Calibration=std::shared_ptr<const CalibrationData>;
struct Node;
bool calibrationIdentifier(NodeKind kind);
CalibrationData mouseCalibration(const Node& node,Point frameSize);
struct MotionData
{
    Point position,velocity;
    double damping=0;
    bool ready=false,futureReady=false;
};
Point projectPoint(Point position,Point velocity,double seconds,double damping,bool ready=true);
struct MonitorInfo
{
    int index=0,winrtIndex=0,x=0,y=0,width=0,height=0;
    std::string device,name;
    bool primary=false;
};
using Monitor = std::shared_ptr<const MonitorInfo>;
using MonitorList = std::shared_ptr<const std::vector<MonitorInfo>>;
MonitorList systemMonitors();
struct Detection
{
    double x = 0.0, y = 0.0, width = 0.0, height = 0.0;
    int classId = -1;
    double confidence = 0.0;
};
struct DetectionFrame
{
    std::vector<Detection> objects;
    std::int64_t version = -1;
    std::uint64_t generation = 0;
    bool fresh = false;
    double timestampSeconds = 0.0;
    double width = 0.0, height = 0.0;
    std::string modelName;
};
using DetectionStream = std::shared_ptr<const DetectionFrame>;
struct FrameData
{
    std::shared_ptr<const void> storage;
    int width = 0, height = 0;
    std::int64_t version = -1;
    std::uint64_t generation = 0;
    double timestampSeconds = 0;
    bool gpu = false;
    std::string error;
};
using FrameStream = std::shared_ptr<const FrameData>;
struct MaskData
{
    std::shared_ptr<const void> storage;
    int width = 0, height = 0;
    std::int64_t version = -1;
    std::uint64_t generation = 0;
    double timestampSeconds = 0;
    bool gpu = false;
    std::string error;
};
using MaskStream = std::shared_ptr<const MaskData>;
struct TargetData
{
    Point point;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool valid = false;
    bool observed = false;
    bool fresh = false;
    int classId = -1;
    double confidence = 0.0;
    int trackId = -1;
    double velocityX = 0.0, velocityY = 0.0;
    int missedFrames = 0;
    double ageSeconds = 0.0;
    bool pointPrepared = false;
};
struct MoveStep { int dx = 0; int dy = 0; };
struct Movement
{
    std::vector<MoveStep> steps;
    double pixelScaleX = 0.0, pixelScaleY = 0.0;
    Point preciseCounts;
    bool hasPreciseCounts = false;
};
Movement pixelsToCounts(Point delta,const CalibrationData& calibration);
Movement applyMouseSpeed(const Movement& movement,double speed);
struct PreviewImage;
using Preview=std::shared_ptr<const PreviewImage>;
struct SettingsData
{
    std::map<std::string,double> numbers;
    std::map<std::string,std::string> texts;
};
using Settings=std::shared_ptr<const SettingsData>;
struct Value
{
    ValueType type = ValueType::Boolean;
    bool boolean = false;
    double number = 0.0;
    Point point;
    TargetData target;
    Movement movement;
    DetectionStream detections;
    FrameStream frame;
    MaskStream mask;
    std::string error;
    bool valid = true;
    std::int32_t integer = 0;
    std::string text;
    Monitor monitor;
    MonitorList monitors;
    Preview preview;
    Settings settings;
    MotionData motion;
    Calibration calibration;
};
struct PreviewImage
{
    int width=0,height=0,sourceWidth=0,sourceHeight=0;
    ValueType sourceType=ValueType::Frame;
    std::int64_t version=-1;
    std::uint64_t generation=0;
    std::vector<std::uint8_t> rgba;
};
struct Pin { const char* name; ValueType type; const char* defaultParameter = nullptr; bool option=false; };
enum class ParameterKind { Number, Integer, Boolean };
struct ParameterSpec
{
    const char* key;
    const char* label;
    double defaultValue, minValue, maxValue;
    ParameterKind kind = ParameterKind::Number;
    const char* choices = nullptr; // Optional labels separated by '|'.
};
struct TextParameterSpec
{
    const char* key;
    const char* label;
    const char* defaultValue;
    std::size_t maxLength = 128;
};
struct Definition
{
    NodeKind kind;
    const char* title;
    const char* category;
    const char* description;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    std::vector<ParameterSpec> parameters;
    std::vector<TextParameterSpec> textParameters;
    std::size_t dataInputs=0;
};
const std::vector<Definition>& definitions();
const Definition& definition(NodeKind kind);
bool canBypass(NodeKind kind);
const char* graphName(GraphKind kind);
const char* typeName(ValueType type);
bool canConnectTypes(ValueType output, ValueType input);
bool variableGet(NodeKind kind);
bool variableSet(NodeKind kind);
struct Node
{
    int id = 0;
    NodeKind kind = NodeKind::Boolean;
    float x = 0.0f;
    float y = 0.0f;
    double value = 0.0;
    bool bypassed = false;
    bool collapsed = false;
    std::map<std::string, double> parameters;
    std::map<std::string, std::string> textParameters;
    bool disabled = false;
    // Zero selects automatic geometry. These are durable canvas dimensions,
    // independent of semantic zoom, telemetry and the collapsed presentation.
    float width = 0;
    float height = 0;
    // Local literal fallbacks for scalar data inputs without a named setting.
    std::map<int,double> inputValues;
    std::map<int,std::string> inputTexts;
    int runtimeKey = 0; // Assigned by the compiled workspace; never serialized.
};
double parameter(const Node& node, const std::string& key);
std::string textParameter(const Node& node, const std::string& key);
void initializeParameters(Node& node);
Node defaultNode(NodeKind kind, int id, float x = 0, float y = 0);
Point mousePixelCalibration(const Node& node, Point frameSize);
struct WireBend
{
    float roundness = 0.58f;
    float twist = 0;
};
struct Link
{
    int fromNode = 0;
    int fromPort = 0;
    int toNode = 0;
    int toPort = 0;
    // Ordered visual waypoints. They never merge or transform signal values.
    std::vector<int> ties;
    WireBend bend; // Presentation only; relative to the wire's endpoints and ties.
};
struct WireTie
{
    int id = 0;
    float x = 0;
    float y = 0;
    float angle = 0; // Wire tangent in degrees; the strap is perpendicular.
};
struct Group
{
    int id = 0;
    std::string title;
    float x = 0.0f;
    float y = 0.0f;
    float width = 256.0f;
    float height = 192.0f;
    // Packed RGBA, identical to ImGui's standard IM_COL32 representation.
    std::uint32_t color = 0xffd9a74cu;
    std::vector<int> nodes;
};
struct Graph
{
    GraphKind kind = GraphKind::Aim;
    std::vector<Node> nodes;
    std::vector<Link> links;
    float panX = 40.0f;
    float panY = 40.0f;
    float zoom = 1.0f;
    std::vector<Group> groups;
    std::vector<WireTie> ties;
    int workflow = 0;
    std::string workflowName,pageName;
    int workflowIcon=-1; // -1 selects the icon of the original template; 0..15 are sidebar icons.
    bool active = true;
    bool enabled = true;
};
struct Workspace { std::vector<Graph> graphs=std::vector<Graph>(GraphCount); int sourceVersion = CurrentFormatVersion; };
void migrateRecoilCalibration(Workspace& workspace);
bool expandProcessingNodes(Graph& graph);
bool shareMouseCalibration(Workspace& workspace);
// Flatten legacy pages and fold the generated calibration workflow into Logic.
bool migrateWorkflowPages(Workspace& workspace);
int workflowId(const Graph& graph,std::size_t index);
std::string workflowTitle(const Graph& graph);
std::string pageTitle(const Graph& graph);
int activePage(const Workspace& workspace,int workflow);
int previewKey(int graph,int node);
Workspace defaults();
// Recognize historical starter geometry without replacing settings or wiring.
bool migrateStarterLayout(Graph& graph);
bool validate(const Workspace& workspace, std::string& error);
// Checks link types and cycles without requiring a complete output node.
bool canConnect(const Graph& graph, const Link& link, std::string& error);
struct Context
{
    bool inputCaptured = false;
    std::function<bool()> inputCapturedNow;
    bool aimHeld = false;
    bool shootHeld = false;
    bool zoomHeld = false;
    Point center;
    double captureFps = 0.0;
    double elapsedSeconds = 0.0;
    bool detectionsAvailable = false;
    DetectionStream detections;
    std::function<TargetData(int nodeKey, const DetectionStream&, bool lock, const Node&)> selectTarget;
    std::function<bool(const TargetData&, double multiplier, Point center)> inScope;
    std::function<double(Point, Point center, double maximumDistance, const Node&)> speedCurve;
    std::function<Movement(Point, double, Point center, const Node&)> mouseDelta;
    std::function<Movement(int nodeKey, const Movement&, const Node&)> windMotion;
    std::function<Point(int nodeKey, Point, const Node&, double lookahead, bool reset)> predict;
    std::function<Point(Point)> motionCompensation;
    std::function<MotionData(int nodeKey,Point,const Node&,bool reset)> estimateMotion;
    std::function<void(const MotionData&,double seconds,const Node&)> futureTrail;
    double observationDelaySeconds=0;
    std::function<bool(int virtualKey)> keyDown;
    std::function<MonitorList()> monitors;
    bool useConfigurationSnapshot=false;
    std::function<void()> captureConfigurationChanged;
    bool controlActive = false;
    FrameStream frame;
    std::function<FrameStream(int nodeKey,const Node&)> captureFrame;
    std::function<DetectionStream(int nodeKey,const FrameStream&,const Node&)> infer;
    std::function<FrameStream(int nodeKey,const FrameStream&,const Node&)> depth;
    std::function<MaskStream(const FrameStream&,const Node&)> depthToMask;
    std::function<FrameStream(const FrameStream&,const MaskStream&,const Node&)> applyMask;
    std::function<DetectionStream(const DetectionStream&,const MaskStream&,const Node&)> filterMask;
    std::function<bool(int nodeKey,const FrameStream&,const DetectionStream&,const Node&)> collect;
    std::function<bool(int nodeKey,const FrameStream&,const DetectionStream&,const MaskStream&,const FrameStream& depth,const Node&)> render;
    std::function<bool(int nodeKey,const FrameStream&,const Node&)> screenshot;
    std::function<Preview(const Value&,const DetectionStream&,const Node&)> preview;
    std::function<std::string(int nodeKey)> nodeError;
};
struct Actions
{
    bool aim = false;
    bool autoShoot = false;
    bool recoil = false;
    Point aimPoint;
    double recoilStrength = 0.0;
    double recoilPixelDeltaY = 0.0;
    TargetData shootTarget;
    Movement movement;
    std::vector<Movement> movements;
    std::vector<int> activePredictionKeys;
    std::vector<int> activeWindKeys;
    std::vector<int> activeSelectionKeys;
    DetectionStream visionDetections;
    std::vector<int> activeProviderKeys;
    std::vector<int> activeCollectorKeys;
    std::vector<int> activeRenderKeys;
    std::vector<int> activeDepthKeys;
    std::vector<int> activeMaskKeys;
    std::vector<int> activeScreenshotKeys;
    std::vector<Node> configuredProviders;
    // Program validity. Invalid values deactivate their own output branch only.
    bool valid = true;
    std::uint64_t revision = 0;
};
struct NodeValue
{
    int nodeId = 0;
    Value value;
    std::uint64_t signalSequence = 0;
    std::chrono::steady_clock::time_point signaledAt{};
    std::uint64_t readInputs = 0;
    bool active = false;
    bool signalActive = false;
};
struct Telemetry
{
    Actions actions;
    std::array<std::vector<NodeValue>, MaxGraphs> graphs;
    std::array<std::chrono::steady_clock::time_point,MaxGraphs> graphSampledAt{};
    std::array<std::uint64_t,MaxGraphs> graphRevisions{};
    std::chrono::steady_clock::time_point sampledAt{};
};
class Service
{
public:
    Service();
    Workspace workspace() const;
    Workspace workspaceMetadata() const;
    std::uint64_t revision() const;
    bool publish(const Workspace& workspace, std::string& error);
    Actions evaluate(const Context& context);
    Actions evaluateVision(const Context& context);
    std::vector<Node> providerNodes(NodeKind kind) const;
    std::vector<Node> evaluateConfiguration(const Context& context);
    bool hasDynamicProviders() const;
    Telemetry telemetry() const;
    void setTelemetryEnabled(bool enabled);
    void setPreviewNodes(std::vector<int> nodes);
    std::shared_ptr<const std::vector<int>> previewNodes() const;
    bool load(const std::string& path, std::string& error);
    bool save(const std::string& path, std::string& error) const;
private:
    struct Program;
    struct ConfigurationSnapshot;
    std::shared_ptr<const Program> program_;
    mutable std::mutex publishMutex_;
    mutable std::mutex telemetryMutex_;
    Telemetry telemetry_;
    std::atomic<bool> telemetryEnabled_{true};
    std::shared_ptr<const std::vector<int>> previewNodes_;
    std::atomic<std::int64_t> previewTouched_{0};
    std::shared_ptr<const ConfigurationSnapshot> configurationSnapshot_;
    std::mutex configurationMutex_;
    std::map<int,std::string> configurationErrors_;
    struct EventState { bool previous = false; bool toggled = false; bool reseed = false; };
    struct FilterState
    {
        DetectionStream input,output;
        std::array<double,4> arguments{};
    };
    struct DomainState
    {
        struct SignalState
        {
            int nodeId = 0;
            std::uint64_t stamp = 0, sequence = 0;
            std::chrono::steady_clock::time_point signaledAt{};
            std::uint64_t readInputs = 0;
            bool available = false, active = false, signalActive = false;
        };
        std::map<int,EventState> events;
        std::map<int,FilterState> filters;
        std::map<int,Node> configurations;
        struct SettingsState
        {
            Settings input,output;
            std::map<std::string,double> numbers;
            std::map<std::string,std::string> texts;
            Node resolved;
            bool resolvedOnce=false;
        };
        std::map<int,SettingsState> settings;
        struct CalibrationState
        {
            std::map<std::string,double> numbers;
            std::map<std::string,std::string> texts;
            Point size;
            Calibration value;
        };
        std::map<int,CalibrationState> calibrations;
        struct EvaluationFrame
        {
            std::vector<Value> values;
            std::vector<std::uint8_t> evaluated,visiting;
            std::vector<std::uint64_t> readInputs;
        };
        std::array<EvaluationFrame,MaxGraphs> evaluations;
        std::vector<std::pair<int,int>> pending;
        struct PreviewState
        {
            Preview image;
            std::array<std::uint64_t,5> stamp{};
            std::array<double,4> settings{};
            std::chrono::steady_clock::time_point updatedAt{};
            std::string error;
        };
        std::map<int,PreviewState> previews;
        std::array<std::vector<std::array<Value,MaxDataPins>>,MaxGraphs> inputStorage;
        std::array<std::vector<SignalState>,MaxGraphs> signals;
        std::shared_ptr<const Program> signalProgram;
        bool telemetryWasEnabled = true;
        std::uint64_t revision = 0;
        std::chrono::steady_clock::time_point sampledAt{};
    };
    std::array<DomainState,2> domains_;
    DomainState configurationDomain_;
    Actions evaluateDomain(const Context&,bool vision,bool configurationOnly=false);
};
// No implicit disk access. Startup explicitly loads the workspace once.
Service& service();
bool serialize(const Workspace& workspace, std::string& text, std::string& error);
bool deserialize(const std::string& text, Workspace& workspace, std::string& error);
}
#endif
