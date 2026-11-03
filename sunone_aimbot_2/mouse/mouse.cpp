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
#include "detection_buffer.h"
#include "runtime/input_state.h"

namespace
{
double currentFrameIntervalSec()
{
    double fps = static_cast<double>(captureFps.load());
    if (fps <= 0.0) fps = 60.0;

    fps = std::clamp(fps, 15.0, 500.0);
    return 1.0 / fps;
}

template <typename States>
bool hasGraphStateCapacity(const States& states, int nodeKey)
{
    constexpr int graphStride = 1000001;
    if (nodeKey <= 0 || nodeKey >= graphStride * static_cast<int>(NodeGraph::MaxGraphs))
        return false;
    const int graph = nodeKey / graphStride;
    std::size_t count = 0;
    for (const auto& entry : states)
        if (entry.first / graphStride == graph)
            ++count;
    return count < NodeGraph::MaxNodes;
}
}

MouseThread::MouseThread(int resolution,IMouseInput* mouseInputDevice)
    :screen_width(std::max(1,resolution)),screen_height(std::max(1,resolution)),
     center_x(screen_width*0.5),center_y(screen_height*0.5),mouseInput(mouseInputDevice)
{
    wind_mouse_enabled=true;wind_G=9;wind_W=3;wind_M=10;wind_D=12;
    resetWindState();clearWindDebugTrail();
    detectionGeneration=detectionBuffer.currentGeneration();
    moveWorker=std::thread(&MouseThread::moveWorkerLoop,this);
}
void MouseThread::setCaptureResolution(int resolution)
{
    resetPrediction();
    screen_width=screen_height=std::max(1,resolution);center_x=screen_width*0.5;center_y=screen_height*0.5;
    clearWindDebugTrail();
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

    const auto delta=std::make_pair(dx*pixelScaleX,dy*pixelScaleY);
    if(!std::isfinite(delta.first)||!std::isfinite(delta.second))return;
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

void MouseThread::recordMotionCompensationStep(int dx, int dy, double pixelScaleX, double pixelScaleY)
{
    if (dx == 0 && dy == 0)
        return;

    const auto delta=std::make_pair(dx*pixelScaleX,dy*pixelScaleY);
    if(!std::isfinite(delta.first)||!std::isfinite(delta.second))return;
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

std::pair<double, double> MouseThread::graphPredict(int nodeKey, double pointX, double pointY,
    std::chrono::steady_clock::time_point observationTime, const NodeGraph::Node& node,
    double lookahead, bool reset)
{
    const auto invalid = std::make_pair(std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(pointX) || !std::isfinite(pointY) || !std::isfinite(lookahead)) return invalid;
    const auto motion=graphEstimateMotion(nodeKey,{pointX,pointY},observationTime,node,reset);
    if(observationTime.time_since_epoch().count()==0)observationTime=std::chrono::steady_clock::now();
    const double delay=std::clamp(std::chrono::duration<double>(std::chrono::steady_clock::now()-observationTime).count(),0.0,0.35);
    if(NodeGraph::parameter(node,"kalman_compensate_detection_delay")!=0)lookahead+=delay;
    lookahead=std::clamp(lookahead+NodeGraph::parameter(node,"kalman_additional_prediction_ms")*0.001,0.0,1.5);
    const auto state=graphPredictionStates.find(nodeKey);if(state!=graphPredictionStates.end())state->second.lookaheadSec=lookahead;
    const auto result=NodeGraph::projectPoint(motion.position,motion.velocity,lookahead,motion.damping,motion.ready);
    return {result.x,result.y};
}
NodeGraph::MotionData MouseThread::graphEstimateMotion(int nodeKey,NodeGraph::Point point,
    std::chrono::steady_clock::time_point observationTime,const NodeGraph::Node& node,bool reset)
{
    NodeGraph::MotionData invalid;invalid.damping=std::numeric_limits<double>::quiet_NaN();
    if(!std::isfinite(point.x)||!std::isfinite(point.y))return invalid;
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
    const auto telemetry=state.kalman.update(point.x,point.y,dt,0);
    state.time=observationTime;state.lookaheadSec=0;
    return {{telemetry.estimate_x,telemetry.estimate_y},{telemetry.velocity_x,telemetry.velocity_y},
        state.kalman.settings().velocity_damping,telemetry.prediction_ready,telemetry.warmup_remaining==0};
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

void MouseThread::dispatchGraphRecoil(int counts,double pixelsPerCount)
{
    if(!std::isfinite(pixelsPerCount))return;
    sendMovementToDriver(0,counts,detectionGeneration,0,pixelsPerCount);
}

double MouseThread::graphSpeedCurve(double pointX,double pointY,NodeGraph::Point center,double maximumDistance,const NodeGraph::Node& node)
{
    const double distance=std::hypot(pointX-center.x,pointY-center.y);
    const double minimum=NodeGraph::parameter(node,"min_speed"),maximum=NodeGraph::parameter(node,"max_speed");
    const double snap=NodeGraph::parameter(node,"snap_radius"),nearRadius=NodeGraph::parameter(node,"near_radius");
    if(!std::isfinite(distance))return std::numeric_limits<double>::quiet_NaN();
    if(distance<snap)return minimum*NodeGraph::parameter(node,"snap_boost");
    if(nearRadius>0&&distance<nearRadius)
    {
        const double curve=1-std::pow(1-distance/nearRadius,NodeGraph::parameter(node,"exponent"));
        return minimum+(maximum-minimum)*curve;
    }
    return minimum+(maximum-minimum)*std::clamp(distance/std::max(1.0,maximumDistance),0.0,1.0);
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
    const auto calibration=NodeGraph::mousePixelCalibration(node,{static_cast<double>(screen_width),static_cast<double>(screen_height)});
    movement.pixelScaleX=calibration.x;movement.pixelScaleY=calibration.y;
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
