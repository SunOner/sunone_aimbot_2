#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <cmath>
#include <limits>
#include <algorithm>
#include <numeric>
#include <mutex>
#include <opencv2/opencv.hpp>

#include "runtime/input_state.h"
#include "AimbotTarget.h"
#include "config.h"

AimbotTarget::AimbotTarget()
    : x(0), y(0), w(0), h(0), classId(0), pivotX(0.0), pivotY(0.0)
{
}

AimbotTarget::AimbotTarget(int x_, int y_, int w_, int h_, int cls, double px, double py)
    : x(x_), y(y_), w(w_), h(h_), classId(cls), pivotX(px), pivotY(py)
{
}

AimbotTarget* sortTargets(
    const std::vector<cv::Rect>& boxes,
    const std::vector<int>& classes,
    int screenWidth,
    int screenHeight,
    bool disableHeadshot)
{
    const size_t inputCount = std::min(boxes.size(), classes.size());
    if (inputCount == 0)
    {
        return nullptr;
    }

    cv::Point center(screenWidth / 2, screenHeight / 2);
    int classHead = 1;
    int classPlayer = 0;
    float headOffset = 0.05f;
    float bodyOffset = 0.15f;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        classHead = config.class_head;
        classPlayer = config.class_player;
        headOffset = config.head_y_offset;
        bodyOffset = config.body_y_offset;
    }

    double minDistance = std::numeric_limits<double>::max();
    int nearestIdx = -1;
    int targetY = 0;

    if (!disableHeadshot)
    {
        for (size_t i = 0; i < inputCount; i++)
        {
            if (boxes[i].width <= 0 || boxes[i].height <= 0)
                continue;

            if (classes[i] == classHead)
            {
                int headOffsetY = static_cast<int>(boxes[i].height * headOffset);
                cv::Point targetPoint(boxes[i].x + boxes[i].width / 2, boxes[i].y + headOffsetY);
                double distance = std::pow(targetPoint.x - center.x, 2) + std::pow(targetPoint.y - center.y, 2);
                if (distance < minDistance)
                {
                    minDistance = distance;
                    nearestIdx = static_cast<int>(i);
                    targetY = targetPoint.y;
                }
            }
        }
    }

    if (disableHeadshot || nearestIdx == -1)
    {
        minDistance = std::numeric_limits<double>::max();
        for (size_t i = 0; i < inputCount; i++)
        {
            if (boxes[i].width <= 0 || boxes[i].height <= 0)
                continue;

            if (disableHeadshot && classes[i] == classHead)
                continue;

            if (classes[i] == classPlayer)
            {
                int offsetY = static_cast<int>(boxes[i].height * bodyOffset);
                cv::Point targetPoint(boxes[i].x + boxes[i].width / 2, boxes[i].y + offsetY);
                double distance = std::pow(targetPoint.x - center.x, 2) + std::pow(targetPoint.y - center.y, 2);
                if (distance < minDistance)
                {
                    minDistance = distance;
                    nearestIdx = static_cast<int>(i);
                    targetY = targetPoint.y;
                }
            }
        }
    }

    if (nearestIdx == -1)
    {
        return nullptr;
    }

    int finalY = 0;
    if (classes[nearestIdx] == classHead)
    {
        int headOffsetY = static_cast<int>(boxes[nearestIdx].height * headOffset);
        finalY = boxes[nearestIdx].y + headOffsetY - boxes[nearestIdx].height / 2;
    }
    else
    {
        finalY = targetY - boxes[nearestIdx].height / 2;
    }

    int finalX = boxes[nearestIdx].x;
    int finalW = boxes[nearestIdx].width;
    int finalH = boxes[nearestIdx].height;
    int finalClass = classes[nearestIdx];

    double pivotX = finalX + (finalW / 2.0);
    double pivotY = finalY + (finalH / 2.0);

    return new AimbotTarget(finalX, finalY, finalW, finalH, finalClass, pivotX, pivotY);
}

float MultiTargetTracker::iou(const cv::Rect2f& a, const cv::Rect2f& b)
{
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width, b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    const float w = std::max(0.0f, x2 - x1);
    const float h = std::max(0.0f, y2 - y1);
    const float inter = w * h;
    const float ua = a.width * a.height + b.width * b.height - inter;
    if (ua <= 1e-6f) return 0.0f;
    return inter / ua;
}

