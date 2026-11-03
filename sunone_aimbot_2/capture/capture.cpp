#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <iostream>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <timeapi.h>
#include <condition_variable>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "capture.h"
#include "nodes/vision_runtime.h"
#ifdef USE_CUDA
#include "trt_detector.h"
#endif
#include "sunone_aimbot_2.h"
#include "keycodes.h"
#include "keyboard_listener.h"
#include "other_tools.h"
#include "duplication_api_capture.h"
#include "winrt_capture.h"
#include "virtual_camera.h"
#include "udp_capture.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

cv::Mat latestFrame;
std::mutex frameMutex;

std::atomic<int> screenWidth(0);
std::atomic<int> screenHeight(0);

std::atomic<int> captureFrameCount(0);
std::atomic<int> captureFps(0);
std::chrono::time_point<std::chrono::high_resolution_clock> captureFpsStartTime;

std::atomic<uint64_t> captureWinrtPollAttemptsTotal(0);
std::atomic<uint64_t> captureWinrtFramesDrainedTotal(0);
std::atomic<uint64_t> captureWinrtFramesReturnedTotal(0);
std::atomic<uint64_t> captureWinrtEmptyPollsTotal(0);
std::atomic<uint64_t> captureWinrtReadbackMicrosTotal(0);
std::atomic<uint64_t> captureWinrtMapMicrosTotal(0);
std::atomic<uint64_t> captureWinrtPixelCopyMicrosTotal(0);

std::deque<cv::Mat> frameQueue;

#ifdef USE_CUDA
namespace
{
std::mutex g_detectionSuppressionMaskMutex;
cv::Mat g_detectionSuppressionMask;
}

std::atomic<uint64_t> captureGpuAttemptsTotal(0);
std::atomic<uint64_t> captureGpuCapturedTotal(0);
std::atomic<uint64_t> captureGpuTimeoutTotal(0);
std::atomic<uint64_t> captureGpuAccumulatedFramesTotal(0);
std::atomic<uint64_t> captureGpuMissedFramesTotal(0);
std::atomic<uint64_t> captureGpuPresentFramesTotal(0);
std::atomic<uint64_t> captureGpuMouseOnlyEventsTotal(0);
std::atomic<uint64_t> captureGpuMetadataOnlyEventsTotal(0);
std::atomic<uint64_t> captureGpuCoalescedEventsTotal(0);
std::atomic<uint64_t> captureCpuFallbackAttemptsTotal(0);
std::atomic<uint64_t> captureCpuFallbackFramesTotal(0);

static void UpdateDetectionSuppressionMask(const cv::Mat& mask)
{
    std::lock_guard<std::mutex> lock(g_detectionSuppressionMaskMutex);
    if (!mask.empty() && mask.type() == CV_8UC1)
        g_detectionSuppressionMask = mask.clone();
    else
        g_detectionSuppressionMask.release();
}

cv::Mat getCurrentDetectionSuppressionMask()
{
    std::lock_guard<std::mutex> lock(g_detectionSuppressionMaskMutex);
    return g_detectionSuppressionMask.clone();
}
#endif

