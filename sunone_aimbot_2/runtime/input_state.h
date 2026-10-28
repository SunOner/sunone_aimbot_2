#pragma once

#include <atomic>
#include <mutex>

class Config;
class MouseThread;
struct DetectionBuffer;

extern Config config;
extern std::mutex configMutex;
extern std::mutex inputDevicesMutex;
extern DetectionBuffer detectionBuffer;
extern MouseThread* globalMouseThread;
extern std::atomic<bool> aiming;
extern std::atomic<bool> shooting;
extern std::atomic<bool> zooming;
extern std::atomic<bool> input_method_changed;
