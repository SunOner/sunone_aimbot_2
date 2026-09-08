#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include "MouseInput.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "Arduino.h"
#include "KmboxAConnection.h"
#include "KmboxNetConnection.h"
#include "Makcu.h"
#include "MediusRuntime.h"
#include "RP2350.h"
#include "Teensy41RawHid.h"
#include "config.h"
#include "ghub.h"
#include "rzctl.h"

namespace
{
bool logicalButtonPressed(
    const std::string& keyName,
    bool shootingActive,
    bool zoomingActive,
    bool aimingActive)
{
    if (keyName == "LeftMouseButton")
        return shootingActive;
    if (keyName == "RightMouseButton")
        return zoomingActive;
    if (keyName == "X2MouseButton")
        return aimingActive;
    return false;
}

bool sendWin32Move(int dx, int dy)
{
    INPUT input{ 0 };
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_VIRTUALDESK;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool sendWin32Click(DWORD flag)
{
    INPUT input{ 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flag;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

class Win32MouseInput final : public IMouseInput
{
public:
    const char* name() const override { return "WIN32"; }
    bool isOpen() const override { return true; }
    bool move(int dx, int dy) override { return sendWin32Move(dx, dy); }
    bool leftDown() override { return sendWin32Click(MOUSEEVENTF_LEFTDOWN); }
    bool leftUp() override { return sendWin32Click(MOUSEEVENTF_LEFTUP); }
};

class ArduinoMouseInput final : public IMouseInput
{
public:
    ArduinoMouseInput(
        const std::string& port,
        unsigned int baudrate,
        bool useButtonState,
        ArduinoProtocol protocol = ArduinoProtocol::Legacy)
        : device_(std::make_unique<Arduino>(port, baudrate, protocol)),
          useButtonState_(useButtonState)
    {
    }

    const char* name() const override { return "ARDUINO"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        device_->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        device_->press();
        return true;
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        device_->release();
        return true;
    }
    bool hasPhysicalButtonState() const override { return useButtonState_; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() && useButtonState_ &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return useButtonState_ && device_ && device_->aiming_active; }
    bool shootingActive() const override { return useButtonState_ && device_ && device_->shooting_active; }
    bool zoomingActive() const override { return useButtonState_ && device_ && device_->zooming_active; }
    Arduino* arduino() override { return device_.get(); }

private:
    std::unique_ptr<Arduino> device_;
    bool useButtonState_ = false;
};

class RP2350MouseInput final : public IMouseInput
{
public:
    RP2350MouseInput(const std::string& port, unsigned int baudrate, bool useButtonState)
        : device_(std::make_unique<RP2350>(port, baudrate)),
          useButtonState_(useButtonState)
    {
    }

    const char* name() const override { return "RP2350"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        device_->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        device_->press();
        return true;
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        device_->release();
        return true;
    }
    bool hasPhysicalButtonState() const override { return useButtonState_; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() && useButtonState_ &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return useButtonState_ && device_ && device_->aiming_active.load(); }
    bool shootingActive() const override { return useButtonState_ && device_ && device_->shooting_active.load(); }
    bool zoomingActive() const override { return useButtonState_ && device_ && device_->zooming_active.load(); }
    RP2350* rp2350() override { return device_.get(); }

private:
    std::unique_ptr<RP2350> device_;
    bool useButtonState_ = false;
};

class Teensy41MouseInput final : public IMouseInput
{
public:
    Teensy41MouseInput(const std::string& port, unsigned int baudrate)
        : device_(std::make_unique<Arduino>(port, baudrate, ArduinoProtocol::Teensy41))
    {
    }

    const char* name() const override { return "TEENSY41"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        device_->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        device_->press();
        return true;
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        device_->release();
        return true;
    }
    bool hasPhysicalButtonState() const override { return true; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return device_ && device_->aiming_active; }
    bool shootingActive() const override { return device_ && device_->shooting_active; }
    bool zoomingActive() const override { return device_ && device_->zooming_active; }
    Arduino* arduino() override { return device_.get(); }

private:
    std::unique_ptr<Arduino> device_;
};

class Teensy41RawHidMouseInput final : public IMouseInput
{
public:
    explicit Teensy41RawHidMouseInput(const Config& config)
        : device_(std::make_unique<Teensy41RawHid>(config))
    {
    }

    const char* name() const override { return "TEENSY41_HID"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        return device_->move(dx, dy);
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        return device_->press();
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        return device_->release();
    }
    bool hasPhysicalButtonState() const override { return true; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return device_ && device_->aimingActive(); }
    bool shootingActive() const override { return device_ && device_->shootingActive(); }
    bool zoomingActive() const override { return device_ && device_->zoomingActive(); }
    Teensy41RawHid* teensy41RawHid() override { return device_.get(); }

private:
    std::unique_ptr<Teensy41RawHid> device_;
};

class GHubMouseInput final : public IMouseInput
{
public:
    GHubMouseInput()
        : device_(std::make_unique<GhubMouse>())
    {
        open_ = device_ && device_->mouse_xy(0, 0);
    }

    ~GHubMouseInput() override
    {
        if (device_)
            device_->mouse_close();
    }

    const char* name() const override { return "GHUB"; }
    bool isOpen() const override { return device_ && open_; }
    bool move(int dx, int dy) override { return isOpen() && device_->mouse_xy(dx, dy); }
    bool leftDown() override { return isOpen() && device_->mouse_down(); }
    bool leftUp() override { return isOpen() && device_->mouse_up(); }
    GhubMouse* ghub() override { return device_.get(); }

private:
    std::unique_ptr<GhubMouse> device_;
    bool open_ = false;
};

class RazerMouseInput final : public IMouseInput
{
public:
    RazerMouseInput()
        : device_(std::make_unique<RzctlMouse>())
    {
    }

    ~RazerMouseInput() override
    {
        if (device_)
            device_->mouse_close();
    }

    const char* name() const override { return "RAZER"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override { return isOpen() && device_->mouse_xy(dx, dy); }
    bool leftDown() override { return isOpen() && device_->mouse_down(); }
    bool leftUp() override { return isOpen() && device_->mouse_up(); }
    RzctlMouse* razer() override { return device_.get(); }

private:
    std::unique_ptr<RzctlMouse> device_;
};

class KmboxNetMouseInput final : public IMouseInput
{
public:
    KmboxNetMouseInput(const std::string& ip, const std::string& port, const std::string& uuid)
        : state_(std::make_shared<State>())
    {
        state_->connecting.store(true);
        std::thread([state = state_, ip, port, uuid] {
            auto device = std::make_unique<KmboxNetConnection>(ip, port, uuid);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->device = std::move(device);
            }
            state->connecting.store(false);
            }).detach();
    }

    ~KmboxNetMouseInput() override
    {
        if (!state_)
            return;

        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->device.reset();
        state_->connecting.store(false);
    }

    const char* name() const override { return "KMBOX_NET"; }
    bool isOpen() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->isOpen();
    }
    bool move(int dx, int dy) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        state_->device->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        state_->device->leftDown();
        return true;
    }
    bool leftUp() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->device || !state_->device->isOpen())
            return false;
        state_->device->leftUp();
        return true;
    }
    bool hasPhysicalButtonState() const override { return true; }
    bool keyPressed(const std::string& keyName) override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        KmboxNetConnection* device = state_->device.get();
        if (!device || !device->isOpen())
            return false;