namespace
{
struct CaptureThreadConfig
{
    std::string capture_method;
    int capture_fps = 0;
    int detection_resolution = 0;
    int monitor_idx = 0;
    bool circle_fov_enabled = false;
    int circle_fov_radius_percent = 100;
    bool capture_borders = true;
    bool capture_cursor = true;
    std::string capture_target;
    std::string capture_window_title;
    std::string capture_monitor_device;
    std::string virtual_camera_name;
    int virtual_camera_width = 0;
    int virtual_camera_heigth = 0;
    std::string udp_ip;
    int udp_port = 0;
    std::string backend;
    bool verbose = false;
#ifdef USE_CUDA
    bool capture_use_cuda = true;
#endif
};

CaptureThreadConfig SnapshotCaptureConfig()
{
    std::lock_guard<std::mutex> cfgLock(configMutex);
    CaptureThreadConfig snapshot;
    snapshot.capture_method = config.capture_method;
    snapshot.capture_fps = config.capture_fps;
    snapshot.detection_resolution = config.detection_resolution;
    snapshot.monitor_idx = config.monitor_idx;
    snapshot.circle_fov_enabled = config.circle_fov_enabled;
    snapshot.circle_fov_radius_percent = config.circle_fov_radius_percent;
    snapshot.capture_borders = config.capture_borders;
    snapshot.capture_cursor = config.capture_cursor;
    snapshot.capture_target = config.capture_target;
    snapshot.capture_window_title = config.capture_window_title;
    snapshot.capture_monitor_device = config.capture_monitor_device;
    snapshot.virtual_camera_name = config.virtual_camera_name;
    snapshot.virtual_camera_width = config.virtual_camera_width;
    snapshot.virtual_camera_heigth = config.virtual_camera_heigth;
    snapshot.udp_ip = config.udp_ip;
    snapshot.udp_port = config.udp_port;
    snapshot.backend = config.backend;
    snapshot.verbose = config.verbose;
#ifdef USE_CUDA
    snapshot.capture_use_cuda = config.capture_use_cuda;
#endif
    return snapshot;
}

#ifdef USE_CUDA
struct CudaCaptureDiagnostics
{
    uint64_t gpuAttempts = 0;
    uint64_t gpuCaptured = 0;
    uint64_t gpuTimeout = 0;
    uint64_t gpuNotReady = 0;
    uint64_t gpuDeviceLost = 0;
    uint64_t gpuAcquireFailed = 0;
    uint64_t gpuMissingTexture = 0;
    uint64_t gpuCudaMapFailed = 0;
    uint64_t gpuCudaArrayFailed = 0;
    uint64_t gpuCudaCopyFailed = 0;
    uint64_t gpuNoPresent = 0;
    uint64_t gpuAccumulatedFrames = 0;
    uint64_t gpuMissedFrames = 0;
    uint64_t gpuPresentFrames = 0;
    uint64_t gpuMouseOnlyEvents = 0;
    uint64_t gpuMetadataOnlyEvents = 0;
    uint64_t gpuCoalescedEvents = 0;
    uint64_t gpuSubmitted = 0;
    uint64_t gpuCpuCopies = 0;
    uint64_t cpuFallbackAttempts = 0;
    uint64_t cpuFallbackFrames = 0;
    uint64_t cpuFallbackEmpty = 0;
    uint64_t cpuPathFrames = 0;
    uint64_t trtCpuSubmitted = 0;
    bool lastPreferGpu = false;
    bool lastNeedCpuCopy = false;
    std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
};

void CountGpuCaptureStatus(CudaCaptureDiagnostics& diag, GpuCaptureStatus status)
{
    switch (status)
    {
    case GpuCaptureStatus::Captured:
        diag.gpuCaptured++;
        captureGpuCapturedTotal.fetch_add(1, std::memory_order_relaxed);
        break;
    case GpuCaptureStatus::NotReady: diag.gpuNotReady++; break;
    case GpuCaptureStatus::Timeout:
        diag.gpuTimeout++;
        captureGpuTimeoutTotal.fetch_add(1, std::memory_order_relaxed);
        break;
    case GpuCaptureStatus::NoPresent: diag.gpuNoPresent++; break;
    case GpuCaptureStatus::DeviceLost: diag.gpuDeviceLost++; break;
    case GpuCaptureStatus::AcquireFailed: diag.gpuAcquireFailed++; break;
    case GpuCaptureStatus::MissingTexture: diag.gpuMissingTexture++; break;
    case GpuCaptureStatus::CudaMapFailed: diag.gpuCudaMapFailed++; break;
    case GpuCaptureStatus::CudaArrayFailed: diag.gpuCudaArrayFailed++; break;
    case GpuCaptureStatus::CudaCopyFailed: diag.gpuCudaCopyFailed++; break;
    }
}

void MaybeLogCudaCaptureDiagnostics(CudaCaptureDiagnostics& diag, const CaptureThreadConfig& cfg)
{
    if (!cfg.verbose)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - diag.lastLog < std::chrono::seconds(2))
        return;