namespace
{
std::vector<int> hungarianMinimize(const std::vector<std::vector<double>>& costs)
{
    const int n = static_cast<int>(costs.size());
    if (n == 0)
        return {};
    const int m = static_cast<int>(costs[0].size());
    if (m == 0)
        return std::vector<int>(n, -1);

    std::vector<double> u(n + 1, 0.0);
    std::vector<double> v(m + 1, 0.0);
    std::vector<int> p(m + 1, 0);
    std::vector<int> way(m + 1, 0);

    for (int i = 1; i <= n; ++i)
    {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(m + 1, std::numeric_limits<double>::infinity());
        std::vector<char> used(m + 1, 0);

        do
        {
            used[j0] = 1;
            const int i0 = p[j0];
            double delta = std::numeric_limits<double>::infinity();
            int j1 = 0;

            for (int j = 1; j <= m; ++j)
            {
                if (used[j])
                    continue;

                const double cur = costs[i0 - 1][j - 1] - u[i0] - v[j];
                if (cur < minv[j])
                {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta)
                {
                    delta = minv[j];
                    j1 = j;
                }
            }

            for (int j = 0; j <= m; ++j)
            {
                if (used[j])
                {
                    u[p[j]] += delta;
                    v[j] -= delta;
                }
                else
                {
                    minv[j] -= delta;
                }
            }

            j0 = j1;
        } while (p[j0] != 0);

        do
        {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    std::vector<int> assignment(n, -1);
    for (int j = 1; j <= m; ++j)
    {
        if (p[j] > 0)
            assignment[p[j] - 1] = j - 1;
    }
    return assignment;
}
}

int MultiTargetTracker::findTrackIndexById(int id) const
{
    for (size_t i = 0; i < tracks_.size(); ++i)
    {
        if (tracks_[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

int MultiTargetTracker::allowedMissedFrames(const TrackState& t) const
{
    // Keep the locked target alive longer to survive short occlusion/fast motion bursts.
    const int lockedBonus = (t.id == lockedTrackId_) ? 8 : 0;
    return maxMissedFrames_ + lockedBonus;
}

void MultiTargetTracker::pruneDeadTracks()
{
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(), [&](const TrackState& t) {
            return t.missed > allowedMissedFrames(t);
            }),
        tracks_.end());
}

int MultiTargetTracker::chooseBestTrack(int screenWidth, int screenHeight) const
{
    if (tracks_.empty())
        return -1;

    const double cx = screenWidth * 0.5;
    const double cy = screenHeight * 0.5;

    int bestIdx = -1;
    double bestScore = std::numeric_limits<double>::max();

    for (size_t i = 0; i < tracks_.size(); ++i)
    {
        const auto& t = tracks_[i];
        if (t.missed > allowedMissedFrames(t))
            continue;

        const double dx = t.pivotX - cx;
        const double dy = t.pivotY - cy;
        const double dist = std::hypot(dx, dy);
        const double hitBonus = std::min(5, t.hits) * 4.0;
        const double missPenalty = t.missed * 50.0;
        const double score = dist + missPenalty - hitBonus;

        if (score < bestScore)
        {
            bestScore = score;
            bestIdx = static_cast<int>(i);
        }
    }

    return bestIdx;
}

void MultiTargetTracker::reset()
{
    tracks_.clear();
    nextId_ = 1;
    lockedTrackId_ = -1;
    frameDtMeanSec_ = 1.0 / 60.0;
    lastTrackerFrameTime_ = {};
}

void MultiTargetTracker::update(
    const std::vector<cv::Rect>& boxes,
    const std::vector<int>& classes,
    int screenWidth,
    int screenHeight,
    bool disableHeadshot,
    bool keepCurrentLock,
    std::chrono::steady_clock::time_point observationTime)
{
    auto now = (observationTime.time_since_epoch().count() != 0)
        ? observationTime
        : std::chrono::steady_clock::now();
    if (lastTrackerFrameTime_.time_since_epoch().count() != 0)
    {
        double frameDt = std::chrono::duration<double>(now - lastTrackerFrameTime_).count();
        if (!std::isfinite(frameDt) || frameDt <= 0.0)
        {
            const double fallbackDt = std::clamp(frameDtMeanSec_, 1.0 / 500.0, 0.25);
            now = lastTrackerFrameTime_ +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(fallbackDt));
            frameDt = fallbackDt;
        }
        frameDt = std::clamp(frameDt, 1.0 / 500.0, 0.25);
        frameDtMeanSec_ = frameDtMeanSec_ * 0.88 + frameDt * 0.12;
    }
    lastTrackerFrameTime_ = now;

    int classPlayer = 0;
    int classHead = 1;
    float bodyOffset = 0.15f;
    float headOffset = 0.05f;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        classPlayer = config.class_player;
        classHead = config.class_head;
        bodyOffset = config.body_y_offset;
        headOffset = config.head_y_offset;
    }

    for (auto& t : tracks_)
        t.observedThisFrame = false;

    const size_t inputCount = std::min(boxes.size(), classes.size());

    std::vector<DetectionCandidate> dets;
    dets.reserve(inputCount);
    for (size_t i = 0; i < inputCount; ++i)
    {
        const int cls = classes[i];
        if (disableHeadshot)
        {
            if (cls != classPlayer)
                continue;
        }
        else
        {
            if (cls != classPlayer && cls != classHead)
            {
                continue;
            }
        }

        const cv::Rect& b = boxes[i];
        if (b.width <= 0 || b.height <= 0)
            continue;

        const double yOffset = (cls == classHead) ? headOffset : bodyOffset;
        DetectionCandidate d;
        d.box = cv::Rect2f(static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.width), static_cast<float>(b.height));
        d.classId = cls;
        d.pivotX = b.x + b.width * 0.5;
        d.pivotY = b.y + b.height * yOffset;
        dets.push_back(d);
    }

    if (!disableHeadshot && !dets.empty())
    {
        // If head and player detections refer to the same entity, keep one track candidate
        // (player box for stable identity) but move its pivot to the head point.
        std::vector<size_t> playerIdx;
        playerIdx.reserve(dets.size());
        for (size_t i = 0; i < dets.size(); ++i)
        {
            if (dets[i].classId == classPlayer)
                playerIdx.push_back(i);
        }

        if (!playerIdx.empty())
        {
            std::vector<char> dropHead(dets.size(), 0);
            std::vector<char> playerHasHeadPivot(dets.size(), 0);
            std::vector<double> playerHeadPivotX(dets.size(), 0.0);
            std::vector<double> playerHeadPivotY(dets.size(), 0.0);
            std::vector<double> playerHeadPivotDist(dets.size(), std::numeric_limits<double>::max());

            for (size_t hi = 0; hi < dets.size(); ++hi)
            {
                const auto& h = dets[hi];
                if (h.classId != classHead)
                    continue;

                const double headCx = h.box.x + h.box.width * 0.5;
                const double headCy = h.box.y + h.box.height * 0.5;

                size_t bestPlayer = static_cast<size_t>(-1);
                double bestDist = std::numeric_limits<double>::max();

                for (size_t pi : playerIdx)
                {
                    const auto& p = dets[pi].box;
                    const double px1 = p.x - p.width * 0.15;
                    const double px2 = p.x + p.width * 1.15;
                    const double py1 = p.y - p.height * 0.20;
                    const double py2 = p.y + p.height * 0.65;

                    if (!(headCx >= px1 && headCx <= px2 && headCy >= py1 && headCy <= py2))
                        continue;

                    const double pCx = p.x + p.width * 0.5;
                    const double pCy = p.y + p.height * 0.5;
                    const double d = std::hypot(headCx - pCx, headCy - pCy);
                    if (d < bestDist)
                    {
                        bestDist = d;
                        bestPlayer = pi;
                    }
                }

                if (bestPlayer != static_cast<size_t>(-1))
                {
                    dropHead[hi] = 1;
                    if (!playerHasHeadPivot[bestPlayer] || bestDist < playerHeadPivotDist[bestPlayer])
                    {
                        playerHasHeadPivot[bestPlayer] = 1;
                        playerHeadPivotDist[bestPlayer] = bestDist;
                        playerHeadPivotX[bestPlayer] = h.box.x + h.box.width * 0.5;
                        playerHeadPivotY[bestPlayer] = h.box.y + h.box.height * headOffset;
                    }
                }
            }

            std::vector<DetectionCandidate> filtered;
            filtered.reserve(dets.size());

            for (size_t i = 0; i < dets.size(); ++i)
            {
                if (dropHead[i])
                    continue;

                DetectionCandidate d = dets[i];
                if (d.classId == classPlayer && playerHasHeadPivot[i])
                {
                    d.pivotX = playerHeadPivotX[i];
                    d.pivotY = playerHeadPivotY[i];
                }
                filtered.push_back(d);
            }

            dets.swap(filtered);
        }
    }

    std::vector<int> detAssigned(dets.size(), -1);
    std::vector<int> trackAssigned(tracks_.size(), -1);
    constexpr double kUnassignedCost = 2.0;
    constexpr double kInvalidCost = 1000000.0;

    auto computeMatchScore = [&](const TrackState& t, const DetectionCandidate& d, bool relaxedForLocked) -> double
        {
            const bool sameClass = (d.classId == t.classId);
            bool classSwappedWithinTarget = false;
            double classPenalty = 0.0;
            if (!sameClass)
            {
                const bool allowHeadBodySwap =
                    !disableHeadshot &&
                    ((t.classId == classPlayer && d.classId == classHead) ||
                     (t.classId == classHead && d.classId == classPlayer));
                if (!allowHeadBodySwap)
                    return std::numeric_limits<double>::infinity();

                classSwappedWithinTarget = true;
                classPenalty = 0.18;
            }

            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                1e-4, 0.25
            );

            const float predCx = t.box.x + t.box.width * 0.5f + t.velocity.x * static_cast<float>(dt);
            const float predCy = t.box.y + t.box.height * 0.5f + t.velocity.y * static_cast<float>(dt);
            cv::Rect2f predBox(predCx - t.box.width * 0.5f, predCy - t.box.height * 0.5f, t.box.width, t.box.height);

            const double detCx = d.box.x + d.box.width * 0.5;
            const double detCy = d.box.y + d.box.height * 0.5;
            const double boxDist = std::hypot(detCx - predCx, detCy - predCy);
            const double predPivotX = t.pivotX + t.velocity.x * dt;
            const double predPivotY = t.pivotY + t.velocity.y * dt;
            const double pivotDist = std::hypot(d.pivotX - predPivotX, d.pivotY - predPivotY);
            const double dist = classSwappedWithinTarget ? pivotDist : std::min(boxDist, pivotDist);

            const double diag = std::hypot(static_cast<double>(t.box.width), static_cast<double>(t.box.height));
            const double speed = std::hypot(t.velocity.x, t.velocity.y);
            const double baseGate = std::max(24.0, diag * 1.15 + 10.0);
            const double speedGate = speed * dt * (1.8 + t.missed * 0.35);
            const double missGate = t.missed * std::max(14.0, diag * 0.18);
            double maxDist = baseGate + speedGate + missGate;
            if (relaxedForLocked)
                maxDist *= 1.6;

            if (dist > maxDist)
                return std::numeric_limits<double>::infinity();

            const double overlap = iou(predBox, d.box);
            const double missPenalty = t.missed * 0.025;
            const double hitBonus = std::min(6, t.hits) * 0.01;
            const double lockedBonus = (t.id == lockedTrackId_) ? 0.10 : 0.0;
            return (dist / maxDist) + (1.0 - overlap) * 0.30 + classPenalty + missPenalty - hitBonus - lockedBonus;
        };

