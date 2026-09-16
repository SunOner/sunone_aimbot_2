#ifndef MAKCU_CONNECTION_H
#define MAKCU_CONNECTION_H

#include <string>
#include <atomic>
#include <mutex>

#include "../modules/makcu/include/makcu.h"

class MakcuConnection
{
public:
    MakcuConnection(const std::string& port, unsigned int baud_rate);
    ~MakcuConnection();

    bool isOpen() const;

    void click(int button);
    void press(int button);
    void release(int button);
    void move(int x, int y);

    // Physical button states reported by the device (written by the SDK listener thread).
    // Which button means what is decided by the configured hotkeys in the adapter.
    std::atomic<bool> left_active;
    std::atomic<bool> right_active;
    std::atomic<bool> middle_active;
    std::atomic<bool> side1_active;
    std::atomic<bool> side2_active;

private:
    void onButtonCallback(makcu::MouseButton button, bool pressed);

private:
    makcu::Device device_;
    std::atomic<bool> is_open_;
    std::mutex write_mutex_;
};

#endif // MAKCU_CONNECTION_H