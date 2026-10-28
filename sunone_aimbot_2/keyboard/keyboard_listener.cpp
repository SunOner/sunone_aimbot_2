#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>
#include <mutex>

#include "config.h"
#include "keyboard_listener.h"
#include "mouse.h"
#include "keycodes.h"
#include "sunone_aimbot_2.h"
#include "capture.h"
#include "runtime/thread_loops.h"

extern std::atomic<bool> shouldExit;
extern std::atomic<bool> aiming;
extern std::atomic<bool> shooting;
extern std::atomic<bool> zooming;
extern std::atomic<bool> detector_model_changed;

extern MouseThread* globalMouseThread;

const float OFFSET_STEP = 0.01f;
const float NORECOIL_STEP = 5.0f;

// Arrow key vectors
const std::vector<std::string> upArrowKeys = { "UpArrow" };
const std::vector<std::string> downArrowKeys = { "DownArrow" };
const std::vector<std::string> leftArrowKeys = { "LeftArrow" };
const std::vector<std::string> rightArrowKeys = { "RightArrow" };
const std::vector<std::string> shiftKeys = { "LeftShift", "RightShift" };

// Previous key states
bool prevUpArrow = false;
bool prevDownArrow = false;
bool prevLeftArrow = false;
bool prevRightArrow = false;

namespace
{
struct KeyboardConfigSnapshot
{
    bool autoAim = false;
    bool enableArrowsSettings = false;
    std::vector<std::string> buttonTargeting;
    std::vector<std::string> buttonShoot;
    std::vector<std::string> buttonZoom;
    std::vector<std::string> buttonExit;
    std::vector<std::string> buttonPause;
    std::vector<std::string> buttonReloadConfig;
    std::vector<std::string> buttonOpenOverlay;
};

KeyboardConfigSnapshot SnapshotKeyboardConfig()
{
    std::lock_guard<std::mutex> lock(configMutex);
    KeyboardConfigSnapshot snapshot;
    snapshot.autoAim = config.auto_aim;
    snapshot.enableArrowsSettings = config.enable_arrows_settings;
    snapshot.buttonTargeting = config.button_targeting;
    snapshot.buttonShoot = config.button_shoot;
    snapshot.buttonZoom = config.button_zoom;
    snapshot.buttonExit = config.button_exit;
    snapshot.buttonPause = config.button_pause;
    snapshot.buttonReloadConfig = config.button_reload_config;
    snapshot.buttonOpenOverlay = config.button_open_overlay;
    return snapshot;
}

bool isAimingActiveFromDevices()
{
    std::lock_guard<std::mutex> lock(inputDevicesMutex);
    return activeMouseInputOwner && activeMouseInputOwner->isOpen() && activeMouseInputOwner->aimingActive();
}

bool isShootingActiveFromDevices()
{
    std::lock_guard<std::mutex> lock(inputDevicesMutex);
    return activeMouseInputOwner && activeMouseInputOwner->isOpen() && activeMouseInputOwner->shootingActive();
}

bool isZoomingActiveFromDevices()
{
    std::lock_guard<std::mutex> lock(inputDevicesMutex);
    return activeMouseInputOwner && activeMouseInputOwner->isOpen() && activeMouseInputOwner->zoomingActive();
}

bool isAnyKeyPressedInternal(const std::vector<std::string>& keys)
{
    bool usePhysicalDevice = false;

    std::lock_guard<std::mutex> lock(inputDevicesMutex);

    IMouseInput* input = activeMouseInputOwner.get();
    if (input && input->isOpen() && input->hasPhysicalButtonState())
        usePhysicalDevice = true;

    for (const auto& key_name : keys)
    {
        int key_code = KeyCodes::getKeyCode(key_name);
        bool pressed = false;

        if (input && input->isOpen())
            pressed = input->keyPressed(key_name);

        // Win32 API
        if (!pressed && key_code != -1)
        {
            bool isMouse = (key_name == "LeftMouseButton" ||
                key_name == "RightMouseButton" ||
                key_name == "MiddleMouseButton" ||
                key_name == "X1MouseButton" ||
                key_name == "X2MouseButton");

            if (!isMouse || !usePhysicalDevice)
            {
                pressed = (GetAsyncKeyState(key_code) & 0x8000) != 0;
            }
        }

        if (pressed) return true;
    }
    return false;
}
} // namespace

bool isAnyKeyPressed(const std::vector<std::string>& keys)
{
    return isAnyKeyPressedInternal(keys);
}

bool isAnyKeyPressedWin32Only(const std::vector<std::string>& keys)
{
    for (const auto& key_name : keys)
    {
        int key_code = KeyCodes::getKeyCode(key_name);
        if (key_code != -1 && (GetAsyncKeyState(key_code) & 0x8000))
            return true;
    }
    return false;
}

