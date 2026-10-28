#pragma once
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <opencv2/opencv.hpp>

struct DetectionBuffer
{
    std::mutex mutex;
    std::condition_variable cv;
    int version = 0;
    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    std::vector<float> confidences;
    std::chrono::steady_clock::time_point frameTimestamp{};
    std::chrono::steady_clock::time_point publishTimestamp{};
    // Written under both generationMutex and mutex. Readers may use either
    // lock; mouse-loop snapshots read these together with detections under mutex.
    std::uint64_t generation = 0;
    bool paused = false;
    std::chrono::steady_clock::time_point generationStartedAt{};

    std::uint64_t currentGeneration()
    {
        std::shared_lock<std::shared_mutex> lock(generationMutex);
        return generation;
    }

    bool isPaused()
    {
        std::shared_lock<std::shared_mutex> lock(generationMutex);
        return paused;
    }

    void togglePaused()
    {
        std::unique_lock<std::shared_mutex> generationLock(generationMutex);
        std::lock_guard<std::mutex> lock(mutex);
        paused = !paused;
        ++generation;
        generationStartedAt = std::chrono::steady_clock::now();
        clearLocked();
    }

    // The callback must not re-enter the generation gate. Shared access keeps
    // frame submission/publication independent of device I/O. Pause takes the
    // exclusive lock, waits for in-flight input, and invalidates frames/moves.
    template <typename Func>
    bool withCurrentGeneration(std::uint64_t expectedGeneration, Func&& func)
    {
        std::shared_lock<std::shared_mutex> lock(generationMutex);
        if (paused || generation != expectedGeneration)
            return false;
        func();
        return true;
    }

    bool setForGeneration(
        std::uint64_t expectedGeneration,
        const std::vector<cv::Rect>& newBoxes,
        const std::vector<int>& newClasses,
        const std::vector<float>& newConfidences,
        std::chrono::steady_clock::time_point newFrameTimestamp)
    {
        std::shared_lock<std::shared_mutex> generationLock(generationMutex);
        std::lock_guard<std::mutex> lock(mutex);
        if (paused || generation != expectedGeneration ||
            (newFrameTimestamp.time_since_epoch().count() != 0 && newFrameTimestamp < generationStartedAt))
            return false;
        setLocked(newBoxes, newClasses, newConfidences, newFrameTimestamp);
        return true;
    }

    void set(const std::vector<cv::Rect>& newBoxes, const std::vector<int>& newClasses)
    {
        set(newBoxes, newClasses, std::vector<float>(), {});
    }

    void set(
        const std::vector<cv::Rect>& newBoxes,
        const std::vector<int>& newClasses,
        const std::vector<float>& newConfidences,
        std::chrono::steady_clock::time_point newFrameTimestamp = {})
    {
        std::shared_lock<std::shared_mutex> generationLock(generationMutex);
        std::lock_guard<std::mutex> lock(mutex);
        if (!paused)
            setLocked(newBoxes, newClasses, newConfidences, newFrameTimestamp);
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        clearLocked();
    }

    void get(
        std::vector<cv::Rect>& outBoxes,
        std::vector<int>& outClasses,
        int& outVersion,
        std::chrono::steady_clock::time_point* outFrameTimestamp = nullptr,
        std::chrono::steady_clock::time_point* outPublishTimestamp = nullptr)
    {
        std::vector<float> ignoredConfidences;
        get(outBoxes, outClasses, ignoredConfidences, outVersion, outFrameTimestamp, outPublishTimestamp);
    }

    void get(
        std::vector<cv::Rect>& outBoxes,
        std::vector<int>& outClasses,
        std::vector<float>& outConfidences,
        int& outVersion,
        std::chrono::steady_clock::time_point* outFrameTimestamp = nullptr,
        std::chrono::steady_clock::time_point* outPublishTimestamp = nullptr)
    {
        std::lock_guard<std::mutex> lock(mutex);
        outBoxes = boxes;
        outClasses = classes;
        outConfidences = confidences;
        outVersion = version;
        if (outFrameTimestamp)
            *outFrameTimestamp = frameTimestamp;
        if (outPublishTimestamp)
            *outPublishTimestamp = publishTimestamp;
    }

private:
    std::shared_mutex generationMutex;

    void setLocked(
        const std::vector<cv::Rect>& newBoxes,
        const std::vector<int>& newClasses,
        const std::vector<float>& newConfidences,
        std::chrono::steady_clock::time_point newFrameTimestamp)
    {
        const auto now = std::chrono::steady_clock::now();
        boxes = newBoxes;
        classes = newClasses;
        confidences = newConfidences;
        frameTimestamp = (newFrameTimestamp.time_since_epoch().count() != 0) ? newFrameTimestamp : now;
        publishTimestamp = now;
        ++version;
        cv.notify_all();
    }

    void clearLocked()
    {
        const auto now = std::chrono::steady_clock::now();
        boxes.clear();
        classes.clear();
        confidences.clear();
        frameTimestamp = now;
        publishTimestamp = now;
        ++version;
        cv.notify_all();
    }
};
