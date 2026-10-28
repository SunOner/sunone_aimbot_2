#include "mouse.h"
#include "capture.h"
#include "config.h"
#include "detection_buffer.h"
#include "runtime/input_state.h"
#include "runtime/thread_loops.h"

#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>

// Link the production mouse loop/worker without opening capture or real input.
Config config{};
std::mutex configMutex;
std::mutex inputDevicesMutex;
MouseThread* globalMouseThread = nullptr;
std::atomic<bool> shouldExit{ false };
std::atomic<bool> aiming{ false };
std::atomic<bool> shooting{ false };
std::atomic<bool> zooming{ false };
std::atomic<bool> input_method_changed{ false };
std::atomic<bool> detection_resolution_changed{ false };
std::atomic<int> captureFps{ 60 };
std::mutex g_trackerDebugMutex;
std::vector<TrackDebugInfo> g_trackerDebugTracks;
int g_trackerLockedId = -1;

void createInputDevices() { throw std::runtime_error("Unexpected real device creation"); }
void assignInputDevices() { throw std::runtime_error("Unexpected real device assignment"); }

namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct FakeState
{
    std::mutex mutex;
    std::condition_variable cv;
    int moves = 0;
    int downs = 0;
    int ups = 0;
    bool blockMoves = false;
    bool unblock = false;
};

class FakeInput final : public IMouseInput
{
public:
    explicit FakeInput(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
    const char* name() const override { return "TEST"; }
    bool isOpen() const override { return true; }
    bool move(int, int) override
    {
        std::unique_lock<std::mutex> lock(state_->mutex);
        ++state_->moves;
        state_->cv.notify_all();
        state_->cv.wait(lock, [&] { return !state_->blockMoves || state_->unblock; });
        return true;
    }
    bool leftDown() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->downs;
        state_->cv.notify_all();
        return true;
    }
    bool leftUp() override
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->ups;
        state_->cv.notify_all();
        return true;
    }

private:
    std::shared_ptr<FakeState> state_;
};

template <typename Predicate>
void waitFor(const std::shared_ptr<FakeState>& state, Predicate predicate, const char* message)
{
    std::unique_lock<std::mutex> lock(state->mutex);
    require(state->cv.wait_for(lock, 2s, predicate), message);
}

void unblock(const std::shared_ptr<FakeState>& state)
{
    std::lock_guard<std::mutex> lock(state->mutex);
    state->unblock = true;
    state->cv.notify_all();
}

struct UnblockOnExit
{
    std::shared_ptr<FakeState> state;
    ~UnblockOnExit() { unblock(state); }
};

struct MouseLoop
{
    std::thread thread;
    explicit MouseLoop(MouseThread& mouse)
        : thread([&mouse] { mouseThreadFunction(mouse); }) {}
    ~MouseLoop()
    {
        shouldExit = true;
        detectionBuffer.cv.notify_all();
        thread.join();
    }
};

void resetFixture()
{
    shouldExit = false;
    aiming = true;
    shooting = false;
    zooming = false;
    input_method_changed = false;
    detection_resolution_changed = false;
    if (detectionBuffer.isPaused())
        detectionBuffer.togglePaused();
    detectionBuffer.clear();

    config = Config{};
    config.detection_resolution = 320;
    config.fovX = 90;
    config.fovY = 90;
    config.minSpeedMultiplier = 1.0;
    config.maxSpeedMultiplier = 1.0;
    config.auto_shoot = true;
    config.bScope_multiplier = 1.0f;
    config.wind_mouse_enabled = false;
    config.kalman_enabled = false;
    config.kalman_reset_timeout_sec = 0.5f;
    config.speedCurveExponent = 1.0f;
    config.body_y_offset = 0.5f;
    config.head_y_offset = 0.5f;
    config.class_player = 0;
    config.class_head = 1;
    config.active_game = "TEST";
    config.game_profiles.emplace("TEST", Config::GameProfile{ "TEST", 1.0, 1.0, 1.0, false, 90.0 });
}

std::unique_ptr<MouseThread> makeMouse(IMouseInput* input)
{
    return std::make_unique<MouseThread>(320, 90, 90, 1.0, 1.0, 0.0, true, 1.0f, input);
}

