#ifdef USE_CUDA
#ifndef TRT_DETECTOR_H
#define TRT_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda.hpp>
#include <NvInfer.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <unordered_map>
#include <cuda_fp16.h>
#include <memory>
#include <thread>
#include <chrono>
#include <cstdint>
#include <functional>
#include <cuda_runtime_api.h>

#include "postProcess.h"

class TrtDetector
{
public:
    TrtDetector();
    ~TrtDetector();
    void initialize(const std::string& modelFile);
    void processFrame(
        const cv::Mat& detection_frame,
        const cv::Mat& source_frame = cv::Mat(),
        std::chrono::steady_clock::time_point frameTimestamp = {});
    void processFrameGpu(
        const cv::cuda::GpuMat& frame,
        std::chrono::steady_clock::time_point frameTimestamp = {});
    void inferenceThread();
    void requestStop();
    bool isInitialized() const;
    std::vector<Detection> detect(const cv::Mat& frame);

    float img_scale;

    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
    std::unordered_map<std::string, size_t> outputSizes;

    std::chrono::duration<double, std::milli> lastPreprocessTime;
    std::chrono::duration<double, std::milli> lastInferenceTime;
    std::chrono::duration<double, std::milli> lastCopyTime;
    std::chrono::duration<double, std::milli> lastPostprocessTime;
    std::chrono::duration<double, std::milli> lastNmsTime;

private:
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;

    cudaStream_t stream;

    bool useCudaGraph;
    bool cudaGraphCaptured;
    cudaGraph_t cudaGraph;
    cudaGraphExec_t cudaGraphExec;
    void captureCudaGraph();
    void launchCudaGraph();
    void destroyCudaGraph();

    std::unordered_map<std::string, void*> pinnedOutputBuffers;
    void allocatePinnedOutputs();
    void freePinnedOutputs();

    std::mutex inferenceMutex;
    std::condition_variable inferenceCV;
    std::atomic<bool> shouldExit;
    cv::Mat currentFrame;
    cv::Mat currentSourceFrame;
    cv::cuda::GpuMat currentFrameGpu;
    std::chrono::steady_clock::time_point currentFrameTimestamp{};
    std::uint64_t currentFrameGeneration = 0;
    bool frameReady;

    enum class PendingFrameType
    {
        None = 0,
        Cpu = 1,
        Gpu = 2
    };
    PendingFrameType pendingFrameType = PendingFrameType::None;

    void loadEngine(const std::string& engineFile);

    void preProcess(const cv::Mat& frame);
    void preProcess(const cv::cuda::GpuMat& frame);
    void copyCpuTensorToDevice(const cv::Mat& bgrFloatFrame, int width, int height, void* inputBuffer);

    cv::cuda::GpuMat gpuFrameBuffer;
    cv::cuda::GpuMat gpuResizedBuffer;
    cv::cuda::GpuMat gpuFloatBuffer;
    cv::cuda::Stream cvStream;

    cv::Mat cpuBgrBuffer;
    cv::Mat cpuResizedBuffer;
    cv::Mat cpuFloatBuffer;
    std::vector<float> inputHostBuffer;

    std::vector<Detection> postProcess(
        const float* output,
        const std::string& outputName,
        std::chrono::duration<double, std::milli>* nmsTime
    );

    void getInputNames();
    void getOutputNames();
    void getBindings();

    std::unordered_map<std::string, size_t> inputSizes;
    std::unordered_map<std::string, void*> inputBindings;
    std::unordered_map<std::string, void*> outputBindings;
    std::unordered_map<std::string, std::vector<int64_t>> outputShapes;
    int numClasses;

    size_t getSizeByDim(const nvinfer1::Dims& dims);
    size_t getElementSize(nvinfer1::DataType dtype);

    std::string inputName;
    void* inputBufferDevice;

    std::unordered_map<std::string, nvinfer1::DataType> outputTypes;
    std::unordered_map<std::string, std::vector<float>> fp16OutputScratch;

    // CUDA Events
    cudaEvent_t preprocessStartEvent = nullptr;
    cudaEvent_t inferenceStartEvent = nullptr;
    cudaEvent_t inferenceCompleteEvent = nullptr;
    cudaEvent_t copyCompleteEvent = nullptr;
    bool asyncInferenceInProgress = false;
};

#endif // TRT_DETECTOR_H
#endif
