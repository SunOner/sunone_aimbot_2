#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <optional>
#include <deque>
#include <random>
#include <array>
#include <cwchar>
#include <memory>

#include <opencv2/core/utils/logger.hpp>

#include "capture.h"
#include "mouse.h"
#include "sunone_aimbot_2.h"
#include "keyboard_listener.h"
#include "overlay.h"
#include "Game_overlay.h"
#include "ghub.h"
#include "other_tools.h"
#include "virtual_camera.h"
#include "mem/cpu_affinity_manager.h"
#include "runtime/thread_loops.h"
#include "benchmarks/provider_benchmark.h"

#ifdef USE_CUDA
#include "mem/gpu_resource_manager.h"
#include "depth/depth_anything_trt.h"
#include "depth/depth_mask.h"
#include "tensorrt/nvinf.h"
#endif

std::condition_variable frameCV;
std::atomic<bool> shouldExit(false);
std::atomic<bool> aiming(false);
std::mutex configMutex;
std::mutex inputDevicesMutex;

#ifdef USE_CUDA
TrtDetector trt_detector;
#else
DirectMLDetector* dml_detector = nullptr;
#endif

MouseThread* globalMouseThread = nullptr;
Config config;


GhubMouse* gHub = nullptr;
RzctlMouse* razerControl = nullptr;
Arduino* arduinoSerial = nullptr;
RP2350* rp2350Serial = nullptr;
KmboxNetConnection* kmboxNetSerial = nullptr;
KmboxAConnection* kmboxASerial = nullptr;
MakcuConnection* makcuSerial = nullptr;
std::unique_ptr<IMouseInput> activeMouseInputOwner;

std::atomic<bool> detection_resolution_changed(false);
std::atomic<bool> capture_method_changed(false);
std::atomic<bool> capture_cursor_changed(false);
std::atomic<bool> capture_borders_changed(false);
std::atomic<bool> capture_fps_changed(false);
std::atomic<bool> capture_window_changed(false);
std::atomic<bool> detector_model_changed(false);
std::atomic<bool> show_window_changed(false);
std::atomic<bool> input_method_changed(false);

std::atomic<bool> zooming(false);
std::atomic<bool> shooting(false);


std::string g_iconLastError;

static int FatalExit(const std::string& message)
{
    std::cerr << message << std::endl;
    std::cout << "Press Enter to exit...";
    std::cin.get();
    return -1;
}

static void SetWorkingDirectoryToExecutable()
{
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
    {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        std::error_code ec;
        std::filesystem::current_path(exeDir, ec);
        if (ec && config.verbose)
        {
            std::cout << "[Config] Failed to set working dir: " << exeDir.u8string()
                      << " (" << ec.message() << ")" << std::endl;
        }
    }
}

static bool SelectCompatibleAiModel()
{
    std::vector<std::string> availableModels = getAvailableModels();
    if (!config.ai_model.empty())
    {
        const std::string modelPath = "models/" + config.ai_model;
        if (!std::filesystem::exists(modelPath))
        {
            std::cerr << "[MAIN] Specified model does not exist: " << modelPath << std::endl;
        }
        else if (std::find(availableModels.begin(), availableModels.end(), config.ai_model) != availableModels.end())
        {
            return true;
        }
        else
        {
            std::cerr << "[MAIN] Specified model is not compatible with backend "
                      << config.backend << ": " << config.ai_model << std::endl;
        }
    }

    if (availableModels.empty())
    {
        std::cerr << "[MAIN] No compatible AI models found in 'models' directory for backend "
                  << config.backend << "." << std::endl;
        return false;
    }

    config.ai_model = availableModels[0];
    config.saveConfig("config.ini");
    std::cout << "[MAIN] Loaded first compatible " << config.backend
              << " model: " << config.ai_model << std::endl;
    return true;
}

static void HandleThreadCrash(const char* name, const std::exception* ex)
{
    std::cerr << "[Thread] " << name << " crashed: "
              << (ex ? ex->what() : "unknown exception") << std::endl;
    shouldExit = true;
    gameOverlayShouldExit.store(true);
#ifdef USE_CUDA
    trt_detector.requestStop();
#endif
    frameCV.notify_all();
    detectionBuffer.cv.notify_all();
}

template <typename Func>
static std::thread StartThreadGuarded(const char* name, Func func)
{
    return std::thread([name, func]() mutable {
        try
        {
            func();
        }
        catch (const std::exception& e)
        {
            HandleThreadCrash(name, &e);
        }
        catch (...)
        {
            HandleThreadCrash(name, nullptr);
        }
        });
}

