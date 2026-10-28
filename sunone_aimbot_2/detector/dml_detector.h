#ifndef DIRECTML_DETECTOR_H
#define DIRECTML_DETECTOR_H

#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "postProcess.h"

class DirectMLDetector
{
public:
    DirectMLDetector(const std::string& model_path);
    ~DirectMLDetector();

    std::vector<Detection> detect(const cv::Mat& input_frame);
    std::vector<std::vector<Detection>> detectBatch(const std::vector<cv::Mat>& frames);

    void dmlInferenceThread();
    void processFrame(
        const cv::Mat& detection_frame,
        const cv::Mat& source_frame = cv::Mat(),
        std::chrono::steady_clock::time_point frameTimestamp = {});

    int getNumberOfClasses();
    bool isReady() const;

    std::chrono::duration<double, std::milli> lastInferenceTimeDML;
    std::chrono::duration<double, std::milli> lastPreprocessTimeDML;
    std::chrono::duration<double, std::milli> lastCopyTimeDML;
    std::chrono::duration<double, std::milli> lastPostprocessTimeDML;
    std::chrono::duration<double, std::milli> lastNmsTimeDML;

    std::condition_variable inferenceCV;
    std::atomic<bool> shouldExit = false;

private:
    Ort::Env env;
    Ort::Session session{ nullptr };
    Ort::AllocatorWithDefaultOptions allocator;
    std::atomic<bool> model_ready = false;
    bool using_directml_provider = false;

    std::string input_name;
    std::string output_name;
    std::vector<std::string> output_names;
    std::vector<const char*> output_name_ptrs;
    std::vector<int64_t> input_shape;
    bool sunpoint_raw_output = false;
    int heat_output_index = -1;
    int box_output_index = -1;
    int offset_output_index = -1;
    int model_input_h = -1;
    int model_input_w = -1;

    std::vector<float> input_tensor_values;
    cv::Mat preprocessBgrBuffer;
    cv::Mat preprocessResizeBuffer;
    cv::Mat preprocessFloatBuffer;
    cv::Mat preprocessGrayResizeBuffer;
    cv::Mat preprocessGrayFloatBuffer;

    std::mutex inferenceMutex;
    cv::Mat currentFrame;
    cv::Mat currentSourceFrame;
    std::chrono::steady_clock::time_point currentFrameTimestamp{};
    std::uint64_t currentFrameGeneration = 0;
    bool frameReady = false;

    bool initializeModel(const std::string& model_path);
    bool tryInitializeModel(
        const std::string& model_path,
        bool useDirectML,
        GraphOptimizationLevel graphOptimizationLevel,
        const char* providerLabel,
        std::string* error);
    Ort::SessionOptions createSessionOptions(
        bool useDirectML,
        GraphOptimizationLevel graphOptimizationLevel);
    void preprocessFrameToTensor(const cv::Mat& frame, float* dst, int target_w, int target_h);
    Ort::MemoryInfo memory_info;
};

#endif // DIRECTML_DETECTOR_H
