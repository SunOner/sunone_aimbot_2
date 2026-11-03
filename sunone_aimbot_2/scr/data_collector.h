#pragma once

#include "config.h"

#include <opencv2/opencv.hpp>
#ifdef USE_CUDA
#include <opencv2/core/cuda.hpp>
#endif

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cvm {

struct DataCollectionUiState
{
    bool enabled = false;
    std::uint64_t observed_frame_count = 0;
    std::uint64_t attempted_sample_count = 0;
    std::uint64_t saved_image_count = 0;
    std::uint64_t saved_label_count = 0;
    std::string resolved_output_dir;
    std::string status;
};

std::filesystem::path ResolveCollectOutputDir(const std::string& root_dir, const char* output_dir_raw);
bool IsDataCollectionEnabled(const Config& cfg);
DataCollectionUiState GetDataCollectionUiState(const std::string& root_dir, const char* model_name, const Config& cfg);
DataCollectionUiState GetDataCollectionNodeUiState(int nodeKey);
void ResetDataCollectionRuntime();
void ResetDataCollectionNodeCounters(int nodeKey);
void RetainDataCollectionNodes(const std::vector<int>& activeKeys);
bool CollectDataSampleForNode(int nodeKey, const std::string& root_dir, const char* model_name,
    const cv::Mat& frame, const std::vector<cv::Rect>& boxes, const std::vector<int>& classes,
    const std::vector<float>& confidences, bool aimbot_enabled, const Config& settings,
    double cooldownMs = 500.0);

void MaybeCollectDataSample(const std::string& root_dir,
                            const char* model_name,
                            const cv::Mat& frame,
                            const std::vector<cv::Rect>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences,
                            bool aimbot_enabled,
                            const Config& cfg);

#ifdef USE_CUDA
void MaybeCollectDataSample(const std::string& root_dir,
                            const char* model_name,
                            const cv::cuda::GpuMat& frame,
                            const std::vector<cv::Rect>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences,
                            bool aimbot_enabled,
                            const Config& cfg);
#endif

}  // namespace cvm
