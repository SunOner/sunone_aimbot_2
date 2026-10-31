#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <cmath>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <atomic>
#include <vector>
#include <iostream>
#include <random>
#include <limits>

#include "mouse.h"
#include "capture.h"
#include "config.h"
#include "detection_buffer.h"
#include "runtime/input_state.h"

namespace
{
double currentFrameIntervalSec()
{
    double fps = static_cast<double>(captureFps.load());
    if (fps <= 0.0)
    {
        std::lock_guard<std::mutex> lock(configMutex);
        fps = (config.capture_fps > 0) ? static_cast<double>(config.capture_fps) : 60.0;
    }

    fps = std::clamp(fps, 15.0, 500.0);
    return 1.0 / fps;
}

template <typename States>
bool hasGraphStateCapacity(const States& states, int nodeKey)
{
    constexpr int graphStride = 1000001;
    if (nodeKey <= 0 || nodeKey >= graphStride * 3)
        return false;
    const int graph = nodeKey / graphStride;
    std::size_t count = 0;
    for (const auto& entry : states)
        if (entry.first / graphStride == graph)
            ++count;
    return count < NodeGraph::MaxNodes;
}
}

MouseThread::MouseThread(
    int resolution,
    int fovX,
    int fovY,
    double minSpeedMultiplier,
    double maxSpeedMultiplier,
    double predictionInterval,
    IMouseInput* mouseInputDevice)
    : screen_width(resolution),
    screen_height(resolution),
    prediction_interval(predictionInterval),
    fov_x(fovX),
    fov_y(fovY),
    max_distance(std::hypot(resolution, resolution) / 2.0),
    min_speed_multiplier(minSpeedMultiplier),
    max_speed_multiplier(maxSpeedMultiplier),
    center_x(resolution / 2.0),
    center_y(resolution / 2.0),
    mouseInput(mouseInputDevice),

    prev_velocity_x(0.0),
    prev_velocity_y(0.0),
    prev_x(0.0),
    prev_y(0.0)
{
    prev_time = std::chrono::steady_clock::time_point();
    last_target_time = std::chrono::steady_clock::now();

    wind_mouse_enabled=true;wind_G=9;wind_W=3;wind_M=10;wind_D=12;
    resetWindState();
    clearWindDebugTrail();
    targetKalman.setSettings(aim::AimKalmanSettings{});
    targetKalman.reset();
    lastKalmanTelemetry = {};
    lastPredictionLookaheadSec = 0.0;
    lastDetectionDelaySec = 0.0;

    detectionGeneration = detectionBuffer.currentGeneration();
    moveWorker = std::thread(&MouseThread::moveWorkerLoop, this);
}

void MouseThread::updateConfig(const Config& snapshot)
{
    std::lock_guard<std::mutex> lock(pendingConfigMutex);
    pendingConfig = snapshot;
}

bool MouseThread::applyPendingConfig()
{
    std::optional<Config> snapshot;
    {
        std::lock_guard<std::mutex> lock(pendingConfigMutex);
        snapshot.swap(pendingConfig);
    }
    if (!snapshot)
        return false;

    // Wait for dispatch and invalidate popped commands before changing the
    // geometry also read by the worker's motion compensation/debug code.
    resetPrediction();

    const int resolution = snapshot->detection_resolution;
    screen_width = screen_height = resolution;
    fov_x = snapshot->fovX;
    fov_y = snapshot->fovY;
    center_x = center_y = resolution / 2.0;
    max_distance = std::hypot(resolution, resolution) / 2.0;
    clearWindDebugTrail();
    targetKalman.setSettings(aim::AimKalmanSettings{});
    return true;
}

MouseThread::~MouseThread()
{
    stop();
}

void MouseThread::requestStop()
{
    {
        std::lock_guard<std::mutex> lock(queueMtx);
        workerStop = true;
        ++queueGeneration;
        while (!moveQueue.empty())
            moveQueue.pop();
    }
    queueCv.notify_all();
}

void MouseThread::stop()
{
    requestStop();
    if (moveWorker.joinable())
        moveWorker.join();
    setMouseInput(nullptr);
}