void publishTarget()
{
    require(detectionBuffer.setForGeneration(
        detectionBuffer.currentGeneration(), { cv::Rect(140, 120, 40, 80) }, { 0 }, { 0.9f }, Clock::now()),
        "Fresh detection must be accepted");
}

void idleStopAndDeviceLifetime()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    auto device = std::make_unique<FakeInput>(state);
    // Repeatedly exercise the stop/wait boundary. CTest also bounds hangs.
    for (int i = 0; i < 200; ++i)
    {
        auto mouse = makeMouse(device.get());
        mouse->stop();
        mouse->stop();
    }
    auto mouse = makeMouse(device.get());
    mouse->pressMouse(AimbotTarget(140, 140, 40, 40, 0));
    mouse->stop();
    device.reset();
    mouse->moveRelative(1, 1);
    require(state->downs == 1 && state->ups == 1 && state->moves == 0,
        "Stop must release and detach input before its owner is destroyed");
}

void stopDiscardsBacklog()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    state->blockMoves = true;
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    UnblockOnExit cleanup{ state };
    mouse->setTargetDetected(true);
    mouse->moveMousePivot(220, 160, Clock::now());
    waitFor(state, [&] { return state->moves == 1; }, "Worker did not start the first move");
    for (int i = 0; i < 4; ++i)
        mouse->moveMousePivot(220, 160, Clock::now());
    mouse->requestStop();
    unblock(state);
    mouse->stop();
    require(state->moves == 1, "Stop must discard the backlog, not drain it");
}

void deviceSwitchReleasesOldButton()
{
    resetFixture();
    auto oldState = std::make_shared<FakeState>();
    auto newState = std::make_shared<FakeState>();
    FakeInput oldDevice(oldState);
    FakeInput newDevice(newState);
    auto mouse = makeMouse(&oldDevice);
    AimbotTarget target(140, 140, 40, 40, 0);
    mouse->pressMouse(target);
    mouse->setMouseInput(&newDevice);
    require(oldState->ups == 1, "Switch must release the old device");
    mouse->pressMouse(target);
    mouse->stop();
    require(newState->downs == 1 && newState->ups == 1, "Press ownership must reset for the new device");
}

void configIsAppliedOnlyByMouseLoop()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    Config snapshot = config;
    snapshot.detection_resolution = 640;
    snapshot.auto_shoot = false;
    mouse->pressMouse(AimbotTarget(140, 140, 40, 40, 0));
    std::thread ui([&] { mouse->updateConfig(snapshot); });
    ui.join();
    require(mouse->check_target_in_scope(140, 140, 40, 40, 1.0f),
        "UI update must not mutate active geometry");
    require(state->ups == 0, "UI update must not send input directly");
    require(mouse->applyPendingConfig(), "Owner must apply queued settings");
    require(!mouse->check_target_in_scope(140, 140, 40, 40, 1.0f) && state->ups == 1,
        "Applying settings must update geometry and release disabled auto-shoot");
    require(!mouse->applyPendingConfig(), "Settings must not be reapplied every iteration");
}

void autoShootHoldsThenReleasesWhenDisabled()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    MouseLoop loop(*mouse);
    publishTarget();
    waitFor(state, [&] { return state->downs == 1; }, "Auto-shoot did not press");
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        require(!state->cv.wait_for(lock, 10ms, [&] { return state->ups != 0; }),
            "Polling timeout must not release a still-current target");
    }
    {
        std::lock_guard<std::mutex> lock(configMutex);
        config.auto_shoot = false;
        mouse->updateConfig(config);
    }
    waitFor(state, [&] { return state->ups == 1; }, "Disabling auto-shoot must release its press");
}

void loopExitReleasesButton()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    {
        MouseLoop loop(*mouse);
        publishTarget();
        waitFor(state, [&] { return state->downs == 1; }, "Auto-shoot did not press before exit");
    }
    require(state->ups == 1, "Leaving the mouse loop must release its press");
    mouse->moveRelative(1, 1);
    require(state->moves == 0, "Stopped mouse loop must not dispatch more input");
}