    if (!tracks_.empty() && !dets.empty())
    {
        const size_t matrixSize = std::max(tracks_.size(), dets.size());
        std::vector<std::vector<double>> costs(
            matrixSize,
            std::vector<double>(matrixSize, kUnassignedCost));

        for (size_t ti = 0; ti < tracks_.size(); ++ti)
        {
            const auto& t = tracks_[ti];
            for (size_t di = 0; di < dets.size(); ++di)
            {
                const bool relaxedForLocked = (t.id == lockedTrackId_);
                const double score = computeMatchScore(t, dets[di], relaxedForLocked);
                costs[ti][di] = std::isfinite(score) ? score : kInvalidCost;
            }
        }

        const auto assignment = hungarianMinimize(costs);
        for (size_t ti = 0; ti < tracks_.size(); ++ti)
        {
            if (ti >= assignment.size())
                continue;

            const int di = assignment[ti];
            if (di < 0 || di >= static_cast<int>(dets.size()))
                continue;

            const double assignedCost = costs[ti][di];
            if (!std::isfinite(assignedCost) ||
                assignedCost >= kInvalidCost ||
                assignedCost >= kUnassignedCost)
            {
                continue;
            }

            trackAssigned[ti] = di;
            detAssigned[di] = static_cast<int>(ti);
        }
    }