void MouseThread::queueMove(int dx, int dy, double pixelScaleX, double pixelScaleY)
{
    if (dx == 0 && dy == 0)
    {
        return;
    }

    std::lock_guard lg(queueMtx);
    if (workerStop)
        return;
    if (moveQueue.size() >= queueLimit) moveQueue.pop();
    moveQueue.push({ dx, dy, detectionGeneration, queueGeneration, pixelScaleX, pixelScaleY });
    queueCv.notify_one();
}

void MouseThread::moveWorkerLoop()
{
    try
    {
        for (;;)
        {
            std::unique_lock ul(queueMtx);
            queueCv.wait(ul, [&] { return workerStop || !moveQueue.empty(); });
            if (workerStop)
                break;

            Move m = moveQueue.front();
            moveQueue.pop();
            ul.unlock();

            std::lock_guard<std::mutex> dispatchLock(dispatchMutex);
            {
                std::lock_guard<std::mutex> queueLock(queueMtx);
                if (workerStop || m.queueGeneration != queueGeneration)
                    continue;
            }
            sendMovementToDriver(m.dx, m.dy, m.detectionGeneration, m.pixelScaleX, m.pixelScaleY);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Mouse] Move worker crashed: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[Mouse] Move worker crashed: unknown exception." << std::endl;
    }
}

NodeGraph::Movement MouseThread::buildWindSteps(int dx, int dy)
{
    NodeGraph::Movement movement;
    if (dx == 0 && dy == 0)
        return movement;
    movement.steps.reserve(5);

    windCarryX += static_cast<double>(dx);
    windCarryY += static_cast<double>(dy);

    const double baseG = std::clamp(wind_G, 0.05, 50.0);
    const double baseW = std::clamp(wind_W, 0.0, 80.0);
    const double baseM = std::max(1.0, wind_M);
    const double baseD = std::max(1.0, wind_D);
    if (!std::isfinite(baseG) || !std::isfinite(baseW) ||
        !std::isfinite(baseM) || !std::isfinite(baseD))
    {
        resetWindState();
        return movement;
    }

    std::uniform_real_distribution<double> noiseDist(-1.0, 1.0);
    std::uniform_real_distribution<double> clipDist(0.55, 1.0);
    constexpr double twoPi = 6.28318530717958647692;

    const double carryMag = std::hypot(windCarryX, windCarryY);
    const int maxSubsteps = std::clamp(static_cast<int>(carryMag * 0.24) + 1, 1, 5);

    for (int i = 0; i < maxSubsteps; ++i)
    {
        const double dist = std::hypot(windCarryX, windCarryY);
        const double velMag = std::hypot(windVelX, windVelY);

        if (dist < 0.20 && velMag < 0.12)
            break;

        const double normDist = std::clamp(dist / baseD, 0.0, 1.0);
        const double pullGain = baseG * (0.25 + 0.75 * normDist);
        const double noiseAmp = baseW * (0.15 + 0.85 * normDist);

        double pullX = 0.0;
        double pullY = 0.0;
        if (dist > 1e-8)
        {
            pullX = windCarryX / dist * pullGain;
            pullY = windCarryY / dist * pullGain;
        }

        windPatternRateA = std::clamp(windPatternRateA + noiseDist(windRng) * 0.004, 0.025, 0.280);
        windPatternRateB = std::clamp(windPatternRateB + noiseDist(windRng) * 0.004, 0.025, 0.280);

        const double stepTempo = 0.20 + 0.95 * normDist;
        windPatternPhaseA += windPatternRateA * stepTempo;
        windPatternPhaseB += windPatternRateB * stepTempo;
        if (windPatternPhaseA > twoPi) windPatternPhaseA = std::fmod(windPatternPhaseA, twoPi);
        if (windPatternPhaseB > twoPi) windPatternPhaseB = std::fmod(windPatternPhaseB, twoPi);

        const double oscAX = std::sin(windPatternPhaseA);
        const double oscBX = std::sin(windPatternPhaseB + 1.61803398875);
        const double oscAY = std::cos(windPatternPhaseA * 0.79 + 0.35);
        const double oscBY = std::cos(windPatternPhaseB * 1.17 - 0.48);

        const double patternAmp = baseW * (0.05 + 0.55 * normDist);
        const double patternTargetX = (oscAX + 0.58 * oscBX) * patternAmp;
        const double patternTargetY = (oscAY + 0.58 * oscBY) * patternAmp;
        const double patternBlend = 0.12 + 0.20 * normDist;
        windPatternX = windPatternX * (1.0 - patternBlend) + patternTargetX * patternBlend;
        windPatternY = windPatternY * (1.0 - patternBlend) + patternTargetY * patternBlend;

        windNoiseX = windNoiseX * 0.72 + noiseDist(windRng) * noiseAmp * 0.28;
        windNoiseY = windNoiseY * 0.72 + noiseDist(windRng) * noiseAmp * 0.28;

        const double windForceX = windNoiseX + windPatternX * 0.42;
        const double windForceY = windNoiseY + windPatternY * 0.42;

        const double drag = 0.82 + (1.0 - normDist) * 0.10;
        windVelX = windVelX * drag + pullX + windForceX;
        windVelY = windVelY * drag + pullY + windForceY;

        const double vCap = std::max(0.65, baseM * (0.30 + 0.70 * normDist));
        const double newVelMag = std::hypot(windVelX, windVelY);
        if (newVelMag > vCap)
        {
            const double clip = vCap * clipDist(windRng);
            windVelX = (windVelX / newVelMag) * clip;
            windVelY = (windVelY / newVelMag) * clip;
        }

        windFracX += windVelX;
        windFracY += windVelY;

        int stepX = static_cast<int>(std::clamp(std::round(windFracX), -32767.0, 32767.0));
        int stepY = static_cast<int>(std::clamp(std::round(windFracY), -32767.0, 32767.0));
        if (stepX == 0 && stepY == 0)
            continue;

        windFracX -= static_cast<double>(stepX);
        windFracY -= static_cast<double>(stepY);
        windCarryX -= static_cast<double>(stepX);
        windCarryY -= static_cast<double>(stepY);
        movement.steps.push_back({stepX, stepY});
    }

    const double carryCap = 120.0;
    const double finalCarryMag = std::hypot(windCarryX, windCarryY);
    if (finalCarryMag > carryCap)
    {
        const double s = carryCap / finalCarryMag;
        windCarryX *= s;
        windCarryY *= s;
    }
    return movement;
}

