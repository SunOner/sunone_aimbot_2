#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "capture.h"
#include "mouse.h"
#include "config.h"
#include "detection_buffer.h"
#include "runtime/input_state.h"
#include "runtime/thread_loops.h"

namespace
{
constexpr int kPredictedOnlyMoveGraceFrames = 3;
constexpr double kPredictedOnlyMoveGraceSec =
    static_cast<double>(kPredictedOnlyMoveGraceFrames) / 60.0;
constexpr int kPredictedOnlyMoveStalePadMs = 16;

double trackerFrameIntervalSec(int captureFpsValue)
{
    const double fps = std::clamp(
        static_cast<double>((captureFpsValue > 0) ? captureFpsValue : 60),
        15.0,
        500.0);
    return 1.0 / fps;
}

bool allowPredictedOnlyMove(
    int activeTrackId,
    bool hasActiveTarget,
    const LockedTargetInfo& lockInfo,
    int captureFpsValue)
{
    if (activeTrackId != lockInfo.trackId ||
        !hasActiveTarget ||
        lockInfo.missedFrames <= 0)
    {
        return false;
    }

    const double frameDtSec = trackerFrameIntervalSec(captureFpsValue);
    const double missedSec = static_cast<double>(lockInfo.missedFrames) * frameDtSec;
    return missedSec <= kPredictedOnlyMoveGraceSec + frameDtSec * 0.51;
}

int trackerStaleTimeoutMs(int captureFpsValue)
{
    const int fps = std::max(1, captureFpsValue);
    const int frameBasedMs = 2000 / fps;
    const int graceBasedMs =
        static_cast<int>(kPredictedOnlyMoveGraceSec * 1000.0 + 0.5) +
        kPredictedOnlyMoveStalePadMs;
    return std::clamp(std::max(frameBasedMs, graceBasedMs), 25, 180);
}
}

void createInputDevices();
void assignInputDevices();
void handleEasyNoRecoil(MouseThread& mouseThread)
{
    bool easyNoRecoil = false;
    int recoil_compensation = 0;
    {
        std::lock_guard<std::mutex> cfgLock(configMutex);
        easyNoRecoil = config.easynorecoil;
        recoil_compensation = static_cast<int>(config.easynorecoilstrength);
    }

    if (easyNoRecoil && shooting.load() && zooming.load())
    {
        mouseThread.moveRelative(0, recoil_compensation);
    }
}