    for (size_t ti = 0; ti < tracks_.size(); ++ti)
    {
        auto& t = tracks_[ti];
        const int di = trackAssigned[ti];

        if (di >= 0)
        {
            const auto& d = dets[di];
            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                1e-4, 0.2
            );

            const float oldX = static_cast<float>(t.pivotX);
            const float oldY = static_cast<float>(t.pivotY);
            const float newX = static_cast<float>(d.pivotX);
            const float newY = static_cast<float>(d.pivotY);
            const cv::Point2f rawVel(
                static_cast<float>((newX - oldX) / dt),
                static_cast<float>((newY - oldY) / dt)
            );

            cv::Point2f clampedRawVel = rawVel;
            const double rawSpeed = std::hypot(clampedRawVel.x, clampedRawVel.y);
            const double maxReasonableSpeed = std::max(screenWidth, screenHeight) * 3.5;
            if (rawSpeed > maxReasonableSpeed && rawSpeed > 1e-4)
            {
                const float scale = static_cast<float>(maxReasonableSpeed / rawSpeed);
                clampedRawVel *= scale;
            }

            const float blend = (t.id == lockedTrackId_) ? 0.45f : 0.35f;
            t.velocity = t.velocity * (1.0f - blend) + clampedRawVel * blend;
            t.box = d.box;
            t.pivotX = d.pivotX;
            t.pivotY = d.pivotY;
            t.classId = d.classId;
            t.hits += 1;
            t.missed = 0;
            t.observedThisFrame = true;
            t.lastUpdate = now;
        }
        else
        {
            const double dt = std::clamp(
                std::chrono::duration<double>(now - t.lastUpdate).count(),
                0.0, 0.2
            );
            t.box.x += t.velocity.x * static_cast<float>(dt);
            t.box.y += t.velocity.y * static_cast<float>(dt);
            t.pivotX += t.velocity.x * dt;
            t.pivotY += t.velocity.y * dt;
            const float decay = (t.id == lockedTrackId_) ? 0.90f : 0.84f;
            t.velocity *= decay;
            t.missed += 1;
            t.observedThisFrame = false;
            t.lastUpdate = now;
        }
    }

    for (size_t di = 0; di < dets.size(); ++di)
    {
        if (detAssigned[di] != -1)
            continue;

        const auto& d = dets[di];
        TrackState t;
        t.id = nextId_++;
        t.box = d.box;
        t.classId = d.classId;
        t.hits = 1;
        t.missed = 0;
        t.observedThisFrame = true;
        t.pivotX = d.pivotX;
        t.pivotY = d.pivotY;
        t.lastUpdate = now;
        tracks_.push_back(t);
    }

    pruneDeadTracks();

    if (findTrackIndexById(lockedTrackId_) < 0)
        lockedTrackId_ = -1;

    if (!keepCurrentLock)
    {
        const int bestIdx = chooseBestTrack(screenWidth, screenHeight);
        lockedTrackId_ = (bestIdx >= 0) ? tracks_[bestIdx].id : -1;
        return;
    }

    if (lockedTrackId_ == -1)
    {
        const int bestIdx = chooseBestTrack(screenWidth, screenHeight);
        lockedTrackId_ = (bestIdx >= 0) ? tracks_[bestIdx].id : -1;
    }
}