void createInputDevices()
{
    if (globalMouseThread)
        globalMouseThread->setMouseInput(nullptr);

    std::unique_ptr<IMouseInput> oldMouseInputOwner;
    {
        std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
        oldMouseInputOwner = std::move(activeMouseInputOwner);
        arduinoSerial = nullptr;
        rp2350Serial = nullptr;
        gHub = nullptr;
        razerControl = nullptr;
        kmboxNetSerial = nullptr;
        kmboxASerial = nullptr;
        makcuSerial = nullptr;
    }
    oldMouseInputOwner.reset();

    Config cfgSnapshot;
    {
        std::lock_guard<std::mutex> cfgLock(configMutex);
        cfgSnapshot = config;
    }

    auto newMouseInputOwner = CreateMouseInputDevice(cfgSnapshot);
    IMouseInput* newMouseInput = newMouseInputOwner.get();

    Arduino* newArduinoSerial = newMouseInput ? newMouseInput->arduino() : nullptr;
    RP2350* newRp2350Serial = newMouseInput ? newMouseInput->rp2350() : nullptr;
    GhubMouse* newGHub = newMouseInput ? newMouseInput->ghub() : nullptr;
    RzctlMouse* newRazerControl = newMouseInput ? newMouseInput->razer() : nullptr;
    KmboxNetConnection* newKmboxNetSerial = newMouseInput ? newMouseInput->kmboxNet() : nullptr;
    KmboxAConnection* newKmboxASerial = newMouseInput ? newMouseInput->kmboxA() : nullptr;
    MakcuConnection* newMakcuSerial = newMouseInput ? newMouseInput->makcu() : nullptr;

    std::string message = std::string("[Mouse] Using ") + (newMouseInput ? newMouseInput->name() : "unknown") + " input.";
    if (!newMouseInput || !newMouseInput->isOpen())
        message += " Device not connected; input disabled until the selected method is available.";

    {
        std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
        activeMouseInputOwner = std::move(newMouseInputOwner);
        arduinoSerial = newArduinoSerial;
        rp2350Serial = newRp2350Serial;
        gHub = newGHub;
        razerControl = newRazerControl;
        kmboxNetSerial = newKmboxNetSerial;
        kmboxASerial = newKmboxASerial;
        makcuSerial = newMakcuSerial;
    }

    std::cout << message << std::endl;
}

void assignInputDevices()
{
    if (globalMouseThread)
    {
        globalMouseThread->setMouseInput(activeMouseInputOwner.get());
    }
}