void MouseThread::resetWindState()
{
    graphWindStates.clear();
    resetWindScalars();
}

void MouseThread::resetWindScalars()
{
    constexpr double twoPi = 6.28318530717958647692;
    std::uniform_real_distribution<double> phaseDist(0.0, twoPi);
    std::uniform_real_distribution<double> rateDist(0.04, 0.16);

    windCarryX = 0.0;
    windCarryY = 0.0;
    windVelX = 0.0;
    windVelY = 0.0;
    windNoiseX = 0.0;
    windNoiseY = 0.0;
    windFracX = 0.0;
    windFracY = 0.0;
    windPatternX = 0.0;
    windPatternY = 0.0;
    windPatternPhaseA = phaseDist(windRng);
    windPatternPhaseB = phaseDist(windRng);
    windPatternRateA = rateDist(windRng);
    windPatternRateB = rateDist(windRng);
}

MouseThread::WindNodeState MouseThread::captureWindState() const
{
    WindNodeState state;
    state.carryX = windCarryX; state.carryY = windCarryY;
    state.velocityX = windVelX; state.velocityY = windVelY;
    state.noiseX = windNoiseX; state.noiseY = windNoiseY;
    state.fractionX = windFracX; state.fractionY = windFracY;
    state.patternX = windPatternX; state.patternY = windPatternY;
    state.phaseA = windPatternPhaseA; state.phaseB = windPatternPhaseB;
    state.rateA = windPatternRateA; state.rateB = windPatternRateB;
    state.rng = windRng;
    return state;
}

