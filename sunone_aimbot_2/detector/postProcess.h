#ifndef POSTPROCESS_H
#define POSTPROCESS_H

#include <chrono>
#include <cstdint>
#include <vector>
#include <opencv2/opencv.hpp>

struct Detection
{
    cv::Rect box;
    float confidence;
    int classId;
};

void NMS(
    std::vector<Detection>& detections,
    float nmsThreshold,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr
);

#ifdef USE_CUDA
std::vector<Detection> postProcessYolo(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    int maxDetections = 100,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr,
    float imageScale = 1.0f
);
#endif

#ifndef USE_CUDA
std::vector<Detection> postProcessYoloDML(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    int maxDetections = 100,
    std::chrono::duration<double, std::milli>* nmsTime = nullptr
);
#endif
#endif // POSTPROCESS_H