void pauseRejectsLateResultsAndReleasesButton()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    MouseLoop loop(*mouse);
    publishTarget();
    waitFor(state, [&] { return state->downs == 1; }, "Auto-shoot did not press before pause");
    const auto oldGeneration = detectionBuffer.currentGeneration();
    const auto oldFrameTime = Clock::now();
    detectionBuffer.togglePaused();
    waitFor(state, [&] { return state->ups == 1; }, "Pause must release our press");
    require(!detectionBuffer.setForGeneration(oldGeneration, { cv::Rect(140, 120, 40, 80) }, { 0 }, {}, Clock::now()),
        "In-flight result must be rejected during pause");
    detectionBuffer.togglePaused();
    require(!detectionBuffer.setForGeneration(oldGeneration, { cv::Rect(140, 120, 40, 80) }, { 0 }, {}, Clock::now()),
        "In-flight result must remain invalid after resume");
    require(!detectionBuffer.setForGeneration(detectionBuffer.currentGeneration(),
        { cv::Rect(140, 120, 40, 80) }, { 0 }, {}, oldFrameTime),
        "A frame captured before pause must not be relabeled as fresh");
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        require(!state->cv.wait_for(lock, 10ms, [&] { return state->downs > 1; }),
            "Resume must not resurrect the previous target");
    }
    publishTarget();
    waitFor(state, [&] { return state->downs == 2; }, "Fresh detection must work after resume");
}

void pauseResumeInvalidatesQueuedMoves()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    state->blockMoves = true;
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    UnblockOnExit cleanup{ state };
    mouse->setTargetDetected(true);
    mouse->moveMousePivot(220, 160, Clock::now());
    waitFor(state, [&] { return state->moves == 1; }, "Worker did not start a move");
    for (int i = 0; i < 4; ++i)
        mouse->moveMousePivot(220, 160, Clock::now());
    {
        // Block the worker in post-send compensation, outside the pause gate,
        // so all queued moves deterministically survive until after resume.
        std::lock_guard<std::mutex> lock(configMutex);
        unblock(state);
        detectionBuffer.togglePaused();
        detectionBuffer.togglePaused();
    }
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        require(!state->cv.wait_for(lock, 10ms, [&] { return state->moves > 1; }),
            "Queued moves from before pause must not execute after resume");
    }
    mouse->stop();
}

void publicationDoesNotWaitForSlowInput()
{
    resetFixture();
    auto state = std::make_shared<FakeState>();
    state->blockMoves = true;
    FakeInput device(state);
    auto mouse = makeMouse(&device);
    UnblockOnExit cleanup{ state };
    const auto generation = detectionBuffer.currentGeneration();
    mouse->setTargetDetected(true);
    mouse->moveMousePivot(220, 160, Clock::now());
    waitFor(state, [&] { return state->moves == 1; }, "Worker did not start a move");

    std::promise<bool> published;
    auto completion = published.get_future();
    std::thread inference([&] {
        published.set_value(detectionBuffer.setForGeneration(generation, {}, {}, {}, Clock::now()));
    });
    const bool ready = completion.wait_for(2s) == std::future_status::ready;
    // Always unblock and join before checking, including on a regression.
    unblock(state);
    inference.join();
    require(ready && completion.get(), "Slow input must not block inference publication");
    mouse->stop();
}
}

int main()
{
    const std::pair<const char*, std::function<void()>> tests[] = {
        { "idle stop and device lifetime", idleStopAndDeviceLifetime },
        { "stop discards backlog", stopDiscardsBacklog },
        { "device switch releases button", deviceSwitchReleasesOldButton },
        { "config owner applies snapshot", configIsAppliedOnlyByMouseLoop },
        { "auto-shoot hold and disable", autoShootHoldsThenReleasesWhenDisabled },
        { "loop exit releases button", loopExitReleasesButton },
        { "pause rejects late inference", pauseRejectsLateResultsAndReleasesButton },
        { "pause/resume cancels queued moves", pauseResumeInvalidatesQueuedMoves },
        { "publication independent of slow input", publicationDoesNotWaitForSlowInput }
    };
    try
    {
        for (const auto& test : tests)
        {
            test.second();
            std::cout << "PASS: " << test.first << '\n';
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