void MouseThread::restoreWindState(const WindNodeState& state)
{
    windCarryX = state.carryX; windCarryY = state.carryY;
    windVelX = state.velocityX; windVelY = state.velocityY;
    windNoiseX = state.noiseX; windNoiseY = state.noiseY;
    windFracX = state.fractionX; windFracY = state.fractionY;
    windPatternX = state.patternX; windPatternY = state.patternY;
    windPatternPhaseA = state.phaseA; windPatternPhaseB = state.phaseB;
    windPatternRateA = state.rateA; windPatternRateB = state.rateB;
    windRng = state.rng;
}

void MouseThread::appendWindDebugStep(int dx, int dy, double pixelScaleX, double pixelScaleY)
{
    if (dx == 0 && dy == 0)
        return;

    const auto delta = pixelScaleX!=0&&pixelScaleY!=0
        ?std::make_pair(dx*pixelScaleX,dy*pixelScaleY):mouseCountsToScreenPixels(dx,dy);
    double deltaPxX = delta.first;
    double deltaPxY = delta.second;
    if (std::abs(deltaPxX) < 1e-8 && std::abs(deltaPxY) < 1e-8)
        return;
    std::lock_guard<std::mutex> lock(windDebugTrailMutex);
    const auto now = std::chrono::steady_clock::now();
    pruneWindDebugTrailLocked(now);

    if (windDebugTrail.empty())
    {
        windDebugCursorX = center_x;
        windDebugCursorY = center_y;
        windDebugTrail.push_back({ windDebugCursorX, windDebugCursorY, now });
    }

    windDebugCursorX += deltaPxX;
    windDebugCursorY += deltaPxY;
    windDebugTrail.push_back({ windDebugCursorX, windDebugCursorY, now });

    constexpr size_t maxTrailPoints = 220;
    while (windDebugTrail.size() > maxTrailPoints)
        windDebugTrail.pop_front();
}

void MouseThread::pruneWindDebugTrailLocked(const std::chrono::steady_clock::time_point& now)
{
    constexpr auto windTrailLifetime = std::chrono::milliseconds(900);
    while (!windDebugTrail.empty() && (now - windDebugTrail.front().t) > windTrailLifetime)
        windDebugTrail.pop_front();
}

std::pair<double, double> MouseThread::mouseCountsToScreenPixels(int dx, int dy) const
{
    double deltaPxX = static_cast<double>(dx);
    double deltaPxY = static_cast<double>(dy);

    {
        std::lock_guard<std::mutex> cfgLock(configMutex);
        const Config::GameProfile* gpPtr = nullptr;

        auto activeIt = config.game_profiles.find(config.active_game);
        if (activeIt != config.game_profiles.end())
            gpPtr = &activeIt->second;
        else
        {
            auto unifiedIt = config.game_profiles.find("UNIFIED");
            if (unifiedIt != config.game_profiles.end())
                gpPtr = &unifiedIt->second;
        }

        if (gpPtr && gpPtr->sens != 0.0 && gpPtr->yaw != 0.0 && gpPtr->pitch != 0.0)
        {
            const double fovNow = std::max(1.0, fov_x);
            const double fovScale = (gpPtr->fovScaled && gpPtr->baseFOV > 1.0) ? (fovNow / gpPtr->baseFOV) : 1.0;
            const double degX = static_cast<double>(dx) * gpPtr->sens * gpPtr->yaw * fovScale;
            const double degY = static_cast<double>(dy) * gpPtr->sens * gpPtr->pitch * fovScale;

            const double degPerPxX = fov_x / std::max(1.0, screen_width);
            const double degPerPxY = fov_y / std::max(1.0, screen_height);

            if (std::abs(degPerPxX) > 1e-8 && std::abs(degPerPxY) > 1e-8)
            {
                deltaPxX = degX / degPerPxX;
                deltaPxY = degY / degPerPxY;
            }
        }
    }

    return { deltaPxX, deltaPxY };
}