void mouseThreadFunction(MouseThread& mouseThread)
{
    struct StopOnExit
    {
        MouseThread& mouse;
        ~StopOnExit() { mouse.stop(); }
    } stopOnExit{ mouseThread };

    int lastVersion = -1;
    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    std::chrono::steady_clock::time_point detectionTimestamp{};
    MultiTargetTracker targetTracker;
    std::optional<AimbotTarget> activeTarget;
    int activeTrackId = -1;
    bool activeTargetObserved = false;
    bool wasAiming = false;
    int appliedDetectionResolution = -1;
    bool appliedTrackerEnabled = true;
    std::uint64_t controlGeneration = detectionBuffer.currentGeneration();
    auto lastTrackerUpdate = std::chrono::steady_clock::time_point::min();

    auto resetActiveTarget = [&]() {
        activeTarget.reset();
        activeTrackId = -1;
        activeTargetObserved = false;
        mouseThread.clearFuturePositions();
        mouseThread.resetPrediction();
    };

    while (!shouldExit)
    {
        if (mouseThread.applyPendingConfig())
            resetActiveTarget();

        bool hasNewDetection = false;
        bool hasAimObservation = false;
        int detectionResolution = 0;
        bool disableHeadshot = false;
        bool trackerEnabled = true;
        int predictionFuturePositions = 0;
        bool autoShoot = false;
        bool pausedNow = false;
        std::uint64_t observationGeneration = controlGeneration;

        {
            std::lock_guard<std::mutex> cfgLock(configMutex);
            detectionResolution = config.detection_resolution;
            disableHeadshot = config.disable_headshot;
            trackerEnabled = config.tracker_enabled;
            predictionFuturePositions = config.prediction_futurePositions;
            autoShoot = config.auto_shoot;
        }

        const bool aimingNow = aiming.load();
        if (aimingNow != wasAiming)
        {
            resetActiveTarget();
            wasAiming = aimingNow;
        }

        {
            std::unique_lock<std::mutex> lock(detectionBuffer.mutex);
            detectionBuffer.cv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return detectionBuffer.version > lastVersion || shouldExit;
                }
            );

            if (shouldExit) break;

            pausedNow = detectionBuffer.paused;
            observationGeneration = detectionBuffer.generation;

            if (detectionBuffer.version > lastVersion)
            {
                boxes = detectionBuffer.boxes;
                classes = detectionBuffer.classes;
                detectionTimestamp = detectionBuffer.frameTimestamp;
                lastVersion = detectionBuffer.version;
                hasNewDetection = true;
            }
        }

        mouseThread.setDetectionGeneration(observationGeneration);
        if (observationGeneration != controlGeneration)
        {
            controlGeneration = observationGeneration;
            targetTracker.reset();
            resetActiveTarget();
            mouseThread.releaseMouse();
            std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
            g_trackerDebugTracks.clear();
            g_trackerLockedId = -1;
        }

        if (input_method_changed.exchange(false))
        {
            createInputDevices();
            assignInputDevices();
        }

        if (detection_resolution_changed.load() || detectionResolution != appliedDetectionResolution)
        {
            {
                std::lock_guard<std::mutex> cfgLock(configMutex);
                appliedDetectionResolution = config.detection_resolution;
                mouseThread.updateConfig(config);
            }
            mouseThread.applyPendingConfig();
            targetTracker.reset();
            {
                std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                g_trackerDebugTracks.clear();
                g_trackerLockedId = -1;
            }
            resetActiveTarget();
        }

        if (pausedNow)
        {
            mouseThread.releaseMouse();
            continue;
        }

        if (trackerEnabled != appliedTrackerEnabled)
        {
            appliedTrackerEnabled = trackerEnabled;
            targetTracker.reset();
            {
                std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                g_trackerDebugTracks.clear();
                g_trackerLockedId = -1;
            }
            resetActiveTarget();
        }

        if (hasNewDetection)
        {
            if (trackerEnabled)
            {
                targetTracker.update(
                    boxes,
                    classes,
                    detectionResolution,
                    detectionResolution,
                    disableHeadshot,
                    aimingNow,
                    detectionTimestamp
                );
                lastTrackerUpdate = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                    g_trackerDebugTracks = targetTracker.getDebugTracks();
                    g_trackerLockedId = targetTracker.getLockedTrackId();
                }

                LockedTargetInfo lockInfo;
                if (targetTracker.getLockedTarget(lockInfo))
                {
                    const int previousActiveTrackId = activeTrackId;
                    const bool hadActiveTarget = activeTarget.has_value();
                    if (activeTrackId != -1 && activeTrackId != lockInfo.trackId)
                    {
                        mouseThread.resetPrediction();
                        mouseThread.clearFuturePositions();
                    }

                    activeTarget = lockInfo.target;
                    activeTrackId = lockInfo.trackId;
                    activeTargetObserved = lockInfo.observedThisFrame;
                    mouseThread.setTargetDetected(true);

                    if (lockInfo.observedThisFrame)
                    {
                        hasAimObservation = true;
                        mouseThread.setLastTargetTime(std::chrono::steady_clock::now());

                        auto futurePositions = mouseThread.predictFuturePositions(
                            activeTarget->pivotX,
                            activeTarget->pivotY,
                            predictionFuturePositions
                        );
                        mouseThread.storeFuturePositions(futurePositions);
                    }
                    else if (allowPredictedOnlyMove(
                        previousActiveTrackId,
                        hadActiveTarget,
                        lockInfo,
                        captureFps.load()))
                    {
                        hasAimObservation = true;

                        auto futurePositions = mouseThread.predictFuturePositions(
                            activeTarget->pivotX,
                            activeTarget->pivotY,
                            predictionFuturePositions
                        );
                        mouseThread.storeFuturePositions(futurePositions);
                    }
                }
                else
                {
                    resetActiveTarget();
                }
            }
            else
            {
                targetTracker.reset();
                {
                    std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
                    g_trackerDebugTracks.clear();
                    g_trackerLockedId = -1;
                }

                std::unique_ptr<AimbotTarget> selected(
                    sortTargets(
                        boxes,
                        classes,
                        detectionResolution,
                        detectionResolution,
                        disableHeadshot));
                lastTrackerUpdate = std::chrono::steady_clock::now();

                if (selected)
                {
                    activeTarget = *selected;
                    activeTrackId = -1;
                    activeTargetObserved = true;
                    hasAimObservation = true;
                    mouseThread.setTargetDetected(true);
                    mouseThread.setLastTargetTime(std::chrono::steady_clock::now());

                    auto futurePositions = mouseThread.predictFuturePositions(
                        activeTarget->pivotX,
                        activeTarget->pivotY,
                        predictionFuturePositions
                    );
                    mouseThread.storeFuturePositions(futurePositions);
                }
                else
                {
                    resetActiveTarget();
                }
            }
        }

        if (activeTarget)
        {
            const int staleMs = trackerStaleTimeoutMs(captureFps.load());
            if (std::chrono::steady_clock::now() - lastTrackerUpdate > std::chrono::milliseconds(staleMs))
            {
                resetActiveTarget();
            }
        }

        if (aimingNow)
        {
            if (activeTarget && hasAimObservation)
            {
                mouseThread.moveMousePivot(activeTarget->pivotX, activeTarget->pivotY, detectionTimestamp);
            }
            else
            {
                if (!activeTarget || !activeTargetObserved)
                {
                    mouseThread.clearQueuedMoves();
                }
            }

            // Keep our press across polling timeouts while the observed target
            // remains valid. Disabling auto-shoot always releases our button.
            if (autoShoot && activeTarget && activeTargetObserved)
                mouseThread.pressMouse(*activeTarget);
            else
                mouseThread.releaseMouse();
        }
        else
        {
            mouseThread.clearQueuedMoves();
            mouseThread.releaseMouse();
        }

        handleEasyNoRecoil(mouseThread);

        mouseThread.checkAndResetPredictions();
    }
}