    diag.lastLog = now;
    std::cout
        << "[CaptureDiag] backend=" << cfg.backend
        << " method=" << cfg.capture_method
        << " capture_fps=" << cfg.capture_fps
        << " use_cuda=" << (cfg.capture_use_cuda ? "true" : "false")
        << " circle_fov=" << (cfg.circle_fov_enabled ? "true" : "false")
        << " circle_fov_radius=" << cfg.circle_fov_radius_percent
        << " prefer_gpu=" << (diag.lastPreferGpu ? "true" : "false")
        << " need_cpu_copy=" << (diag.lastNeedCpuCopy ? "true" : "false")
        << " gpu_attempts=" << diag.gpuAttempts
        << " gpu_ok=" << diag.gpuCaptured
        << " gpu_timeout=" << diag.gpuTimeout
        << " gpu_no_present=" << diag.gpuNoPresent
        << " gpu_accumulated=" << diag.gpuAccumulatedFrames
        << " gpu_missed=" << diag.gpuMissedFrames
        << " gpu_present=" << diag.gpuPresentFrames
        << " gpu_mouse_only=" << diag.gpuMouseOnlyEvents
        << " gpu_metadata_only=" << diag.gpuMetadataOnlyEvents
        << " gpu_coalesced=" << diag.gpuCoalescedEvents
        << " gpu_not_ready=" << diag.gpuNotReady
        << " gpu_lost=" << diag.gpuDeviceLost
        << " gpu_acquire_failed=" << diag.gpuAcquireFailed
        << " gpu_missing_tex=" << diag.gpuMissingTexture
        << " cuda_map_failed=" << diag.gpuCudaMapFailed
        << " cuda_array_failed=" << diag.gpuCudaArrayFailed
        << " cuda_copy_failed=" << diag.gpuCudaCopyFailed
        << " trt_gpu_submitted=" << diag.gpuSubmitted
        << " gpu_cpu_copies=" << diag.gpuCpuCopies
        << " cpu_fallback_attempts=" << diag.cpuFallbackAttempts
        << " cpu_fallback_frames=" << diag.cpuFallbackFrames
        << " cpu_fallback_empty=" << diag.cpuFallbackEmpty
        << " cpu_path_frames=" << diag.cpuPathFrames
        << " trt_cpu_submitted=" << diag.trtCpuSubmitted
        << std::endl;
}
#endif

std::string NormalizeCaptureMethod(const std::string& method)
{
    if (method == "duplication_api" || method == "winrt" || method == "virtual_camera" || method == "udp_capture")
        return method;
    return "duplication_api";
}

bool IsWinrtWindowTarget(const CaptureThreadConfig& cfg)
{
    return NormalizeCaptureMethod(cfg.capture_method) == "winrt" && cfg.capture_target == "window";
}

bool IsWinrtWindowTargetMissing(const CaptureThreadConfig& cfg)
{
    if (!IsWinrtWindowTarget(cfg))
        return false;

    if (OtherTools::TrimAscii(cfg.capture_window_title).empty())
        return true;

    return FindCaptureWindowByTitle(cfg.capture_window_title) == nullptr;
}

class TimerResolutionGuard
{
public:
    void Enable()
    {
        if (!enabled_)
        {
            timeBeginPeriod(1);
            enabled_ = true;
        }
    }

    void Disable()
    {
        if (enabled_)
        {
            timeEndPeriod(1);
            enabled_ = false;
        }
    }

    ~TimerResolutionGuard()
    {
        Disable();
    }

private:
    bool enabled_{ false };
};

class WinrtApartmentGuard
{
public:
    void Ensure(bool required)
    {
        if (required && !initialized_)
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized_ = true;
        }
        else if (!required && initialized_)
        {
            winrt::uninit_apartment();
            initialized_ = false;
        }
    }

    ~WinrtApartmentGuard()
    {
        if (initialized_)
            winrt::uninit_apartment();
    }

private:
    bool initialized_{ false };
};

} // namespace

std::vector<cv::Mat> getBatchFromQueue(int batch_size)
{
    std::vector<cv::Mat> batch;
    std::lock_guard<std::mutex> lk(frameMutex);
    const size_t target_size = (batch_size > 0) ? static_cast<size_t>(batch_size) : 0;
    const size_t n = std::min(frameQueue.size(), target_size);

    for (size_t i = 0; i < n; ++i)
        batch.push_back(frameQueue[frameQueue.size() - n + i]);

    while (batch.size() < target_size && !batch.empty())
        batch.push_back(batch.back().clone());
    return batch;
}