void MouseThread::recordMotionCompensationStep(int dx, int dy, double pixelScaleX, double pixelScaleY)
{
    if (dx == 0 && dy == 0)
        return;

    const auto delta = (pixelScaleX != 0.0 || pixelScaleY != 0.0)
        ? std::make_pair(dx * pixelScaleX, dy * pixelScaleY)
        : mouseCountsToScreenPixels(dx, dy);
    if (std::abs(delta.first) < 1e-8 && std::abs(delta.second) < 1e-8)
        return;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(motionCompensationMutex);
    pruneMotionCompensationTrailLocked(now);
    motionCompensationTrail.push_back({ delta.first, delta.second, now });

    constexpr size_t maxSamples = 512;
    while (motionCompensationTrail.size() > maxSamples)
        motionCompensationTrail.pop_front();
}

void MouseThread::pruneMotionCompensationTrailLocked(const std::chrono::steady_clock::time_point& now)
{
    constexpr auto motionTrailLifetime = std::chrono::seconds(2);
    while (!motionCompensationTrail.empty() && (now - motionCompensationTrail.front().t) > motionTrailLifetime)
        motionCompensationTrail.pop_front();
}

std::pair<double, double> MouseThread::getMotionCompensationSince(
    std::chrono::steady_clock::time_point since) const
{
    if (since.time_since_epoch().count() == 0)
        return { 0.0, 0.0 };

    double x = 0.0;
    double y = 0.0;
    std::lock_guard<std::mutex> lock(motionCompensationMutex);
    for (const auto& sample : motionCompensationTrail)
    {
        if (sample.t >= since)
        {
            x += sample.x;
            y += sample.y;
        }
    }

    return { x, y };
}

void MouseThread::restorePredictionState(const PredictionNodeState& state)
{
    prev_x = state.x; prev_y = state.y;
    prev_velocity_x = state.velocityX; prev_velocity_y = state.velocityY;
    prev_time = state.time;
    targetKalman = state.kalman;
    lastKalmanTelemetry = state.telemetry;
    lastPredictionLookaheadSec = state.lookaheadSec;
    lastDetectionDelaySec = state.detectionDelaySec;
}

std::pair<double, double> MouseThread::graphPredict(int nodeKey, double pointX, double pointY,
    std::chrono::steady_clock::time_point observationTime, const NodeGraph::Node& node,
    double lookahead, bool reset)
{
    const auto invalid = std::make_pair(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(pointX) || !std::isfinite(pointY) || !std::isfinite(lookahead)) return invalid;
    auto found = graphPredictionStates.find(nodeKey);
    if (found == graphPredictionStates.end())
    {
        if (!hasGraphStateCapacity(graphPredictionStates,nodeKey)) return invalid;
        found = graphPredictionStates.emplace(nodeKey,PredictionNodeState{}).first;
    }
    auto& state = found->second;
    const auto now = std::chrono::steady_clock::now();
    if (observationTime.time_since_epoch().count()==0) observationTime=now;
    const double gap = state.time.time_since_epoch().count()==0 ? 0.0 :
        std::chrono::duration<double>(observationTime-state.time).count();
    const double timeout = NodeGraph::parameter(node,"kalman_reset_timeout_sec");
    aim::AimKalmanSettings settings;
    settings.enabled = NodeGraph::parameter(node,"kalman_enabled")!=0;
    settings.process_noise_position = NodeGraph::parameter(node,"kalman_process_noise_position");
    settings.process_noise_velocity = NodeGraph::parameter(node,"kalman_process_noise_velocity");
    settings.measurement_noise = NodeGraph::parameter(node,"kalman_measurement_noise");
    settings.velocity_damping = NodeGraph::parameter(node,"kalman_velocity_damping");
    settings.max_velocity = NodeGraph::parameter(node,"kalman_max_velocity");
    settings.warmup_frames = static_cast<int>(NodeGraph::parameter(node,"kalman_warmup_frames"));
    state.kalman.setSettings(settings);
    if (reset || gap<0 || gap>timeout) state.kalman.reset();
    const double dt = gap>0 ? std::clamp(gap,1.0/500.0,0.25) : currentFrameIntervalSec();
    const double delay = std::clamp(std::chrono::duration<double>(now-observationTime).count(),0.0,0.35);
    if (NodeGraph::parameter(node,"kalman_compensate_detection_delay")!=0) lookahead+=delay;
    lookahead+=NodeGraph::parameter(node,"kalman_additional_prediction_ms")*0.001;
    lookahead=std::clamp(lookahead,0.0,1.5);
    state.telemetry=state.kalman.update(pointX,pointY,dt,lookahead);
    state.time=observationTime;state.x=pointX;state.y=pointY;
    state.velocityX=state.telemetry.velocity_x;state.velocityY=state.telemetry.velocity_y;
    state.lookaheadSec=lookahead;state.detectionDelaySec=delay;
    // Existing diagnostics can display the most recent node without owning its state.
    restorePredictionState(state);
    return {state.telemetry.predicted_x,state.telemetry.predicted_y};
}