void keyboardListener()
{
    while (!shouldExit)
    {
        KeyboardConfigSnapshot cfg = SnapshotKeyboardConfig();

        // Aiming
        if (!cfg.autoAim)
        {
            aiming = isAnyKeyPressedInternal(cfg.buttonTargeting) ||
                isAimingActiveFromDevices();
        }
        else
        {
            aiming = true;
        }

        // Shooting
        shooting = isAnyKeyPressedInternal(cfg.buttonShoot) ||
            isShootingActiveFromDevices();

        // Zooming
        zooming = isAnyKeyPressedInternal(cfg.buttonZoom) ||
            isZoomingActiveFromDevices();

        // Exit - Win32
        if (isAnyKeyPressedWin32Only(cfg.buttonExit))
        {
            shouldExit = true;
            gameOverlayShouldExit.store(true);
            detectionBuffer.cv.notify_all();
            frameCV.notify_all();
        }

        // Pause detection - Win32
        static bool pausePressed = false;
        if (isAnyKeyPressedWin32Only(cfg.buttonPause))
        {
            if (!pausePressed)
            {
                detectionBuffer.togglePaused();
                pausePressed = true;
            }
        }
        else
        {
            pausePressed = false;
        }

        // Reload config - Win32
        static bool reloadPressed = false;
        if (isAnyKeyPressedWin32Only(cfg.buttonReloadConfig))
        {
            if (!reloadPressed)
            {
                {
                    std::lock_guard<std::mutex> lock(configMutex);
                    const int oldDetectionResolution = config.detection_resolution;
                    const int oldCaptureFps = config.capture_fps;
                    const std::string oldCaptureMethod = config.capture_method;
                    const std::string oldCaptureTarget = config.capture_target;
                    const std::string oldCaptureWindowTitle = config.capture_window_title;
                    const int oldMonitorIdx = config.monitor_idx;
                    const bool oldCaptureBorders = config.capture_borders;
                    const bool oldCaptureCursor = config.capture_cursor;
                    const std::string oldVirtualCameraName = config.virtual_camera_name;
                    const int oldVirtualCameraWidth = config.virtual_camera_width;
                    const int oldVirtualCameraHeight = config.virtual_camera_heigth;
                    const std::string oldUdpIp = config.udp_ip;
                    const int oldUdpPort = config.udp_port;
                    const std::string oldBackend = config.backend;
                    const std::string oldAiModel = config.ai_model;
                    const std::string oldInputMethod = config.input_method;

                    config.loadConfig();

                    if (config.detection_resolution != oldDetectionResolution)
                    {
                        detection_resolution_changed.store(true);
                        detector_model_changed.store(true);
                    }

                    if (config.capture_fps != oldCaptureFps)
                        capture_fps_changed.store(true);

                    if (config.capture_method != oldCaptureMethod ||
                        config.capture_target != oldCaptureTarget ||
                        config.capture_window_title != oldCaptureWindowTitle ||
                        config.monitor_idx != oldMonitorIdx ||
                        config.virtual_camera_name != oldVirtualCameraName ||
                        config.virtual_camera_width != oldVirtualCameraWidth ||
                        config.virtual_camera_heigth != oldVirtualCameraHeight ||
                        config.udp_ip != oldUdpIp ||
                        config.udp_port != oldUdpPort)
                    {
                        capture_method_changed.store(true);
                        capture_window_changed.store(true);
                    }

                    if (config.capture_borders != oldCaptureBorders)
                        capture_borders_changed.store(true);

                    if (config.capture_cursor != oldCaptureCursor)
                        capture_cursor_changed.store(true);

                    if (config.backend != oldBackend || config.ai_model != oldAiModel)
                        detector_model_changed.store(true);

                    if (config.input_method != oldInputMethod)
                        input_method_changed.store(true);

                    if (globalMouseThread)
                    {
                        globalMouseThread->updateConfig(config);
                    }
                }
                reloadPressed = true;
            }
        }
        else
        {
            reloadPressed = false;
        }

        // Open overlay - Win32
        static bool overlayPressed = false;
        if (isAnyKeyPressedWin32Only(cfg.buttonOpenOverlay))
        {
            if (!overlayPressed)
            {
                overlayPressed = true;
            }
        }
        else
        {
            overlayPressed = false;
        }

        // Arrow key detection - Win32
        bool upArrow = isAnyKeyPressedWin32Only(upArrowKeys);
        bool downArrow = isAnyKeyPressedWin32Only(downArrowKeys);
        bool leftArrow = isAnyKeyPressedWin32Only(leftArrowKeys);
        bool rightArrow = isAnyKeyPressedWin32Only(rightArrowKeys);
        bool shiftKey = isAnyKeyPressedWin32Only(shiftKeys);

        // Adjust offsets based on arrow keys and shift combination
        if (cfg.enableArrowsSettings)
        {
            if (upArrow && !prevUpArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                if (shiftKey)
                {
                    // Shift + Up Arrow: Decrease head offset
                    config.head_y_offset = std::max(0.0f, config.head_y_offset - OFFSET_STEP);
                }
                else
                {
                    // Up Arrow: Decrease body offset
                    config.body_y_offset = std::max(0.0f, config.body_y_offset - OFFSET_STEP);
                }
            }
            if (downArrow && !prevDownArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                if (shiftKey)
                {
                    // Shift + Down Arrow: Increase head offset
                    config.head_y_offset = std::min(1.0f, config.head_y_offset + OFFSET_STEP);
                }
                else
                {
                    // Down Arrow: Increase body offset
                    config.body_y_offset = std::min(1.0f, config.body_y_offset + OFFSET_STEP);
                }
            }


            // Adjust norecoil strength based on left and right arrow keys
            if (leftArrow && !prevLeftArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                config.easynorecoilstrength = std::max(0.1f, config.easynorecoilstrength - NORECOIL_STEP);
            }

            if (rightArrow && !prevRightArrow)
            {
                std::lock_guard<std::mutex> lock(configMutex);
                config.easynorecoilstrength = std::min(500.0f, config.easynorecoilstrength + NORECOIL_STEP);
            }
        }
        
        // Update previous key states
        prevUpArrow = upArrow;
        prevDownArrow = downArrow;
        prevLeftArrow = leftArrow;
        prevRightArrow = rightArrow;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
