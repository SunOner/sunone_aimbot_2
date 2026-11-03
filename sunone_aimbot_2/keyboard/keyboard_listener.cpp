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
#include "runtime/input_state.h"
#include "nodes/node_graph.h"
#include "nodes/vision_runtime.h"
#include "overlay/node_editor.h"

extern std::atomic<bool> shouldExit;
extern std::atomic<bool> aiming;
extern std::atomic<bool> shooting;
extern std::atomic<bool> zooming;
extern std::atomic<bool> detector_model_changed;

extern MouseThread* globalMouseThread;

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

        // Publish physical inputs. Activation policy belongs to the graphs.
        const bool operationalInput = !editorInputCaptured.load();
        aiming = operationalInput && (isAnyKeyPressedInternal(cfg.buttonTargeting) ||
            isAimingActiveFromDevices());

        // Shooting
        shooting = operationalInput && (isAnyKeyPressedInternal(cfg.buttonShoot) ||
            isShootingActiveFromDevices());

        // Zooming
        zooming = operationalInput && (isAnyKeyPressedInternal(cfg.buttonZoom) ||
            isZoomingActiveFromDevices());

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
                bool applicationLoaded=false;
                {
                    std::lock_guard<std::mutex> lock(configMutex);
                    const Config previous=config;applicationLoaded=config.reloadApplicationSettings();
                    if(applicationLoaded&&!config.sameInputDeviceSettings(previous))input_method_changed=true;
                }
                NodeEditor_RequestReload(applicationLoaded?"":"Could not reload application settings from config.ini.");
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
        if (cfg.enableArrowsSettings && operationalInput && !editorInputCaptured.load())
        {
            const int offsetDirection=(downArrow&&!prevDownArrow)-(upArrow&&!prevUpArrow);
            const int recoilDirection=(rightArrow&&!prevRightArrow)-(leftArrow&&!prevLeftArrow);
            if(offsetDirection||recoilDirection)NodeEditor_RequestTuning(offsetDirection,recoilDirection,shiftKey);
        }
        
        // Update previous key states
        prevUpArrow = upArrow;
        prevDownArrow = downArrow;
        prevLeftArrow = leftArrow;
        prevRightArrow = rightArrow;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