std::vector<std::pair<double,double>> MouseThread::graphFuturePositions(int nodeKey,int frames) const
{
    std::vector<std::pair<double,double>> positions;
    const auto found=graphPredictionStates.find(nodeKey);
    if(found==graphPredictionStates.end()||!found->second.kalman.initialized())return positions;
    frames=std::clamp(frames,0,300);positions.reserve(frames);
    const double dt=currentFrameIntervalSec();
    for(int i=1;i<=frames;++i) positions.push_back(found->second.kalman.predict(found->second.lookaheadSec+i*dt));
    return positions;
}

void MouseThread::retainGraphStates(const std::vector<int>& predictionKeys,const std::vector<int>& windKeys)
{
    for(auto it=graphPredictionStates.begin();it!=graphPredictionStates.end();)
        if(std::find(predictionKeys.begin(),predictionKeys.end(),it->first)==predictionKeys.end())it=graphPredictionStates.erase(it);else++it;
    for(auto it=graphWindStates.begin();it!=graphWindStates.end();)
        if(std::find(windKeys.begin(),windKeys.end(),it->first)==windKeys.end())it=graphWindStates.erase(it);else++it;
}

void MouseThread::sendMovementToDriver(int dx, int dy, std::uint64_t generation,
    double pixelScaleX, double pixelScaleY)
{
    if (dx == 0 && dy == 0)
    {
        return;
    }

    bool sent = false;
    detectionBuffer.withCurrentGeneration(generation, [&] {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);

        if (workerStop || editorInputCaptured.load() || !mouseInput)
            return;

        sent = mouseInput->move(dx, dy);
    });

    if (sent)
    {
        recordMotionCompensationStep(dx, dy, pixelScaleX, pixelScaleY);
        appendWindDebugStep(dx, dy, pixelScaleX, pixelScaleY);
    }
}

void MouseThread::moveRelative(int dx, int dy)
{
    sendMovementToDriver(dx, dy, detectionGeneration);
}

double MouseThread::graphSpeedCurve(double pointX,double pointY,const NodeGraph::Node& node)
{
    const double distance=std::hypot(pointX-center_x,pointY-center_y);
    const double minimum=NodeGraph::parameter(node,"min_speed"),maximum=NodeGraph::parameter(node,"max_speed");
    const double snap=NodeGraph::parameter(node,"snap_radius"),nearRadius=NodeGraph::parameter(node,"near_radius");
    if(!std::isfinite(distance))return std::numeric_limits<double>::quiet_NaN();
    if(distance<snap)return minimum*NodeGraph::parameter(node,"snap_boost");
    if(nearRadius>0&&distance<nearRadius)
    {
        const double curve=1-std::pow(1-distance/nearRadius,NodeGraph::parameter(node,"exponent"));
        return minimum+(maximum-minimum)*curve;
    }
    return minimum+(maximum-minimum)*std::clamp(distance/std::max(1.0,max_distance),0.0,1.0);
}

