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

#include "mouse.h"
#include "capture.h"
#include "config.h"
#include "detection_buffer.h"
#include "runtime/input_state.h"

namespace
{
aim::AimKalmanSettings buildKalmanSettings(const Config& snapshot)
{
    aim::AimKalmanSettings settings;
    settings.enabled = snapshot.kalman_enabled;
    settings.process_noise_position = static_cast<double>(snapshot.kalman_process_noise_position);
    settings.process_noise_velocity = static_cast<double>(snapshot.kalman_process_noise_velocity);
    settings.measurement_noise = static_cast<double>(snapshot.kalman_measurement_noise);
    settings.velocity_damping = static_cast<double>(snapshot.kalman_velocity_damping);
    settings.max_velocity = static_cast<double>(snapshot.kalman_max_velocity);
    settings.warmup_frames = snapshot.kalman_warmup_frames;
    return settings;
}

aim::AimKalmanSettings buildKalmanSettingsFromConfig()
{
    std::lock_guard<std::mutex> lock(configMutex);
    return buildKalmanSettings(config);
}

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
}

MouseThread::MouseThread(
    int resolution,
    int fovX,
    int fovY,
    double minSpeedMultiplier,
    double maxSpeedMultiplier,
    double predictionInterval,
    bool auto_shoot,
    float bScope_multiplier,
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
    auto_shoot(auto_shoot),
    bScope_multiplier(bScope_multiplier),
    mouseInput(mouseInputDevice),

    prev_velocity_x(0.0),
    prev_velocity_y(0.0),
    prev_x(0.0),
    prev_y(0.0)
{
    prev_time = std::chrono::steady_clock::time_point();
    last_target_time = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(configMutex);
        wind_mouse_enabled = config.wind_mouse_enabled;
        wind_G = config.wind_G;
        wind_W = config.wind_W;
        wind_M = config.wind_M;
        wind_D = config.wind_D;
    }
    resetWindState();
    clearWindDebugTrail();
    targetKalman.setSettings(buildKalmanSettingsFromConfig());
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
    if (!snapshot->auto_shoot)
        releaseMouse();

    const int resolution = snapshot->detection_resolution;
    screen_width = screen_height = resolution;
    fov_x = snapshot->fovX;
    fov_y = snapshot->fovY;
    min_speed_multiplier = snapshot->minSpeedMultiplier;
    max_speed_multiplier = snapshot->maxSpeedMultiplier;
    prediction_interval = snapshot->predictionInterval;
    auto_shoot = snapshot->auto_shoot;
    bScope_multiplier = snapshot->bScope_multiplier;
    center_x = center_y = resolution / 2.0;
    max_distance = std::hypot(resolution, resolution) / 2.0;
    wind_mouse_enabled = snapshot->wind_mouse_enabled;
    wind_G = snapshot->wind_G;
    wind_W = snapshot->wind_W;
    wind_M = snapshot->wind_M;
    wind_D = snapshot->wind_D;
    clearWindDebugTrail();
    targetKalman.setSettings(buildKalmanSettings(*snapshot));
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

void MouseThread::queueMove(int dx, int dy)
{
    if (dx == 0 && dy == 0)
    {
        return;
    }

    std::lock_guard lg(queueMtx);
    if (workerStop)
        return;
    if (moveQueue.size() >= queueLimit) moveQueue.pop();
    moveQueue.push({ dx, dy, detectionGeneration, queueGeneration });
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
            sendMovementToDriver(m.dx, m.dy, m.detectionGeneration);
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

void MouseThread::windMouseMoveRelative(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    windCarryX += static_cast<double>(dx);
    windCarryY += static_cast<double>(dy);

    const double baseG = std::clamp(wind_G, 0.05, 50.0);
    const double baseW = std::clamp(wind_W, 0.0, 80.0);
    const double baseM = std::max(1.0, wind_M);
    const double baseD = std::max(1.0, wind_D);

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

        int stepX = static_cast<int>(std::round(windFracX));
        int stepY = static_cast<int>(std::round(windFracY));
        if (stepX == 0 && stepY == 0)
            continue;

        windFracX -= static_cast<double>(stepX);
        windFracY -= static_cast<double>(stepY);
        windCarryX -= static_cast<double>(stepX);
        windCarryY -= static_cast<double>(stepY);
        queueMove(stepX, stepY);
    }

    const double carryCap = 120.0;
    const double finalCarryMag = std::hypot(windCarryX, windCarryY);
    if (finalCarryMag > carryCap)
    {
        const double s = carryCap / finalCarryMag;
        windCarryX *= s;
        windCarryY *= s;
    }
}

void MouseThread::resetWindState()
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

void MouseThread::appendWindDebugStep(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    auto delta = mouseCountsToScreenPixels(dx, dy);
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

void MouseThread::recordMotionCompensationStep(int dx, int dy)
{
    if (dx == 0 && dy == 0)
        return;

    const auto delta = mouseCountsToScreenPixels(dx, dy);
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

double MouseThread::currentDetectionDelaySec(double observationAgeSec) const
{
    double detectionDelaySec = 0.05;
    if (std::isfinite(observationAgeSec) && observationAgeSec >= 0.0)
    {
        detectionDelaySec = observationAgeSec;
    }
    else
    {
        // Last measured observation age belongs to this mouse loop. Reading
        // mutable timing fields owned by an inference thread would race.
        if (lastDetectionDelaySec > 0.0)
            detectionDelaySec = lastDetectionDelaySec;
    }
    if (!std::isfinite(detectionDelaySec))
        detectionDelaySec = 0.05;
    return std::clamp(detectionDelaySec, 0.0, 0.35);
}

double MouseThread::currentPredictionLookaheadSec(double detectionDelaySec) const
{
    double lookahead = std::max(0.0, prediction_interval);
    bool compensateDetectionDelay = false;
    float additionalPredictionMs = 0.0f;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        compensateDetectionDelay = config.kalman_compensate_detection_delay;
        additionalPredictionMs = config.kalman_additional_prediction_ms;
    }

    if (compensateDetectionDelay)
        lookahead += std::max(0.0, detectionDelaySec);
    lookahead += static_cast<double>(additionalPredictionMs) * 0.001;
    return std::clamp(lookahead, 0.0, 1.5);
}

std::pair<double, double> MouseThread::predict_target_position(
    double target_x,
    double target_y,
    std::chrono::steady_clock::time_point observationTime)
{
    auto current_time = std::chrono::steady_clock::now();
    if (observationTime.time_since_epoch().count() == 0)
        observationTime = current_time;

    double observationAgeSec = std::chrono::duration<double>(current_time - observationTime).count();
    if (!std::isfinite(observationAgeSec) || observationAgeSec < 0.0)
        observationAgeSec = 0.0;

    targetKalman.setSettings(buildKalmanSettingsFromConfig());

    if (prev_time.time_since_epoch().count() == 0 || !target_detected.load())
    {
        prev_time = observationTime;
        prev_x = target_x;
        prev_y = target_y;
        prev_velocity_x = 0.0;
        prev_velocity_y = 0.0;
        targetKalman.reset();
        const double detectionDelaySec = currentDetectionDelaySec(observationAgeSec);
        const double lookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
        lastKalmanTelemetry = targetKalman.update(target_x, target_y, currentFrameIntervalSec(), lookaheadSec);
        lastDetectionDelaySec = detectionDelaySec;
        lastPredictionLookaheadSec = lookaheadSec;
        return { target_x, target_y };
    }

    double dt = std::chrono::duration<double>(observationTime - prev_time).count();
    if (!std::isfinite(dt) || dt <= 0.0)
        dt = currentFrameIntervalSec();
    dt = std::clamp(dt, 1.0 / 500.0, 0.25);

    prev_time = observationTime;
    prev_x = target_x;
    prev_y = target_y;

    const double detectionDelaySec = currentDetectionDelaySec(observationAgeSec);
    const double lookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
    lastDetectionDelaySec = detectionDelaySec;
    lastPredictionLookaheadSec = lookaheadSec;

    lastKalmanTelemetry = targetKalman.update(target_x, target_y, dt, lookaheadSec);
    prev_velocity_x = lastKalmanTelemetry.velocity_x;
    prev_velocity_y = lastKalmanTelemetry.velocity_y;

    double predictedX = lastKalmanTelemetry.predicted_x;
    double predictedY = lastKalmanTelemetry.predicted_y;
    if (!std::isfinite(predictedX)) predictedX = target_x;
    if (!std::isfinite(predictedY)) predictedY = target_y;

    return { predictedX, predictedY };
}

void MouseThread::sendMovementToDriver(int dx, int dy, std::uint64_t generation)
{
    if (dx == 0 && dy == 0)
    {
        return;
    }

    bool sent = false;
    detectionBuffer.withCurrentGeneration(generation, [&] {
        std::lock_guard<std::mutex> lock(inputDevicesMutex);

        if (workerStop || !mouseInput)
            return;

        sent = mouseInput->move(dx, dy);
    });

    if (sent)
    {
        recordMotionCompensationStep(dx, dy);
        appendWindDebugStep(dx, dy);
    }
}

void MouseThread::moveRelative(int dx, int dy)
{
    sendMovementToDriver(dx, dy, detectionGeneration);
}

std::pair<double, double> MouseThread::calc_movement(double tx, double ty)
{
    double offx = tx - center_x;
    double offy = ty - center_y;
    double dist = std::hypot(offx, offy);
    double speed = calculate_speed_multiplier(dist);

    double degPerPxX = fov_x / screen_width;
    double degPerPxY = fov_y / screen_height;

    double mmx = offx * degPerPxX;
    double mmy = offy * degPerPxY;

    double corr = 1.0;
    double fps = static_cast<double>(captureFps.load());
    if (fps > 30.0) corr = 30.0 / fps;

    std::pair<double, double> counts_pair;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        counts_pair = config.degToCounts(mmx, mmy, fov_x);
    }
    double move_x = counts_pair.first * speed * corr;
    double move_y = counts_pair.second * speed * corr;

    return { move_x, move_y };
}

double MouseThread::calculate_speed_multiplier(double distance)
{
    float snapRadius = 0.0f;
    float nearRadius = 0.0f;
    float speedCurveExponent = 1.0f;
    float snapBoostFactor = 1.0f;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        snapRadius = config.snapRadius;
        nearRadius = config.nearRadius;
        speedCurveExponent = config.speedCurveExponent;
        snapBoostFactor = config.snapBoostFactor;
    }

    if (distance < snapRadius)
        return min_speed_multiplier * snapBoostFactor;

    if (nearRadius > 0.0f && distance < nearRadius)
    {
        double t = distance / nearRadius;
        double curve = 1.0 - std::pow(1.0 - t, speedCurveExponent);
        return min_speed_multiplier +
            (max_speed_multiplier - min_speed_multiplier) * curve;
    }

    double norm = std::clamp(distance / max_distance, 0.0, 1.0);
    return min_speed_multiplier +
        (max_speed_multiplier - min_speed_multiplier) * norm;
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

void MouseThread::moveMouse(const AimbotTarget& target)
{
    std::lock_guard lg(input_method_mutex);

    auto predicted = predict_target_position(
        target.x + target.w / 2.0,
        target.y + target.h / 2.0);

    auto mv = calc_movement(predicted.first, predicted.second);
    queueMove(static_cast<int>(mv.first), static_cast<int>(mv.second));
}

void MouseThread::moveMousePivot(
    double pivotX,
    double pivotY,
    std::chrono::steady_clock::time_point observationTime)
{
    std::lock_guard lg(input_method_mutex);
    if (observationTime.time_since_epoch().count() != 0)
    {
        auto cameraDelta = getMotionCompensationSince(observationTime);
        pivotX -= cameraDelta.first;
        pivotY -= cameraDelta.second;
    }

    auto predicted = predict_target_position(pivotX, pivotY, observationTime);
    auto mv = calc_movement(predicted.first, predicted.second);
    int mx = static_cast<int>(mv.first);
    int my = static_cast<int>(mv.second);

    if (mx == 0 && my == 0)
    {
        return;
    }

    if (wind_mouse_enabled)
    {
        windMouseMoveRelative(mx, my);
    }
    else
    {
        queueMove(mx, my);
    }
}

void MouseThread::clearQueuedMoves()
{
    std::lock_guard<std::mutex> dispatchLock(dispatchMutex);
    std::lock_guard<std::mutex> lock(queueMtx);
    ++queueGeneration;
    while (!moveQueue.empty())
        moveQueue.pop();
    resetWindState();
}

void MouseThread::pressMouse(const AimbotTarget& target)
{
    if (!auto_shoot || workerStop)
        return;
    bool bScope = check_target_in_scope(target.x, target.y, target.w, target.h, bScope_multiplier);
    if (bScope && !mouse_pressed)
    {
        detectionBuffer.withCurrentGeneration(detectionGeneration, [&] {
            std::lock_guard<std::mutex> lock(inputDevicesMutex);
            if (!workerStop && mouseInput && mouseInput->leftDown())
                mouse_pressed = true;
        });
    }
    else if (!bScope && mouse_pressed)
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

void MouseThread::checkAndResetPredictions()
{
    auto current_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(current_time - last_target_time).count();
    double resetTimeoutSec = 0.5;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        resetTimeoutSec = static_cast<double>(config.kalman_reset_timeout_sec);
    }
    const double timeoutSec = std::clamp(resetTimeoutSec, 0.05, 3.0);

    if (elapsed > timeoutSec && target_detected.load())
    {
        resetPrediction();
    }
}

std::vector<std::pair<double, double>> MouseThread::predictFuturePositions(double pivotX, double pivotY, int frames)
{
    std::vector<std::pair<double, double>> result;
    if (frames <= 0)
        return result;

    result.reserve(frames);

    const double frame_time = currentFrameIntervalSec();

    targetKalman.setSettings(buildKalmanSettingsFromConfig());
    if (targetKalman.initialized())
    {
        const double detectionDelaySec = (lastDetectionDelaySec > 0.0)
            ? lastDetectionDelaySec
            : currentDetectionDelaySec();
        const double baseLookaheadSec = currentPredictionLookaheadSec(detectionDelaySec);
        for (int i = 1; i <= frames; ++i)
        {
            const double t = baseLookaheadSec + frame_time * i;
            auto predicted = targetKalman.predict(t);
            if (!std::isfinite(predicted.first) || !std::isfinite(predicted.second))
                continue;
            result.push_back(predicted);
        }

        if (!result.empty())
            return result;
    }

    auto current_time = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(current_time - prev_time).count();

    if (prev_time.time_since_epoch().count() == 0 || dt > 0.5)
    {
        return result;
    }

    double vx = prev_velocity_x;
    double vy = prev_velocity_y;
    
    for (int i = 1; i <= frames; i++)
    {
        double t = frame_time * i;
        double px = pivotX + vx * t;
        double py = pivotY + vy * t;
        result.push_back({ px, py });
    }

    return result;
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