void captureThread(int CAPTURE_WIDTH, int CAPTURE_HEIGHT)
{
    try
    {
        const bool initiallyRequested=NodeVision::syncCaptureConfiguration();
        CaptureThreadConfig currentCfg = SnapshotCaptureConfig();
        if (currentCfg.verbose)
            std::cout << "[Capture] OpenCV version: " << CV_VERSION << std::endl;

        int captureWidth = std::max(1, CAPTURE_WIDTH);
        int captureHeight = std::max(1, CAPTURE_HEIGHT);
        if (currentCfg.detection_resolution > 0)
        {
            captureWidth = currentCfg.detection_resolution;
            captureHeight = currentCfg.detection_resolution;
        }

        WinrtApartmentGuard winrtApartment;
        auto createCapturer = [&](const CaptureThreadConfig& cfg, int width, int height) -> std::unique_ptr<IScreenCapture>
        {
            try
            {
                const std::string method = NormalizeCaptureMethod(cfg.capture_method);
                if (method != cfg.capture_method)
                    std::cout << "[Capture] Unknown capture_method '" << cfg.capture_method << "'. Falling back to duplication_api." << std::endl;

                if (method == "duplication_api")
                {
                    if (cfg.verbose)
                        std::cout << "[Capture] Using Duplication API" << std::endl;
                    auto capture = std::make_unique<DuplicationAPIScreenCapture>(width, height, cfg.monitor_idx,cfg.capture_monitor_device);
                    if (!capture->isInitialized())
                        return nullptr;
                    return capture;
                }

                if (method == "winrt")
                {
                    if (cfg.verbose)
                        std::cout << "[Capture] Using WinRT" << std::endl;

                    WinRTScreenCapture::Options options;
                    options.target = cfg.capture_target;
                    options.windowTitle = cfg.capture_window_title;
                    options.monitorIndex = cfg.monitor_idx;
                    options.monitorDevice = cfg.capture_monitor_device;
                    options.captureBorders = cfg.capture_borders;
                    options.captureCursor = cfg.capture_cursor;

                    return std::make_unique<WinRTScreenCapture>(width, height, options);
                }

                if (method == "virtual_camera")
                {
                    if (cfg.verbose)
                        std::cout << "[Capture] Using Virtual Camera" << std::endl;
                    return std::make_unique<VirtualCameraCapture>(
                        cfg.virtual_camera_width,
                        cfg.virtual_camera_heigth,
                        cfg.virtual_camera_name,
                        cfg.capture_fps,
                        cfg.verbose
                    );
                }

                if (cfg.verbose)
                    std::cout << "[Capture] Using UDP capture" << std::endl;
                auto capture = std::make_unique<UDPCapture>(width, height, cfg.udp_ip, cfg.udp_port);
                if (!capture->isInitialized())
                    return nullptr;
                return capture;
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Failed to initialize '" << cfg.capture_method
                    << "' capture: " << e.what() << std::endl;
                return nullptr;
            }
        };

        auto publishCaptureSourceSize = [](const IScreenCapture* capture)
        {
            int sourceWidth = 0;
            int sourceHeight = 0;
            if (capture && capture->GetSourceDimensions(sourceWidth, sourceHeight) &&
                sourceWidth > 0 && sourceHeight > 0)
            {
                screenWidth.store(sourceWidth, std::memory_order_relaxed);
                screenHeight.store(sourceHeight, std::memory_order_relaxed);
            }
            else
            {
                screenWidth.store(0, std::memory_order_relaxed);
                screenHeight.store(0, std::memory_order_relaxed);
            }
        };

        std::string desiredCaptureMethod = NormalizeCaptureMethod(currentCfg.capture_method);
        winrtApartment.Ensure(initiallyRequested&&desiredCaptureMethod == "winrt");

        std::unique_ptr<IScreenCapture> capturer = initiallyRequested?createCapturer(currentCfg, captureWidth, captureHeight):nullptr;
        publishCaptureSourceSize(capturer.get());
        std::string activeCapturerMethod = capturer ? desiredCaptureMethod : std::string();
        auto lastCapturerCreateAttempt = std::chrono::steady_clock::now();
        bool waitingForWinrtWindowTarget = !capturer && IsWinrtWindowTargetMissing(currentCfg);

        auto clearCaptureFrames = [&]()
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            latestFrame.release();
            frameQueue.clear();
        };

        auto clearDetections = [&]()
        {
            detectionBuffer.clear();
        };

        auto markCaptureUnavailable = [&]()
        {
            clearCaptureFrames();
            clearDetections();
            frameCV.notify_one();
        };

        bool captureUnavailable = false;
        auto setCaptureUnavailable = [&]()
        {
            if (captureUnavailable)
                return;
            captureUnavailable = true;
            markCaptureUnavailable();
        };
        auto setCaptureAvailable = [&]()
        {
            captureUnavailable = false;
        };

        // Do not keep stale preview/detections from previous capture state.
        setCaptureUnavailable();

        TimerResolutionGuard timerResolution;
        // Always request 1 ms timer resolution for the lifetime of the capture thread.
        // The loop performs short sleeps or yields in the "no new frame" backoff paths
        // (when capture_fps == 0 / unlimited) to avoid 100% CPU spin while waiting for
        // Duplication API / WinRT to deliver a present. It also needs precise sleeps for
        // the frame limiter when a cap is active.
        // Without timeBeginPeriod(1), these round up to the default Windows timer
        // resolution (~15.6 ms), throttling the capture loop (and reported captureFps)
        // unless something else in the process (the overlay/GUI D3D presents + short
        // sleep loops) has already bumped the resolution as a side effect.
        timerResolution.Enable();

        std::optional<std::chrono::steady_clock::duration> frameDuration;
        auto updateFrameDuration = [&](int captureFpsSetting)
        {
            if (captureFpsSetting > 0)
            {
                const auto frameMs = std::chrono::duration<double, std::milli>(1000.0 / captureFpsSetting);
                frameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(frameMs);
            }
            else
            {
                frameDuration.reset();
                // Do NOT Disable() the timer resolution here. The 1 ms backoff sleeps
                // (see the four sites below that check !frameDuration.has_value()) and
                // the applyFrameLimiter pacing still require it when running unlimited.
                // The guard's destructor will call timeEndPeriod(1) on thread exit.
            }
        };
        updateFrameDuration(currentCfg.capture_fps);

        captureFpsStartTime = std::chrono::high_resolution_clock::now();

        auto frameStartTime = std::chrono::steady_clock::now();
        auto applyFrameLimiter = [&]()
        {
            if (frameDuration.has_value())
            {
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = now - frameStartTime;
                if (elapsed < frameDuration.value())
                {
                    std::this_thread::sleep_for(frameDuration.value() - elapsed);
                }
            }
            frameStartTime = std::chrono::steady_clock::now();
        };

#ifdef USE_CUDA
        CudaCaptureDiagnostics cudaDiag;
#endif
        auto lastSuccessfulFrameTime = std::chrono::steady_clock::now();
        constexpr auto staleFrameTimeout = std::chrono::milliseconds(500);

        while (!shouldExit)
        {
            try
            {
                if(!NodeVision::syncCaptureConfiguration())
                {
                    setCaptureUnavailable();
                    capturer.reset();activeCapturerMethod.clear();winrtApartment.Ensure(false);
                    lastCapturerCreateAttempt=std::chrono::steady_clock::now()-std::chrono::seconds(1);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                currentCfg = SnapshotCaptureConfig();

            if (capture_fps_changed.exchange(false))
            {
                updateFrameDuration(currentCfg.capture_fps);
            }

            const bool resolutionChanged =
                currentCfg.detection_resolution > 0 &&
                (currentCfg.detection_resolution != captureWidth ||
                 currentCfg.detection_resolution != captureHeight);

            const bool needsReinit =
                detection_resolution_changed.exchange(false) ||
                resolutionChanged ||
                capture_method_changed.exchange(false) ||
                capture_cursor_changed.exchange(false) ||
                capture_borders_changed.exchange(false) ||
                capture_window_changed.exchange(false);

            if (needsReinit)
            {
                setCaptureUnavailable();
                waitingForWinrtWindowTarget = false;

                if (currentCfg.detection_resolution > 0)
                {
                    captureWidth = currentCfg.detection_resolution;
                    captureHeight = currentCfg.detection_resolution;
                }

                const std::string nextMethod = NormalizeCaptureMethod(currentCfg.capture_method);
                desiredCaptureMethod = nextMethod;
                const bool nextNeedsWinrt = (nextMethod == "winrt");

                // Always teardown current backend first to avoid overlap between old/new capture objects.
                // WinRT must be destroyed before apartment teardown.
                if (capturer)
                {
                    const bool activeWasWinrt = (activeCapturerMethod == "winrt");
                    capturer.reset();
                    activeCapturerMethod.clear();
                    if (activeWasWinrt && !nextNeedsWinrt)
                        winrtApartment.Ensure(false);
                }

                winrtApartment.Ensure(nextNeedsWinrt);

                if (nextMethod == "virtual_camera")
                    VirtualCameraCapture::GetAvailableVirtualCameras(true);

                capturer = createCapturer(currentCfg, captureWidth, captureHeight);
                publishCaptureSourceSize(capturer.get());
                if (capturer)
                    activeCapturerMethod = nextMethod;
                else
                {
                    activeCapturerMethod.clear();
                    waitingForWinrtWindowTarget = IsWinrtWindowTargetMissing(currentCfg);
                }

                lastCapturerCreateAttempt = std::chrono::steady_clock::now();
                if (currentCfg.verbose)
                    std::cout << "[Capture] Reinitialized capture backend." << std::endl;
            }

            if (!capturer)
            {
                if (waitingForWinrtWindowTarget && IsWinrtWindowTarget(currentCfg))
                {
                    setCaptureUnavailable();
                    if (!frameDuration.has_value())
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    applyFrameLimiter();
                    continue;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now - lastCapturerCreateAttempt >= std::chrono::seconds(1))
                {
                    desiredCaptureMethod = NormalizeCaptureMethod(currentCfg.capture_method);
                    winrtApartment.Ensure(desiredCaptureMethod == "winrt");

                    if (desiredCaptureMethod == "virtual_camera")
                        VirtualCameraCapture::GetAvailableVirtualCameras(true);

                    capturer = createCapturer(currentCfg, captureWidth, captureHeight);
                    publishCaptureSourceSize(capturer.get());
                    lastCapturerCreateAttempt = now;

                    if (capturer)
                    {
                        waitingForWinrtWindowTarget = false;
                        activeCapturerMethod = desiredCaptureMethod;
                        lastSuccessfulFrameTime = now;
                        if (currentCfg.verbose)
                            std::cout << "[Capture] Capture backend recovered." << std::endl;
                    }
                    else
                    {
                        activeCapturerMethod.clear();
                        waitingForWinrtWindowTarget = IsWinrtWindowTargetMissing(currentCfg);
                    }
                }

                setCaptureUnavailable();
                if (!frameDuration.has_value())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                applyFrameLimiter();
                continue;
            }

#ifdef USE_CUDA
            // Visible Preview/dataset nodes request CPU pixels lazily from the
            // graph frame. The retired global preview must not download every frame.
            const bool needCpuCopyFromGpu = false;
#endif

            cv::Mat sourceCpu;
            cv::Mat detectionFrame;
            std::chrono::steady_clock::time_point frameTimestamp{};
            bool frameSubmittedToDetector = false;
            bool skipCpuFallbackThisFrame = false;
            bool keepCaptureAliveThisFrame = false;

#ifdef USE_CUDA
            const bool preferGpuCapturePath =
                NormalizeCaptureMethod(currentCfg.capture_method) == "duplication_api" &&
                currentCfg.capture_use_cuda;
            cudaDiag.lastPreferGpu = preferGpuCapturePath;
            cudaDiag.lastNeedCpuCopy = needCpuCopyFromGpu;

            if (preferGpuCapturePath)
            {
                auto* duplicationCapture = dynamic_cast<DuplicationAPIScreenCapture*>(capturer.get());
                if (duplicationCapture)
                {
                    cv::cuda::GpuMat capturedGpu;
                    GpuCaptureStatus gpuStatus = GpuCaptureStatus::NotReady;
                    uint32_t accumulatedFrames = 0;
                    DdaCaptureFrameInfo ddaFrameInfo;
                    auto countDdaFrameInfo = [&](const DdaCaptureFrameInfo& info)
                    {
                        if (info.hasLastPresentTime)
                        {
                            cudaDiag.gpuPresentFrames++;
                            captureGpuPresentFramesTotal.fetch_add(1, std::memory_order_relaxed);
                        }
                        else if (info.hasLastMouseUpdateTime)
                        {
                            cudaDiag.gpuMouseOnlyEvents++;
                            captureGpuMouseOnlyEventsTotal.fetch_add(1, std::memory_order_relaxed);
                        }
                        else
                        {
                            cudaDiag.gpuMetadataOnlyEvents++;
                            captureGpuMetadataOnlyEventsTotal.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (info.rectsCoalesced)
                        {
                            cudaDiag.gpuCoalescedEvents++;
                            captureGpuCoalescedEventsTotal.fetch_add(1, std::memory_order_relaxed);
                        }
                    };

                    cudaDiag.gpuAttempts++;
                    captureGpuAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
                    if (duplicationCapture->GetNextFrameGpu(capturedGpu, &gpuStatus, &accumulatedFrames, &ddaFrameInfo))
                    {
                        CountGpuCaptureStatus(cudaDiag, gpuStatus);
                        const uint64_t accumulated = accumulatedFrames;
                        const uint64_t missed = accumulated > 0 ? accumulated - 1 : 0;
                        cudaDiag.gpuAccumulatedFrames += accumulated;
                        cudaDiag.gpuMissedFrames += missed;
                        captureGpuAccumulatedFramesTotal.fetch_add(accumulated, std::memory_order_relaxed);
                        captureGpuMissedFramesTotal.fetch_add(missed, std::memory_order_relaxed);
                        countDdaFrameInfo(ddaFrameInfo);
                        frameTimestamp = std::chrono::steady_clock::now();
                        trt_detector.processFrameGpu(capturedGpu, frameTimestamp);
                        cudaDiag.gpuSubmitted++;
                        frameSubmittedToDetector = true;

                        if (needCpuCopyFromGpu)
                        {
                            capturedGpu.download(sourceCpu);
                            cudaDiag.gpuCpuCopies++;
                        }
                    }
                    else
                    {
                        CountGpuCaptureStatus(cudaDiag, gpuStatus);
                        if (gpuStatus == GpuCaptureStatus::NoPresent)
                        {
                            countDdaFrameInfo(ddaFrameInfo);
                            skipCpuFallbackThisFrame = true;
                            keepCaptureAliveThisFrame = true;
                        }
                        else if (gpuStatus == GpuCaptureStatus::Timeout)
                            skipCpuFallbackThisFrame = true;
                    }
                }
                else
                {
                    cudaDiag.gpuAttempts++;
                    CountGpuCaptureStatus(cudaDiag, GpuCaptureStatus::NotReady);
                }
            }
#endif

            if (!frameSubmittedToDetector)
            {
                if (skipCpuFallbackThisFrame)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (keepCaptureAliveThisFrame)
                    {
                        lastSuccessfulFrameTime = now;
                        publishCaptureSourceSize(capturer.get());
                        setCaptureAvailable();
                    }
                    else if (now - lastSuccessfulFrameTime >= staleFrameTimeout)
                        setCaptureUnavailable();

                    if (!frameDuration.has_value())
                        std::this_thread::yield();
#ifdef USE_CUDA
                    MaybeLogCudaCaptureDiagnostics(cudaDiag, currentCfg);
#endif
                    applyFrameLimiter();
                    continue;
                }

#ifdef USE_CUDA
                const bool cpuFallbackFromGpu = cudaDiag.lastPreferGpu;
                if (cpuFallbackFromGpu)
                {
                    cudaDiag.cpuFallbackAttempts++;
                    captureCpuFallbackAttemptsTotal.fetch_add(1, std::memory_order_relaxed);
                }
#endif
                sourceCpu = capturer->GetNextFrameCpu();
                frameTimestamp = std::chrono::steady_clock::now();

                if (sourceCpu.empty())
                {
#ifdef USE_CUDA
                    if (cpuFallbackFromGpu)
                        cudaDiag.cpuFallbackEmpty++;
#endif
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastSuccessfulFrameTime >= staleFrameTimeout)
                        setCaptureUnavailable();

                    if (!frameDuration.has_value())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#ifdef USE_CUDA
                    MaybeLogCudaCaptureDiagnostics(cudaDiag, currentCfg);
#endif
                    applyFrameLimiter();
                    continue;
                }
#ifdef USE_CUDA
                if (cpuFallbackFromGpu)
                {
                    cudaDiag.cpuFallbackFrames++;
                    captureCpuFallbackFramesTotal.fetch_add(1, std::memory_order_relaxed);
                }
                else
                    cudaDiag.cpuPathFrames++;
#endif

                if (NormalizeCaptureMethod(currentCfg.capture_method) == "virtual_camera")
                {
                    const int targetW = std::max(1, captureWidth);
                    const int targetH = std::max(1, captureHeight);
                    const int roiW = std::min(targetW, sourceCpu.cols);
                    const int roiH = std::min(targetH, sourceCpu.rows);

                    if (roiW <= 0 || roiH <= 0)
                    {
#ifdef USE_CUDA
                        MaybeLogCudaCaptureDiagnostics(cudaDiag, currentCfg);
#endif
                        applyFrameLimiter();
                        continue;
                    }

                    const int x = std::max(0, (sourceCpu.cols - roiW) / 2);
                    const int y = std::max(0, (sourceCpu.rows - roiH) / 2);
                    cv::Mat centered = sourceCpu(cv::Rect(x, y, roiW, roiH));

                    if (roiW != targetW || roiH != targetH)
                    {
                        cv::resize(centered, sourceCpu, cv::Size(targetW, targetH), 0, 0, cv::INTER_LINEAR);
                    }
                    else
                    {
                        sourceCpu = centered;
                    }
                }

                detectionFrame = sourceCpu;

#ifdef USE_CUDA
                trt_detector.processFrame(detectionFrame, sourceCpu, frameTimestamp);
                cudaDiag.trtCpuSubmitted++;
#else
                if (dml_detector)
                {
                    dml_detector->processFrame(detectionFrame, sourceCpu, frameTimestamp);
                }
#endif
            }

            if (frameSubmittedToDetector || !sourceCpu.empty())
            {
                lastSuccessfulFrameTime = std::chrono::steady_clock::now();
                publishCaptureSourceSize(capturer.get());
                setCaptureAvailable();
            }

            if (!sourceCpu.empty())
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                latestFrame = sourceCpu;
                if (frameQueue.size() >= 1)
                    frameQueue.pop_front();
                frameQueue.push_back(latestFrame);
            }
            frameCV.notify_one();

            captureFrameCount++;
            auto currentTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsedTime = currentTime - captureFpsStartTime;
            if (elapsedTime.count() >= 1.0)
            {
                captureFps = static_cast<int>(captureFrameCount / elapsedTime.count());
                captureFrameCount = 0;
                captureFpsStartTime = currentTime;
            }

#ifdef USE_CUDA
                MaybeLogCudaCaptureDiagnostics(cudaDiag, currentCfg);
#endif
                applyFrameLimiter();
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Capture] Loop exception: " << e.what() << std::endl;
                capturer.reset();
                publishCaptureSourceSize(nullptr);
                activeCapturerMethod.clear();
                winrtApartment.Ensure(false);
                setCaptureUnavailable();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            catch (...)
            {
                std::cerr << "[Capture] Loop exception: unknown." << std::endl;
                capturer.reset();
                publishCaptureSourceSize(nullptr);
                activeCapturerMethod.clear();
                winrtApartment.Ensure(false);
                setCaptureUnavailable();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Capture] Unhandled exception: " << e.what() << std::endl;
        throw;
    }
}
