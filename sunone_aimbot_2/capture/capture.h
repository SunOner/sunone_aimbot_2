#ifndef CAPTURE_H
#define CAPTURE_H

#include <opencv2/opencv.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <cstdint>

extern std::atomic<bool> detection_resolution_changed;
extern std::atomic<bool> capture_method_changed;
extern std::atomic<bool> capture_cursor_changed;
extern std::atomic<bool> capture_borders_changed;
extern std::atomic<bool> capture_fps_changed;
extern std::atomic<bool> capture_window_changed;
extern std::deque<cv::Mat> frameQueue;

void captureThread(int CAPTURE_WIDTH, int CAPTURE_HEIGHT);
extern std::atomic<int> screenWidth;
extern std::atomic<int> screenHeight;

extern std::atomic<int> captureFrameCount;
extern std::atomic<int> captureFps;
extern std::chrono::time_point<std::chrono::high_resolution_clock> captureFpsStartTime;

extern std::atomic<uint64_t> captureWinrtPollAttemptsTotal;
extern std::atomic<uint64_t> captureWinrtFramesDrainedTotal;
extern std::atomic<uint64_t> captureWinrtFramesReturnedTotal;
extern std::atomic<uint64_t> captureWinrtEmptyPollsTotal;
extern std::atomic<uint64_t> captureWinrtReadbackMicrosTotal;
extern std::atomic<uint64_t> captureWinrtMapMicrosTotal;
extern std::atomic<uint64_t> captureWinrtPixelCopyMicrosTotal;

extern cv::Mat latestFrame;

extern std::mutex frameMutex;
extern std::condition_variable frameCV;
extern std::atomic<bool> shouldExit;

class IScreenCapture
{
public:
    virtual ~IScreenCapture() {}
    virtual cv::Mat GetNextFrameCpu() = 0;
    bool GetSourceDimensions(int& width, int& height) const
    {
        width = sourceWidth_;
        height = sourceHeight_;
        return width > 0 && height > 0;
    }

protected:
    void SetSourceDimensions(int width, int height)
    {
        sourceWidth_ = width;
        sourceHeight_ = height;
    }

private:
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
};

#ifdef USE_CUDA
cv::Mat getCurrentDetectionSuppressionMask();
extern std::atomic<uint64_t> captureGpuAttemptsTotal;
extern std::atomic<uint64_t> captureGpuCapturedTotal;
extern std::atomic<uint64_t> captureGpuTimeoutTotal;
extern std::atomic<uint64_t> captureGpuAccumulatedFramesTotal;
extern std::atomic<uint64_t> captureGpuMissedFramesTotal;
extern std::atomic<uint64_t> captureGpuPresentFramesTotal;
extern std::atomic<uint64_t> captureGpuMouseOnlyEventsTotal;
extern std::atomic<uint64_t> captureGpuMetadataOnlyEventsTotal;
extern std::atomic<uint64_t> captureGpuCoalescedEventsTotal;
extern std::atomic<uint64_t> captureCpuFallbackAttemptsTotal;
extern std::atomic<uint64_t> captureCpuFallbackFramesTotal;
#endif

#endif // CAPTURE_H