NodeGraph::Movement MouseThread::graphMouseDelta(double pointX,double pointY,double speed,
    NodeGraph::Point center,const NodeGraph::Node& node)
{
    NodeGraph::Movement movement;
    const double fovX=NodeGraph::parameter(node,"fov_x"),fovY=NodeGraph::parameter(node,"fov_y");
    const double sens=NodeGraph::parameter(node,"sens"),yaw=NodeGraph::parameter(node,"yaw"),pitch=NodeGraph::parameter(node,"pitch");
    const double baseFov=NodeGraph::parameter(node,"base_fov");
    const double scale=NodeGraph::parameter(node,"fov_scaled")!=0&&baseFov>1?fovX/baseFov:1.0;
    if(!std::isfinite(pointX)||!std::isfinite(pointY)||!std::isfinite(speed)||
        !std::isfinite(center.x)||!std::isfinite(center.y)||sens==0||yaw==0||pitch==0||fovX<=0||fovY<=0)return movement;
    double dx=(pointX-center.x)*(fovX/std::max(1.0,screen_width))/(sens*yaw*scale)*speed;
    double dy=(pointY-center.y)*(fovY/std::max(1.0,screen_height))/(sens*pitch*scale)*speed;
    if(NodeGraph::parameter(node,"legacy_fps")!=0 && captureFps.load()>30)
    {const double correction=30.0/captureFps.load();dx*=correction;dy*=correction;}
    if(!std::isfinite(dx)||!std::isfinite(dy))return movement;
    movement.pixelScaleX=sens*yaw*scale*screen_width/fovX;
    movement.pixelScaleY=sens*pitch*scale*screen_height/fovY;
    movement.preciseCounts={dx,dy};movement.hasPreciseCounts=true;
    const int x=static_cast<int>(std::clamp(dx,-32767.0,32767.0)),y=static_cast<int>(std::clamp(dy,-32767.0,32767.0));
    if(x||y)movement.steps.push_back({x,y});
    return movement;
}

NodeGraph::Movement MouseThread::graphWindMotion(const NodeGraph::Movement& movement)
{
    if (movement.steps.size() > 32)
        return {};
    for (const auto& step : movement.steps)
        if (step.dx < -32767 || step.dx > 32767 || step.dy < -32767 || step.dy > 32767)
            return {};
    if (!wind_mouse_enabled || movement.steps.empty())
        return movement;

    // A movement value describes the desired relative displacement. Wind
    // synthesizes a bounded path for that displacement without dispatching it.
    long long dx = 0, dy = 0;
    for (const auto& step : movement.steps)
    {
        dx += step.dx;
        dy += step.dy;
    }
    return buildWindSteps(
        static_cast<int>(std::clamp(dx, -32767LL, 32767LL)),
        static_cast<int>(std::clamp(dy, -32767LL, 32767LL)));
}

NodeGraph::Movement MouseThread::graphWindMotion(int nodeKey, const NodeGraph::Movement& movement)
{
    auto found = graphWindStates.find(nodeKey);
    if (found == graphWindStates.end())
    {
        if (!hasGraphStateCapacity(graphWindStates, nodeKey))
            return {};
        // The first node inherits the reset baseline, preserving the existing
        // default motion profile. Further nodes receive independent phases/RNG.
        if (!graphWindStates.empty())
            resetWindScalars();
        found = graphWindStates.emplace(nodeKey, captureWindState()).first;
    }
    restoreWindState(found->second);
    const auto result = graphWindMotion(movement);
    // Invalid Wind parameters may reset all state; do not retain an iterator
    // across synthesis, since that reset clears the node map.
    graphWindStates.insert_or_assign(nodeKey, captureWindState());
    return result;
}

NodeGraph::Movement MouseThread::graphWindMotion(int nodeKey,const NodeGraph::Movement& movement,
    const NodeGraph::Node& node)
{
    wind_mouse_enabled=NodeGraph::parameter(node,"enabled")!=0;
    wind_G=NodeGraph::parameter(node,"wind_G");wind_W=NodeGraph::parameter(node,"wind_W");
    wind_M=NodeGraph::parameter(node,"wind_M");wind_D=NodeGraph::parameter(node,"wind_D");
    auto resolved=graphWindMotion(nodeKey,movement);
    resolved.pixelScaleX=movement.pixelScaleX;resolved.pixelScaleY=movement.pixelScaleY;
    return resolved;
}

void MouseThread::dispatchGraphMovement(const NodeGraph::Movement& movement)
{
    // Graph evaluation already resolved coordinates, speed and optional wind.
    // Validate the complete batch before queueing any part of it.
    if (movement.steps.size() > 32)
        return;
    if(!std::isfinite(movement.pixelScaleX)||!std::isfinite(movement.pixelScaleY))return;
    for (const auto& step : movement.steps)
        if (step.dx < -32767 || step.dx > 32767 || step.dy < -32767 || step.dy > 32767)
            return;
    for (const auto& step : movement.steps)
        queueMove(step.dx, step.dy, movement.pixelScaleX, movement.pixelScaleY);
}

