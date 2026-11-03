#ifndef MOUSE_H
#define MOUSE_H

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <utility>
#include <queue>
#include <thread>
#include <condition_variable>
#include <deque>
#include <random>
#include <optional>
#include <cstdint>
#include <unordered_map>

#include "MouseInput.h"
#include "aim_kalman.h"
#include "nodes/node_graph.h"

class MouseThread
{
private:
    double screen_width;
    double screen_height;
    double center_x;
    double center_y;
    std::atomic<bool> mouse_pressed{ false };

    IMouseInput* mouseInput;

    void sendMovementToDriver(int dx, int dy, std::uint64_t detectionGeneration,
        double pixelScaleX = 0.0, double pixelScaleY = 0.0);

    struct Move
    {
        int dx;
        int dy;
        std::uint64_t detectionGeneration;
        std::uint64_t queueGeneration;
        double pixelScaleX = 0.0;
        double pixelScaleY = 0.0;
    };

    std::queue<Move>              moveQueue;
    std::mutex                    queueMtx;
    std::condition_variable       queueCv;
    const size_t                  queueLimit = 5;
    std::thread                   moveWorker;
    std::atomic<bool>             workerStop{ false };
    std::mutex                    dispatchMutex;
    std::uint64_t                 queueGeneration = 0; // protected by queueMtx
    std::uint64_t                 detectionGeneration = 0; // mouse loop only

    std::vector<std::pair<double, double>> futurePositions;
    std::mutex                    futurePositionsMutex;
    struct PredictionNodeState
    {
        std::chrono::steady_clock::time_point time{};
        aim::AimKalman2D kalman;
        double lookaheadSec = 0.0;
    };
    std::unordered_map<int, PredictionNodeState> graphPredictionStates;

    void moveWorkerLoop();
    void queueMove(int dx, int dy, double pixelScaleX = 0.0, double pixelScaleY = 0.0);

    bool   wind_mouse_enabled = true;
    double wind_G, wind_W, wind_M, wind_D;
    NodeGraph::Movement buildWindSteps(int dx, int dy);
    NodeGraph::Movement graphWindMotion(const NodeGraph::Movement& movement);
    NodeGraph::Movement graphWindMotion(int nodeKey, const NodeGraph::Movement& movement);
    void   resetWindState();
    void   resetWindScalars();
    void   appendWindDebugStep(int dx, int dy, double pixelScaleX, double pixelScaleY);
    void   pruneWindDebugTrailLocked(const std::chrono::steady_clock::time_point& now);

    struct WindDebugPoint
    {
        double x = 0.0;
        double y = 0.0;
        std::chrono::steady_clock::time_point t{};
    };

    // Persistent wind state to avoid per-frame "reset" feel.
    double windCarryX = 0.0;
    double windCarryY = 0.0;
    double windVelX = 0.0;
    double windVelY = 0.0;
    double windNoiseX = 0.0;
    double windNoiseY = 0.0;
    double windFracX = 0.0;
    double windFracY = 0.0;
    double windPatternX = 0.0;
    double windPatternY = 0.0;
    double windPatternPhaseA = 0.0;
    double windPatternPhaseB = 0.0;
    double windPatternRateA = 0.0;
    double windPatternRateB = 0.0;
    std::mt19937 windRng{ std::random_device{}() };

    struct WindNodeState
    {
        double carryX = 0.0, carryY = 0.0;
        double velocityX = 0.0, velocityY = 0.0;
        double noiseX = 0.0, noiseY = 0.0;
        double fractionX = 0.0, fractionY = 0.0;
        double patternX = 0.0, patternY = 0.0;
        double phaseA = 0.0, phaseB = 0.0;
        double rateA = 0.0, rateB = 0.0;
        std::mt19937 rng;
    };
    std::unordered_map<int, WindNodeState> graphWindStates;
    WindNodeState captureWindState() const;
    void restoreWindState(const WindNodeState& state);

    std::deque<WindDebugPoint> windDebugTrail;
    std::mutex                             windDebugTrailMutex;
    double                                 windDebugCursorX = 0.0;
    double                                 windDebugCursorY = 0.0;

    struct MotionCompensationSample
    {
        double x = 0.0;
        double y = 0.0;
        std::chrono::steady_clock::time_point t{};
    };

    mutable std::mutex motionCompensationMutex;
    std::deque<MotionCompensationSample> motionCompensationTrail;
    void recordMotionCompensationStep(int dx, int dy, double pixelScaleX = 0.0, double pixelScaleY = 0.0);
    void pruneMotionCompensationTrailLocked(const std::chrono::steady_clock::time_point& now);

public:
    explicit MouseThread(int resolution, IMouseInput* mouseInputDevice = nullptr);
    ~MouseThread();
    // Capture geometry is supplied by the control loop. Tuning belongs to nodes.
    void setCaptureResolution(int resolution);
    void setDetectionGeneration(std::uint64_t generation) { detectionGeneration = generation; }
    void requestStop();
    void stop();

    // Explicit graph stages. Only dispatchGraphMovement queues device commands.
    double graphSpeedCurve(double pointX, double pointY, NodeGraph::Point center, double maximumDistance, const NodeGraph::Node& node);
    NodeGraph::Movement graphMouseDelta(double pointX, double pointY, double speed,
        NodeGraph::Point center, const NodeGraph::Node& node);
    NodeGraph::Movement graphWindMotion(int nodeKey, const NodeGraph::Movement& movement,
        const NodeGraph::Node& node);
    std::pair<double, double> graphPredict(int nodeKey, double pointX, double pointY,
        std::chrono::steady_clock::time_point observationTime, const NodeGraph::Node& node,
        double lookahead, bool reset);
    NodeGraph::MotionData graphEstimateMotion(int nodeKey,NodeGraph::Point point,
        std::chrono::steady_clock::time_point observationTime,const NodeGraph::Node& node,bool reset);
    std::vector<std::pair<double,double>> graphFuturePositions(int nodeKey, int frames) const;
    void retainGraphStates(const std::vector<int>& predictionKeys, const std::vector<int>& windKeys);
    void dispatchGraphMovement(const NodeGraph::Movement& movement);
    void dispatchGraphRecoil(int counts, double pixelsPerCount);
    void clearQueuedMoves();
    void pressMouse();
    void releaseMouse();
    void resetPrediction();

    void storeFuturePositions(const std::vector<std::pair<double, double>>& positions);
    void clearFuturePositions();
    std::vector<std::pair<double, double>> getFuturePositions();
    void clearWindDebugTrail();
    std::vector<std::pair<double, double>> getWindDebugTrail();
    std::pair<double, double> getMotionCompensationSince(
        std::chrono::steady_clock::time_point since) const;

    void setMouseInput(IMouseInput* newMouseInput);

};

#endif // MOUSE_H
