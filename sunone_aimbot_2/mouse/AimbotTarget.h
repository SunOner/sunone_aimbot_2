#ifndef AIMBOTTARGET_H
#define AIMBOTTARGET_H

#include <opencv2/opencv.hpp>
#include <vector>
#include <chrono>

class AimbotTarget
{
public:
    AimbotTarget();
    int x, y, w, h;
    int classId;

    double pivotX;
    double pivotY;

    AimbotTarget(int x, int y, int w, int h, int classId, double pivotX = 0.0, double pivotY = 0.0);
};

struct TargetSelectionSettings
{
    int classPlayer = 0;
    int classHead = 1;
    bool filterClasses = true;
    bool preferHead = true;
    bool mergeHeadBody = true;
    bool applyOffsets = true;
    double bodyOffset = 0.15;
    double headOffset = 0.05;
    int maxMissedFrames = 6;
};

AimbotTarget* sortTargets(
    const std::vector<cv::Rect>& boxes,
    const std::vector<int>& classes,
    int screenWidth,
    int screenHeight,
    bool disableHeadshot,
    const TargetSelectionSettings& settings
);

struct LockedTargetInfo
{
    int trackId = -1;
    bool observedThisFrame = false;
    int missedFrames = 0;
    double confidence = 0.0;
    double velocityX = 0.0;
    double velocityY = 0.0;
    AimbotTarget target;
};

struct TrackDebugInfo
{
    int trackId = -1;
    int classId = -1;
    cv::Rect box;
    double pivotX = 0.0;
    double pivotY = 0.0;
    double velocityX = 0.0;
    double velocityY = 0.0;
    std::chrono::steady_clock::time_point lastUpdate{};
    bool observedThisFrame = false;
    int missedFrames = 0;
    bool isLocked = false;
};

class MultiTargetTracker
{
public:
    void reset();
    void update(
        const std::vector<cv::Rect>& boxes,
        const std::vector<int>& classes,
        int screenWidth,
        int screenHeight,
        bool disableHeadshot,
        bool keepCurrentLock,
        const TargetSelectionSettings& settings,
        const std::vector<float>& confidences = {},
        std::chrono::steady_clock::time_point observationTime = {}
    );
    bool getLockedTarget(LockedTargetInfo& out) const;
    int getLockedTrackId() const { return lockedTrackId_; }
    std::vector<TrackDebugInfo> getDebugTracks() const;

private:
    struct TrackState
    {
        int id = -1;
        cv::Rect2f box;
        cv::Point2f velocity = { 0.0f, 0.0f };
        int classId = -1;
        double confidence = 0.0;
        int hits = 0;
        int missed = 0;
        bool observedThisFrame = false;
        double pivotX = 0.0;
        double pivotY = 0.0;
        std::chrono::steady_clock::time_point lastUpdate;
    };

    struct DetectionCandidate
    {
        cv::Rect2f box;
        int classId = -1;
        double confidence = 0.0;
        double pivotX = 0.0;
        double pivotY = 0.0;
    };

    static float iou(const cv::Rect2f& a, const cv::Rect2f& b);
    int findTrackIndexById(int id) const;
    int chooseBestTrack(int screenWidth, int screenHeight) const;
    int allowedMissedFrames(const TrackState& t) const;
    void pruneDeadTracks();

    std::vector<TrackState> tracks_;
    int nextId_ = 1;
    int lockedTrackId_ = -1;
    int maxMissedFrames_ = 6;
    double frameDtMeanSec_ = 1.0 / 60.0;
    std::chrono::steady_clock::time_point lastTrackerFrameTime_{};
};

#endif // AIMBOTTARGET_H