bool MouseThread::check_target_in_scope(double target_x, double target_y, double target_w, double target_h, double reduction_factor)
{
    double center_target_x = target_x + target_w / 2.0;
    double center_target_y = target_y + target_h / 2.0;

    double reduced_w = target_w * (reduction_factor / 2.0);
    double reduced_h = target_h * (reduction_factor / 2.0);

    double x1 = center_target_x - reduced_w;
    double x2 = center_target_x + reduced_w;
    double y1 = center_target_y - reduced_h;
    double y2 = center_target_y + reduced_h;

    return (center_x > x1 && center_x < x2 && center_y > y1 && center_y < y2);
}

void MouseThread::clearQueuedMoves()
{
    std::lock_guard<std::mutex> dispatchLock(dispatchMutex);
    std::lock_guard<std::mutex> lock(queueMtx);
    ++queueGeneration;
    while (!moveQueue.empty())
        moveQueue.pop();
    resetWindScalars();
}

void MouseThread::pressMouse()
{
    if (workerStop || editorInputCaptured.load())
        return;
    if (!mouse_pressed)
    {
        detectionBuffer.withCurrentGeneration(detectionGeneration, [&] {
            std::lock_guard<std::mutex> lock(inputDevicesMutex);
            if (!workerStop && !editorInputCaptured.load() && mouseInput && mouseInput->leftDown())
                mouse_pressed = true;
        });
    }
}

void MouseThread::releaseMouse()
{
    if (mouse_pressed)
    {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);
        if (!mouseInput)
        {
            mouse_pressed = false;
            return;
        }

        if (mouseInput->leftUp())
            mouse_pressed = false;
    }
}

void MouseThread::resetPrediction()
{
    graphPredictionStates.clear();
    graphWindStates.clear();
    clearQueuedMoves();
    prev_time = std::chrono::steady_clock::time_point();
    prev_x = 0;
    prev_y = 0;
    prev_velocity_x = 0;
    prev_velocity_y = 0;
    targetKalman.reset();
    lastKalmanTelemetry = {};
    lastPredictionLookaheadSec = 0.0;
    lastDetectionDelaySec = 0.0;
    target_detected.store(false);
}

void MouseThread::storeFuturePositions(const std::vector<std::pair<double, double>>& positions)
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex);
    futurePositions = positions;
}

void MouseThread::clearFuturePositions()
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex);
    futurePositions.clear();
}

std::vector<std::pair<double, double>> MouseThread::getFuturePositions()
{
    std::lock_guard<std::mutex> lock(futurePositionsMutex);
    return futurePositions;
}

void MouseThread::clearWindDebugTrail()
{
    std::lock_guard<std::mutex> lock(windDebugTrailMutex);
    windDebugTrail.clear();
    windDebugCursorX = center_x;
    windDebugCursorY = center_y;
}

std::vector<std::pair<double, double>> MouseThread::getWindDebugTrail()
{
    std::lock_guard<std::mutex> lock(windDebugTrailMutex);
    const auto now = std::chrono::steady_clock::now();
    pruneWindDebugTrailLocked(now);

    std::vector<std::pair<double, double>> out;
    out.reserve(windDebugTrail.size());
    for (const auto& p : windDebugTrail)
        out.emplace_back(p.x, p.y);
    return out;
}

void MouseThread::setMouseInput(IMouseInput* newMouseInput)
{
    clearQueuedMoves();
    std::lock_guard<std::mutex> lock(inputDevicesMutex);
    if (mouseInput != newMouseInput && mouse_pressed)
    {
        try
        {
            if (mouseInput)
                mouseInput->leftUp();
        }
        catch (...)
        {
            std::cerr << "[Mouse] Failed to release the old input device." << std::endl;
        }
        mouse_pressed = false;
    }
    mouseInput = newMouseInput;
}
