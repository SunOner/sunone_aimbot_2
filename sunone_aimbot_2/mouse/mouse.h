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
#include "config.h"
#include "nodes/node_graph.h"

class MouseThread
{
private:
    double screen_width;
    double screen_height;
    double prediction_interval;
    double fov_x;
    double fov_y;
    double max_distance;
    double min_speed_multiplier;
    double max_speed_multiplier;
    double center_x;
    double center_y;

    double prev_x, prev_y;
    double prev_velocity_x, prev_velocity_y;
    std::chrono::time_point<std::chrono::steady_clock> prev_time;
    std::chrono::steady_clock::time_point last_target_time;
    std::atomic<bool> target_detected{ false };
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
    std::mutex                    pendingConfigMutex;
    std::optional<Config>         pendingConfig;

    std::vector<std::pair<double, double>> futurePositions;
    std::mutex                    futurePositionsMutex;
    aim::AimKalman2D              targetKalman;
    aim::AimKalmanTelemetry       lastKalmanTelemetry;
    double                        lastPredictionLookaheadSec = 0.0;
    double                        lastDetectionDelaySec = 0.0;

    struct PredictionNodeState
    {
        double x = 0.0, y = 0.0;
        double velocityX = 0.0, velocityY = 0.0;
        std::chrono::steady_clock::time_point time{};
        aim::AimKalman2D kalman;
        aim::AimKalmanTelemetry telemetry;
        double lookaheadSec = 0.0;
        double detectionDelaySec = 0.0;
    };
    std::unordered_map<int, PredictionNodeState> graphPredictionStates;
    void restorePredictionState(const PredictionNodeState& state);

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
    std::pair<double, double> mouseCountsToScreenPixels(int dx, int dy) const;

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
    MouseThread(
        int  resolution,
        int  fovX,
        int  fovY,
        double minSpeedMultiplier,
        double maxSpeedMultiplier,
        double predictionInterval,
        IMouseInput* mouseInputDevice = nullptr
    );
    ~MouseThread();

    // Caller holds configMutex while copying config. Active state is changed
    // only by applyPendingConfig() on the mouse loop, never by UI callbacks.
    void updateConfig(const Config& snapshot);
    bool applyPendingConfig();
    void setDetectionGeneration(std::uint64_t generation) { detectionGeneration = generation; }
    void requestStop();
    void stop();

    // Explicit graph stages. Only dispatchGraphMovement queues device commands.
    double graphSpeedCurve(double pointX, double pointY, const NodeGraph::Node& node);
    NodeGraph::Movement graphMouseDelta(double pointX, double pointY, double speed,
        NodeGraph::Point center, const NodeGraph::Node& node);
    NodeGraph::Movement graphWindMotion(int nodeKey, const NodeGraph::Movement& movement,
        const NodeGraph::Node& node);
    std::pair<double, double> graphPredict(int nodeKey, double pointX, double pointY,
        std::chrono::steady_clock::time_point observationTime, const NodeGraph::Node& node,
        double lookahead, bool reset);
    std::vector<std::pair<double,double>> graphFuturePositions(int nodeKey, int frames) const;
    void retainGraphStates(const std::vector<int>& predictionKeys, const std::vector<int>& windKeys);
    void dispatchGraphMovement(const NodeGraph::Movement& movement);
    void moveRelative(int dx, int dy);
    void clearQueuedMoves();
    void pressMouse();
    void releaseMouse();
    void resetPrediction();
    bool check_target_in_scope(double target_x, double target_y,
        double target_w, double target_h, double reduction_factor);

    void storeFuturePositions(const std::vector<std::pair<double, double>>& positions);
    void clearFuturePositions();
    std::vector<std::pair<double, double>> getFuturePositions();
    void clearWindDebugTrail();
    std::vector<std::pair<double, double>> getWindDebugTrail();
    std::pair<double, double> getMotionCompensationSince(
        std::chrono::steady_clock::time_point since) const;

    void setMouseInput(IMouseInput* newMouseInput);

    void setTargetDetected(bool detected) { target_detected.store(detected); }
    void setLastTargetTime(const std::chrono::steady_clock::time_point& t) { last_target_time = t; }
};

#endif // MOUSE_H