int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetRandomConsoleTitle();
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_FATAL);
    SetWorkingDirectoryToExecutable();

    if (benchmarks::IsProviderBenchmarkRequested(argc, argv))
    {
        return benchmarks::RunProviderBenchmarkCli(argc, argv);
    }

    if (!config.loadConfig())
    {
        std::cerr << "[Config] Error with loading config!" << std::endl;
        std::cin.get();
        return -1;
    }

    CPUAffinityManager cpuManager;

    if (config.cpuCoreReserveCount > 0)
    {
        if (!cpuManager.reserveCPUCores(config.cpuCoreReserveCount))
            return FatalExit("[MAIN] Failed to reserve CPU cores.");
    }

    if (config.systemMemoryReserveMB > 0)
    {
        if (!cpuManager.reserveSystemMemory(config.systemMemoryReserveMB))
            return FatalExit("[MAIN] Failed to reserve system memory.");
    }

    try
    {
#ifdef USE_CUDA
        int cuda_runtime_version = 0;
        cudaError_t runtime_status = cudaRuntimeGetVersion(&cuda_runtime_version);

        if (runtime_status != cudaSuccess)
        {
            std::cerr << "[MAIN] CUDA runtime check failed: " << cudaGetErrorString(runtime_status) << std::endl;
            std::cin.get();
            return -1;
        }

        if (config.verbose)
            std::cout << "[CUDA] Version: " << cuda_runtime_version << std::endl;

        const int required_cuda_version = 13010;
        if (cuda_runtime_version < required_cuda_version)
        {
            int required_major = required_cuda_version / 1000;
            int required_minor = (required_cuda_version % 1000) / 10;
            int runtime_major = cuda_runtime_version / 1000;
            int runtime_minor = (cuda_runtime_version % 1000) / 10;
            std::cerr << "[MAIN] CUDA " << required_major << "." << required_minor
                << " required. Detected " << runtime_major << "." << runtime_minor << "." << std::endl;
            const wchar_t* title = L"CUDA Update Required";
            std::wstring message =
                L"An outdated CUDA version was detected. "
                L"Please update your graphics drivers to the latest version "
                L"and install CUDA 13.1.\n\n"
                L"The program will now attempt to continue.";
            MessageBoxW(nullptr, message.c_str(), title, MB_OK | MB_ICONWARNING);
        }

        GPUResourceManager gpuManager;
        if (config.gpuMemoryReserveMB > 0)
        {
            if (!gpuManager.reserveGPUMemory(config.gpuMemoryReserveMB))
                return FatalExit("[MAIN] Failed to reserve GPU memory.");
        }
        
        if (config.enableGpuExclusiveMode)
        {
            if (!gpuManager.setGPUExclusiveMode())
                return FatalExit("[MAIN] Failed to set GPU exclusive mode.");
        }

        int cuda_devices = 0;
        if (cudaGetDeviceCount(&cuda_devices) != cudaSuccess || cuda_devices == 0)
        {
            std::cerr << "[MAIN] CUDA required but no devices found." << std::endl;
            std::cin.get();
            return -1;
        }
#endif
        if (!CreateDirectory(L"models", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with models folder" << std::endl;
            std::cin.get();
            return -1;
        }
        if (!CreateDirectory(L"depth_models", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with depth_models folder" << std::endl;
            std::cin.get();
            return -1;
        }

        if (config.capture_method == "virtual_camera")
        {
            auto cams = VirtualCameraCapture::GetAvailableVirtualCameras(true);
            if (!cams.empty())
            {
                if (config.virtual_camera_name != "None" &&
                    std::find(cams.begin(), cams.end(), config.virtual_camera_name) == cams.end())
                {
                    config.virtual_camera_name = "None";
                    config.saveConfig("config.ini");
                    std::cout << "[MAIN] virtual_camera_name reset to None (auto-select)." << std::endl;
                }
                std::cout << "[MAIN] Virtual cameras loaded: " << cams.size() << std::endl;
            }
            else
            {
                std::cerr << "[MAIN] No virtual cameras found" << std::endl;
            }
        }

        if (!SelectCompatibleAiModel())
        {
            std::cin.get();
            return -1;
        }

        createInputDevices();

        MouseThread mouseThread(
            config.detection_resolution,
            config.fovX,
            config.fovY,
            config.minSpeedMultiplier,
            config.maxSpeedMultiplier,
            config.predictionInterval,
            config.auto_shoot,
            config.bScope_multiplier,
            activeMouseInputOwner.get()
        );

        globalMouseThread = &mouseThread;
        assignInputDevices();

#ifdef USE_CUDA
        trt_detector.initialize("models/" + config.ai_model);
#else
        std::thread dml_detThread;
        DirectMLDetector* newDmlDetector = nullptr;
        try
        {
            newDmlDetector = new DirectMLDetector("models/" + config.ai_model);
            dml_detector = newDmlDetector;
            std::cout << "[MAIN] DML detector created"
                      << (dml_detector->isReady() ? "." : " without an active model.") << std::endl;
            dml_detThread = StartThreadGuarded("DmlDetector", [] {
                dml_detector->dmlInferenceThread();
                });
        }
        catch (const std::exception& e)
        {
            if (dml_detector == newDmlDetector)
                dml_detector = nullptr;
            delete newDmlDetector;
            std::cerr << "[MAIN] DML detector is unavailable: " << e.what()
                      << ". The application will continue without DML inference." << std::endl;
        }
        catch (...)
        {
            if (dml_detector == newDmlDetector)
                dml_detector = nullptr;
            delete newDmlDetector;
            std::cerr << "[MAIN] DML detector is unavailable: unknown exception. "
                      << "The application will continue without DML inference." << std::endl;
        }
#endif

        detection_resolution_changed.store(true);

        std::thread keyThread = StartThreadGuarded("KeyboardListener", [] {
            keyboardListener();
            });
        std::thread capThread = StartThreadGuarded("CaptureThread", [] {
            captureThread(config.detection_resolution, config.detection_resolution);
            });

#ifdef USE_CUDA
        std::thread trt_detThread = StartThreadGuarded("TrtDetector", [] {
            trt_detector.inferenceThread();
            });
#endif
        std::thread mouseMovThread = StartThreadGuarded("MouseThread", [&mouseThread] {
            mouseThreadFunction(mouseThread);
            });
        std::thread overlayThread = StartThreadGuarded("OverlayThread", [] {
            OverlayThread();
            });

        gameOverlayShouldExit.store(false);
        gameOverlayThread = StartThreadGuarded("GameOverlay", [] {
            gameOverlayRenderLoop();
            });

        welcome_message();

        keyThread.join();
        capThread.join();
#ifdef USE_CUDA
        trt_detector.requestStop();
        trt_detThread.join();
#else
        if (dml_detThread.joinable())
        {
            if (dml_detector)
            {
                dml_detector->shouldExit = true;
                dml_detector->inferenceCV.notify_all();
            }
            dml_detThread.join();
        }
#endif

        gameOverlayShouldExit.store(true);
        if (gameOverlayThread.joinable()) gameOverlayThread.join();

        mouseMovThread.join();
        overlayThread.join();

        mouseThread.stop();
        globalMouseThread = nullptr;

        {
            std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
            activeMouseInputOwner.reset();
            arduinoSerial = nullptr;
            rp2350Serial = nullptr;
            gHub = nullptr;
            razerControl = nullptr;
            kmboxNetSerial = nullptr;
            kmboxASerial = nullptr;
            makcuSerial = nullptr;
        }

#ifndef USE_CUDA
        if (dml_detector)
        {
            delete dml_detector;
            dml_detector = nullptr;
        }
#endif

        if (gameOverlayPtr)
        {
            gameOverlayPtr->Stop();
            delete gameOverlayPtr;
            gameOverlayPtr = nullptr;
        }

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MAIN] An error has occurred in the main stream: " << e.what() << std::endl;
        std::cout << "Press Enter to exit...";
        std::cin.get();
        return -1;
    }
}
