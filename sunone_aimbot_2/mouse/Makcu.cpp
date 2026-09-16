#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <windows.h>
#include <iostream>

#include "Makcu.h"

MakcuConnection::MakcuConnection(const std::string& port, unsigned int baud_rate)
    : is_open_(false)
    , left_active(false)
    , right_active(false)
    , middle_active(false)
    , side1_active(false)
    , side2_active(false)
{
    try
    {
        device_.setMouseButtonCallback([this](makcu::MouseButton button, bool pressed) {
            onButtonCallback(button, pressed);
        });

        device_.enableButtonMonitoring(true);

        if (device_.connect(port))
        {
            device_.enableHighPerformanceMode(true);

            constexpr unsigned int sdkHighSpeedBaud = 4000000;
            if (baud_rate > 0 && baud_rate != sdkHighSpeedBaud)
            {
                std::cout << "[Makcu] Ignoring configured baud rate " << baud_rate
                    << "; the MAKCU SDK connection is already running at "
                    << sdkHighSpeedBaud << " baud." << std::endl;
            }

            is_open_ = true;
            std::cout << "[Makcu] Connected! PORT: " << port << std::endl;
        }
        else
        {
            std::cerr << "[Makcu] Unable to connect to the port: " << port << std::endl;
        }
    }
    catch (const makcu::MakcuException& e)
    {
        std::cerr << "[Makcu] Error: " << e.what() << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Makcu] Error: " << e.what() << std::endl;
    }
}

MakcuConnection::~MakcuConnection()
{
    try
    {
        device_.disconnect();
    }
    catch (...)
    {
    }
    is_open_ = false;
}

bool MakcuConnection::isOpen() const
{
    return is_open_ && device_.isConnected();
}

void MakcuConnection::move(int x, int y)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.mouseMove(x, y);
    }
    catch (...)
    {
        is_open_ = false;
    }
}

void MakcuConnection::click(int button)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.click(makcu::MouseButton::LEFT);
    }
    catch (...)
    {
        is_open_ = false;
    }
}

void MakcuConnection::press(int button)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.mouseDown(makcu::MouseButton::LEFT);
    }
    catch (...)
    {
        is_open_ = false;
    }
}

void MakcuConnection::release(int button)
{
    if (!is_open_)
        return;

    std::lock_guard<std::mutex> lock(write_mutex_);
    try
    {
        device_.mouseUp(makcu::MouseButton::LEFT);
    }
    catch (...)
    {
        is_open_ = false;
    }
}

void MakcuConnection::onButtonCallback(makcu::MouseButton button, bool pressed)
{
    switch (button)
    {
    // Physical button states only; which button means what is decided by the
    // configured hotkeys (button_targeting / button_shoot / button_zoom).
    case makcu::MouseButton::LEFT:
        left_active = pressed;
        break;

    case makcu::MouseButton::RIGHT:
        right_active = pressed;
        break;

    case makcu::MouseButton::MIDDLE:
        middle_active = pressed;
        break;

    case makcu::MouseButton::SIDE1:
        side1_active = pressed;
        break;

    case makcu::MouseButton::SIDE2:
        side2_active = pressed;
        break;
    }
}