        if (keyName == "LeftMouseButton" && device->monitorMouseLeft() == 1)
            return true;
        if (keyName == "RightMouseButton" && device->monitorMouseRight() == 1)
            return true;
        if (keyName == "MiddleMouseButton" && device->monitorMouseMiddle() == 1)
            return true;
        if (keyName == "X1MouseButton" && device->monitorMouseSide1() == 1)
            return true;
        if (keyName == "X2MouseButton" && device->monitorMouseSide2() == 1)
            return true;

        return logicalButtonPressed(
            keyName,
            device->shooting_active.load(),
            device->zooming_active.load(),
            device->aiming_active.load());
    }
    bool aimingActive() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->aiming_active.load();
    }
    bool shootingActive() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->shooting_active.load();
    }
    bool zoomingActive() const override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device && state_->device->zooming_active.load();
    }
    KmboxNetConnection* kmboxNet() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->device.get();
    }

private:
    struct State
    {
        mutable std::mutex mutex;
        std::unique_ptr<KmboxNetConnection> device;
        std::atomic<bool> connecting{ false };
    };

    std::shared_ptr<State> state_;
};

class KmboxAMouseInput final : public IMouseInput
{
public:
    explicit KmboxAMouseInput(const std::string& pidvid)
        : device_(std::make_unique<KmboxAConnection>(pidvid))
    {
    }

    const char* name() const override { return "KMBOX_A"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        device_->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        device_->leftDown();
        return true;
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        device_->leftUp();
        return true;
    }
    KmboxAConnection* kmboxA() override { return device_.get(); }

private:
    std::unique_ptr<KmboxAConnection> device_;
};

class MakcuMouseInput final : public IMouseInput
{
public:
    MakcuMouseInput(const std::string& port, unsigned int baudrate)
        : device_(std::make_unique<MakcuConnection>(port, baudrate))
    {
    }

    const char* name() const override { return "MAKCU"; }
    bool isOpen() const override { return device_ && device_->isOpen(); }
    bool move(int dx, int dy) override
    {
        if (!isOpen())
            return false;
        device_->move(dx, dy);
        return true;
    }
    bool leftDown() override
    {
        if (!isOpen())
            return false;
        device_->press(0);
        return true;
    }
    bool leftUp() override
    {
        if (!isOpen())
            return false;
        device_->release(0);
        return true;
    }
    bool hasPhysicalButtonState() const override { return true; }
    bool keyPressed(const std::string& keyName) override
    {
        return isOpen() &&
            logicalButtonPressed(keyName, shootingActive(), zoomingActive(), aimingActive());
    }
    bool aimingActive() const override { return device_ && device_->aiming_active; }
    bool shootingActive() const override { return device_ && device_->shooting_active; }
    bool zoomingActive() const override { return device_ && device_->zooming_active; }
    MakcuConnection* makcu() override { return device_.get(); }

private:
    std::unique_ptr<MakcuConnection> device_;
};