bool MultiTargetTracker::getLockedTarget(LockedTargetInfo& out) const
{
    const int idx = findTrackIndexById(lockedTrackId_);
    if (idx < 0)
        return false;

    const auto& t = tracks_[idx];
    if (t.missed > allowedMissedFrames(t))
        return false;

    out.trackId = t.id;
    out.observedThisFrame = t.observedThisFrame;
    out.missedFrames = t.missed;
    out.target = AimbotTarget(
        static_cast<int>(std::lround(t.box.x)),
        static_cast<int>(std::lround(t.box.y)),
        static_cast<int>(std::lround(t.box.width)),
        static_cast<int>(std::lround(t.box.height)),
        t.classId,
        t.pivotX,
        t.pivotY
    );
    return true;
}

std::vector<TrackDebugInfo> MultiTargetTracker::getDebugTracks() const
{
    std::vector<TrackDebugInfo> out;
    out.reserve(tracks_.size());

    for (const auto& t : tracks_)
    {
        if (t.missed > allowedMissedFrames(t))
            continue;

        TrackDebugInfo d;
        d.trackId = t.id;
        d.classId = t.classId;
        d.box = cv::Rect(
            static_cast<int>(std::lround(t.box.x)),
            static_cast<int>(std::lround(t.box.y)),
            static_cast<int>(std::lround(t.box.width)),
            static_cast<int>(std::lround(t.box.height))
        );
        d.pivotX = t.pivotX;
        d.pivotY = t.pivotY;
        d.velocityX = t.velocity.x;
        d.velocityY = t.velocity.y;
        d.lastUpdate = t.lastUpdate;
        d.observedThisFrame = t.observedThisFrame;
        d.missedFrames = t.missed;
        d.isLocked = (t.id == lockedTrackId_);
        out.push_back(d);
    }

    return out;
}
