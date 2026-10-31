#pragma once
#include "nodes/node_graph.h"
#include <opencv2/core.hpp>
#ifdef USE_CUDA
#include <opencv2/core/cuda.hpp>
#endif
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace NodeVision
{
struct FrameStorage
{
    mutable std::mutex cpuMutex;
    mutable cv::Mat image;
    cv::Mat source;
#ifdef USE_CUDA
    cv::cuda::GpuMat gpu;
#endif
};
struct MaskStorage { cv::Mat mask; };
using Infer = std::function<NodeGraph::DetectionStream(int,const NodeGraph::FrameStream&,const NodeGraph::Node&)>;

// Snapshot provider-owned buffers before they can be reused by capture.
NodeGraph::FrameStream makeFrame(const cv::Mat& image,const cv::Mat& source,
    std::chrono::steady_clock::time_point timestamp,std::uint64_t generation);
#ifdef USE_CUDA
NodeGraph::FrameStream makeFrameGpu(const cv::cuda::GpuMat& image,
    std::chrono::steady_clock::time_point timestamp,std::uint64_t generation);
#endif
std::shared_ptr<const FrameStorage> storage(const NodeGraph::FrameStream& frame);
cv::Mat cpuImage(const NodeGraph::FrameStream& frame);
cv::Mat sourceImage(const NodeGraph::FrameStream& frame);
std::string resolveModelPath(const NodeGraph::Node& node);
void reportError(int nodeKey,const std::string& error);

// Called without configMutex. Applies node settings to the physical capture
// service and returns false when the graph has no active capture source.
bool syncCaptureConfiguration(bool force = false);
void seedMissingModelPaths();
NodeGraph::Actions processFrame(const NodeGraph::FrameStream& frame,const Infer& infer);
}