class MediusMouseInput final : public IMouseInput
{
public:
    MediusMouseInput()
        : runtime_(MediusRuntime::instance()), connected_(runtime_.connectMouse())
    {
    }

    ~MediusMouseInput() override
    {
        if (connected_)
            runtime_.disconnectMouse();
    }

    const char* name() const override { return "MEDIUS"; }
    bool isOpen() const override { return connected_ && runtime_.status().deviceConnected; }
    bool move(int dx, int dy) override { return connected_ && runtime_.move(dx, dy); }
    bool leftDown() override { return connected_ && runtime_.leftDown(); }
    bool leftUp() override { return connected_ && runtime_.leftUp(); }

private:
    MediusRuntime& runtime_;
    bool connected_ = false;
};
}

std::optional<MouseInputMethod> ParseMouseInputMethod(const std::string& method)
{
    if (method == "WIN32")
        return MouseInputMethod::Win32;
    if (method == "GHUB")
        return MouseInputMethod::GHub;
    if (method == "RAZER")
        return MouseInputMethod::Razer;
    if (method == "ARDUINO")
        return MouseInputMethod::Arduino;
    if (method == "RP2350")
        return MouseInputMethod::RP2350;
    if (method == "TEENSY41")
        return MouseInputMethod::Teensy41;
    if (method == "TEENSY41_HID")
        return MouseInputMethod::Teensy41Hid;
    if (method == "KMBOX_NET")
        return MouseInputMethod::KmboxNet;
    if (method == "KMBOX_A")
        return MouseInputMethod::KmboxA;
    if (method == "MAKCU")
        return MouseInputMethod::Makcu;
    if (method == "MEDIUS")
        return MouseInputMethod::Medius;
    return std::nullopt;
}

std::string MouseInputMethodName(MouseInputMethod method)
{
    switch (method)
    {
    case MouseInputMethod::GHub: return "GHUB";
    case MouseInputMethod::Razer: return "RAZER";
    case MouseInputMethod::Arduino: return "ARDUINO";
    case MouseInputMethod::RP2350: return "RP2350";
    case MouseInputMethod::Teensy41: return "TEENSY41";
    case MouseInputMethod::Teensy41Hid: return "TEENSY41_HID";
    case MouseInputMethod::KmboxNet: return "KMBOX_NET";
    case MouseInputMethod::KmboxA: return "KMBOX_A";
    case MouseInputMethod::Makcu: return "MAKCU";
    case MouseInputMethod::Medius: return "MEDIUS";
    case MouseInputMethod::Win32:
    default:
        return "WIN32";
    }
}

std::unique_ptr<IMouseInput> CreateMouseInputDevice(const Config& config)
{
    const MouseInputMethod method = ParseMouseInputMethod(config.input_method).value_or(MouseInputMethod::Win32);
    switch (method)
    {
    case MouseInputMethod::Arduino:
        return std::make_unique<ArduinoMouseInput>(
            config.arduino_port,
            static_cast<unsigned int>(config.arduino_baudrate),
            config.arduino_enable_keys);
    case MouseInputMethod::RP2350:
        return std::make_unique<RP2350MouseInput>(
            config.rp2350_port,
            static_cast<unsigned int>(config.rp2350_baudrate),
            config.rp2350_enable_keys);
    case MouseInputMethod::Teensy41:
        return std::make_unique<Teensy41MouseInput>(config.arduino_port, static_cast<unsigned int>(config.arduino_baudrate));
    case MouseInputMethod::Teensy41Hid:
        return std::make_unique<Teensy41RawHidMouseInput>(config);
    case MouseInputMethod::GHub:
        return std::make_unique<GHubMouseInput>();
    case MouseInputMethod::Razer:
        return std::make_unique<RazerMouseInput>();
    case MouseInputMethod::KmboxNet:
        return std::make_unique<KmboxNetMouseInput>(config.kmbox_net_ip, config.kmbox_net_port, config.kmbox_net_uuid);
    case MouseInputMethod::KmboxA:
        return std::make_unique<KmboxAMouseInput>(config.kmbox_a_pidvid);
    case MouseInputMethod::Makcu:
        return std::make_unique<MakcuMouseInput>(config.makcu_port, static_cast<unsigned int>(config.makcu_baudrate));
    case MouseInputMethod::Medius:
        return std::make_unique<MediusMouseInput>();
    case MouseInputMethod::Win32:
    default:
        return std::make_unique<Win32MouseInput>();
    }
}