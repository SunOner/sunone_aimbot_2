#ifdef USE_CUDA
#pragma once
#include <atomic>
#include <string>
#include <mutex>
#include <map>
#include <chrono>

struct ProgressPhase {
    std::string name;
    int current = 0;
    int max = 0;
};

inline std::mutex gProgressMutex;
inline std::recursive_mutex gTrtBuildMutex;
inline thread_local const std::atomic<bool>* gTrtCancellationToken=nullptr;
inline std::map<std::string, ProgressPhase> gProgressPhases;
inline std::atomic<bool> gIsTrtExporting = false;
inline std::atomic<bool> gTrtExportCancelRequested = false;
inline std::atomic<long long> gTrtExportLastUpdateMs = 0;

inline long long TrtNowMs() noexcept
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline void TrtExportResetState() noexcept
{
    gTrtExportCancelRequested = gTrtCancellationToken&&gTrtCancellationToken->load();
    gTrtExportLastUpdateMs = TrtNowMs();
}

class ImGuiProgressMonitor : public nvinfer1::IProgressMonitor {
public:
    void phaseStart(const char* phaseName, const char* parentPhase, int32_t nbSteps) noexcept override {
        std::lock_guard<std::mutex> lock(gProgressMutex);
        ProgressPhase phase;
        phase.name = phaseName;
        phase.current = 0;
        phase.max = nbSteps;
        gProgressPhases[phaseName] = phase;
        gIsTrtExporting = true;
        gTrtExportLastUpdateMs = TrtNowMs();
    }

    bool stepComplete(const char* phaseName, int32_t step) noexcept override {
        std::lock_guard<std::mutex> lock(gProgressMutex);
        gProgressPhases[phaseName].current = step;
        gTrtExportLastUpdateMs = TrtNowMs();
        return !gTrtExportCancelRequested.load()&&!(gTrtCancellationToken&&gTrtCancellationToken->load());
    }

    void phaseFinish(const char* phaseName) noexcept override {
        std::lock_guard<std::mutex> lock(gProgressMutex);
        gProgressPhases.erase(phaseName);
        if (gProgressPhases.empty())
            gIsTrtExporting = false;
        gTrtExportLastUpdateMs = TrtNowMs();
    }
};
#endif
