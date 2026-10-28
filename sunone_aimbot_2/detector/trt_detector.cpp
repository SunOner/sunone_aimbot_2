#ifdef USE_CUDA
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudawarping.hpp>
#include <opencv2/dnn.hpp>
#include <algorithm>
#include <atomic>
#include <limits>
#include <numeric>
#include <vector>
#include <queue>
#include <mutex>

#include "trt_detector.h"
#include "nvinf.h"
#include "sunone_aimbot_2.h"
#include "other_tools.h"
#include "postProcess.h"
#include "cuda_preprocess.h"
#include "depth/depth_mask.h"
#include "capture.h"
#include "capture/circle_fov.h"
#include "scr/data_collector.h"

int model_quant;
std::vector<float> outputData;

extern std::atomic<bool> detector_model_changed;
extern std::atomic<bool> detection_resolution_changed;

static bool error_logged = false;

namespace {
bool tryGetDimInt(int64_t value, int* out)
{
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        return false;
    *out = static_cast<int>(value);
    return true;
}

bool tryGetPositiveDimInt(int64_t value, int* out)
{
    if (value <= 0)
        return false;
    return tryGetDimInt(value, out);
}

bool intersectsDepthMask(const cv::Rect& box, const cv::Mat& mask)
{
    if (box.width <= 0 || box.height <= 0 || mask.empty() || mask.type() != CV_8UC1)
        return false;

    const cv::Rect imageBounds(0, 0, mask.cols, mask.rows);
    const cv::Rect clipped = box & imageBounds;
    if (clipped.width <= 0 || clipped.height <= 0)
        return false;

    const int cx = clipped.x + clipped.width / 2;
    const int cy = clipped.y + clipped.height / 2;
    if (mask.at<uint8_t>(cy, cx) != 0)
        return true;

    const cv::Mat roi = mask(clipped);
    return cv::countNonZero(roi) > 0;
}

void filterDetectionsByDepthMask(std::vector<Detection>& detections)
{
    static cv::Mat holdTtl;

    if (detections.empty())
        return;

    if (!config.depth_inference_enabled || !config.depth_mask_enabled)
    {
        holdTtl.release();
        return;
    }

    const int holdFrames = std::clamp(config.depth_mask_hold_frames, 0, 120);
    cv::Mat currentMask = getCurrentDetectionSuppressionMask();
    cv::Mat suppressionMask;

    if (holdFrames <= 0)
    {
        holdTtl.release();
        suppressionMask = currentMask;
    }
    else
    {
        if (!currentMask.empty() && currentMask.type() == CV_8UC1)
        {
            if (holdTtl.empty() || holdTtl.size() != currentMask.size())
                holdTtl = cv::Mat::zeros(currentMask.size(), CV_16UC1);
            cv::subtract(holdTtl, cv::Scalar(1), holdTtl);
            holdTtl.setTo(cv::Scalar(static_cast<uint16_t>(holdFrames)), currentMask);
        }
        else if (!holdTtl.empty())
        {
            cv::subtract(holdTtl, cv::Scalar(1), holdTtl);
        }

        if (!holdTtl.empty() && cv::countNonZero(holdTtl) > 0)
        {
            cv::compare(holdTtl, cv::Scalar(0), suppressionMask, cv::CMP_GT);
        }
        else
        {
            suppressionMask.release();
        }
    }

    if (suppressionMask.empty() || suppressionMask.type() != CV_8UC1)
        return;

    detections.erase(
        std::remove_if(detections.begin(), detections.end(),
            [&suppressionMask](const Detection& det) { return intersectsDepthMask(det.box, suppressionMask); }),
        detections.end());
}

void filterDetectionsByCircleFov(std::vector<Detection>& detections)
{
    if (detections.empty() || !config.circle_fov_enabled)
        return;

    const cv::Size detectionSize(config.detection_resolution, config.detection_resolution);
    detections.erase(
        std::remove_if(detections.begin(), detections.end(),
            [&detectionSize](const Detection& det)
            {
                const cv::Point2f center(
                    static_cast<float>(det.box.x) + static_cast<float>(det.box.width) * 0.5f,
                    static_cast<float>(det.box.y) + static_cast<float>(det.box.height) * 0.5f);
                return !pointInsideCircleFov(center, detectionSize, config.circle_fov_radius_percent);
            }),
        detections.end());
}
} // namespace

TrtDetector::TrtDetector()
    : frameReady(false),
    shouldExit(false),
    useCudaGraph(false),
    cudaGraphCaptured(false),
    cudaGraph(nullptr),
    cudaGraphExec(nullptr),
    inputBufferDevice(nullptr),
    img_scale(1.0f),
    numClasses(0)
{
    stream = nullptr;
    cudaStreamCreate(&stream);
}

TrtDetector::~TrtDetector()
{
    requestStop();
    destroyCudaGraph();
    freePinnedOutputs();

    for (auto& binding : inputBindings) if (binding.second) cudaFree(binding.second);
    for (auto& binding : outputBindings) if (binding.second) cudaFree(binding.second);
    if (inputBufferDevice) cudaFree(inputBufferDevice);
    if (preprocessStartEvent) cudaEventDestroy(preprocessStartEvent);
    if (inferenceStartEvent) cudaEventDestroy(inferenceStartEvent);
    if (inferenceCompleteEvent) cudaEventDestroy(inferenceCompleteEvent);
    if (copyCompleteEvent) cudaEventDestroy(copyCompleteEvent);
    if (stream) cudaStreamDestroy(stream);
}

void TrtDetector::requestStop()
{
    shouldExit = true;
    inferenceCV.notify_all();
}

bool TrtDetector::isInitialized() const
{
    return context != nullptr;
}

void TrtDetector::freePinnedOutputs()
{
    for (auto& kv : pinnedOutputBuffers)
    {
        if (kv.second)
            cudaFreeHost(kv.second);
    }
    pinnedOutputBuffers.clear();
}

void TrtDetector::allocatePinnedOutputs()
{
    freePinnedOutputs();

    for (const auto& name : outputNames)
    {
        const size_t bytes = outputSizes[name];
        if (bytes == 0) continue;

        void* hostPtr = nullptr;
        cudaError_t err = cudaHostAlloc(&hostPtr, bytes, cudaHostAllocDefault);
        if (err != cudaSuccess)
        {
            std::cerr << "[Detector] cudaHostAlloc failed for output " << name
                << " (" << bytes << " bytes): " << cudaGetErrorString(err) << std::endl;
            continue;
        }

        pinnedOutputBuffers[name] = hostPtr;

        if (config.verbose)
        {
            std::cout << "[Detector] Allocated pinned host buffer for output " << name
                << ": " << bytes << " bytes" << std::endl;
        }
    }
}

void TrtDetector::destroyCudaGraph()
{
    if (cudaGraphExec)
    {
        cudaGraphExecDestroy(cudaGraphExec);
        cudaGraphExec = nullptr;
    }
    if (cudaGraph)
    {
        cudaGraphDestroy(cudaGraph);
        cudaGraph = nullptr;
    }
    cudaGraphCaptured = false;
}

void TrtDetector::captureCudaGraph()
{
    if (!useCudaGraph || cudaGraphCaptured) return;

    destroyCudaGraph();

    cudaStreamSynchronize(stream);

    cudaError_t st = cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] BeginCapture failed: "
            << cudaGetErrorString(st) << std::endl;
        return;
    }

    context->enqueueV3(stream);
    cudaEventRecord(inferenceCompleteEvent, stream);

    for (const auto& name : outputNames)
        if (pinnedOutputBuffers.count(name))
            cudaMemcpyAsync(pinnedOutputBuffers[name],
                outputBindings[name],
                outputSizes[name],
                cudaMemcpyDeviceToHost,
                stream);

    st = cudaStreamEndCapture(stream, &cudaGraph);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] EndCapture failed: "
            << cudaGetErrorString(st) << std::endl;
        return;
    }

    st = cudaGraphInstantiate(&cudaGraphExec, cudaGraph, 0);
    if (st != cudaSuccess) {
        std::cerr << "[Detector] GraphInstantiate failed: "
            << cudaGetErrorString(st) << std::endl;
        cudaGraphDestroy(cudaGraph);
        cudaGraph = nullptr;
        return;
    }

    cudaGraphCaptured = true;
}

inline void TrtDetector::launchCudaGraph()
{
    auto err = cudaGraphLaunch(cudaGraphExec, stream);
    if (err != cudaSuccess)
    {
        std::cerr << "[Detector] GraphLaunch failed: " << cudaGetErrorString(err) << std::endl;
    }
}

void TrtDetector::getInputNames()
{
    inputNames.clear();
    inputSizes.clear();

    for (int i = 0; i < engine->getNbIOTensors(); ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
        {
            inputNames.emplace_back(name);
            if (config.verbose)
            {
                std::cout << "[Detector] Detected input: " << name << std::endl;
            }
        }
    }
}

void TrtDetector::getOutputNames()
{
    outputNames.clear();
    outputSizes.clear();
    outputTypes.clear();
    outputShapes.clear();

    for (int i = 0; i < engine->getNbIOTensors(); ++i)
    {
        const char* name = engine->getIOTensorName(i);
        if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kOUTPUT)
        {
            outputNames.emplace_back(name);
            outputTypes[name] = engine->getTensorDataType(name);

            if (config.verbose)
            {
                std::cout << "[Detector] Detected output: " << name << std::endl;
            }
        }
    }
}

void TrtDetector::getBindings()
{
    for (auto& binding : inputBindings)
    {
        if (binding.second) cudaFree(binding.second);
    }
    inputBindings.clear();

    for (auto& binding : outputBindings)
    {
        if (binding.second) cudaFree(binding.second);
    }
    outputBindings.clear();

    for (const auto& name : inputNames)
    {
        size_t size = inputSizes[name];
        if (size > 0)
        {
            void* ptr = nullptr;

            cudaError_t err = cudaMalloc(&ptr, size);
            if (err == cudaSuccess)
            {
                inputBindings[name] = ptr;
                if (config.verbose)
                {
                    std::cout << "[Detector] Allocated " << size << " bytes for input " << name << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate input memory: " << cudaGetErrorString(err) << std::endl;
            }
        }
    }

    for (const auto& name : outputNames)
    {
        size_t size = outputSizes[name];
        if (size > 0) {
            void* ptr = nullptr;
            cudaError_t err = cudaMalloc(&ptr, size);
            if (err == cudaSuccess)
            {
                outputBindings[name] = ptr;
                if (config.verbose)
                {
                    std::cout << "[Detector] Allocated " << size << " bytes for output " << name << std::endl;
                }
            }
            else
            {
                std::cerr << "[Detector] Failed to allocate output memory: " << cudaGetErrorString(err) << std::endl;
            }
        }
    }
}

void TrtDetector::initialize(const std::string& modelFile)
{
    runtime.reset(nvinfer1::createInferRuntime(gLogger));
    loadEngine(modelFile);
    if (!engine)
    {
        std::cerr << "[Detector] Engine loading failed" << std::endl;
        return;
    }

    context.reset(engine->createExecutionContext());
    if (!context)
    {
        std::cerr << "[Detector] Context creation failed" << std::endl;
        return;
    }

    getInputNames();
    getOutputNames();
    if (inputNames.empty())
    {
        std::cerr << "[Detector] No input tensors found" << std::endl;
        return;
    }
    inputName = inputNames[0];

    nvinfer1::Dims modelInputDims = engine->getTensorShape(inputName.c_str());
    bool isStatic = true;
    for (int i = 0; i < modelInputDims.nbDims; ++i)
        if (modelInputDims.d[i] <= 0) isStatic = false;

    if (isStatic != config.fixed_input_size)
    {
        config.fixed_input_size = isStatic;
        detector_model_changed.store(true);
        std::cout << "[Detector] Automatically set fixed_input_size = " << (isStatic ? "true" : "false") << std::endl;
    }

    const int target = config.detection_resolution;
    nvinfer1::Dims inputDims = modelInputDims;
    if (!isStatic)
    {
        nvinfer1::Dims4 newShape{ 1, 3, target, target };
        context->setInputShape(inputName.c_str(), newShape);
        if (!context->allInputDimensionsSpecified())
        {
            std::cerr << "[Detector] Failed to set input dimensions" << std::endl;
            return;
        }
        inputDims = context->getTensorShape(inputName.c_str());
    }
    else
    {
        inputDims = context->getTensorShape(inputName.c_str());
    }

    inputSizes.clear();
    outputSizes.clear();
    outputShapes.clear();
    outputTypes.clear();
    fp16OutputScratch.clear();

    for (const auto& inName : inputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(inName.c_str());
        nvinfer1::DataType dt = engine->getTensorDataType(inName.c_str());
        inputSizes[inName] = getSizeByDim(d) * getElementSize(dt);
    }
    for (const auto& outName : outputNames)
    {
        nvinfer1::Dims d = context->getTensorShape(outName.c_str());
        nvinfer1::DataType dt = engine->getTensorDataType(outName.c_str());
        outputSizes[outName] = getSizeByDim(d) * getElementSize(dt);
        std::vector<int64_t> shape(d.nbDims);
        for (int j = 0; j < d.nbDims; ++j) shape[j] = d.d[j];
        outputShapes[outName] = std::move(shape);
        outputTypes[outName] = dt;
    }

    getBindings();

    allocatePinnedOutputs();

    if (!outputNames.empty())
    {
        const std::string& mainOut = outputNames[0];
        nvinfer1::Dims outDims = context->getTensorShape(mainOut.c_str());
        const int64_t outChannels = (outDims.nbDims >= 2) ? outDims.d[outDims.nbDims - 2] : 0;
        const int64_t classes64 = (outChannels > 4) ? (outChannels - 4) : 1;
        int classes = 0;
        if (!tryGetDimInt(classes64, &classes) || classes <= 0)
        {
            numClasses = 0;
        }
        else
        {
            numClasses = classes;
        }
    }

    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(inputDims.d[1], &c)
        || !tryGetPositiveDimInt(inputDims.d[2], &h)
        || !tryGetPositiveDimInt(inputDims.d[3], &w))
    {
        std::cerr << "[Detector] Invalid input dimensions" << std::endl;
        return;
    }

    img_scale = static_cast<float>(config.detection_resolution) / w;

    gpuResizedBuffer.create(h, w, CV_8UC3);
    gpuFloatBuffer.create(h, w, CV_32FC3);
    cvStream = cv::cuda::StreamAccessor::wrapStream(stream);

    cpuResizedBuffer.create(h, w, CV_8UC3);
    cpuFloatBuffer.create(h, w, CV_32FC3);
    inputHostBuffer.resize(static_cast<size_t>(c) * static_cast<size_t>(h) * static_cast<size_t>(w));

    for (const auto& n : inputNames)
        context->setTensorAddress(n.c_str(), inputBindings[n]);
    for (const auto& n : outputNames)
        context->setTensorAddress(n.c_str(), outputBindings[n]);

    if (preprocessStartEvent) cudaEventDestroy(preprocessStartEvent);
    if (inferenceStartEvent) cudaEventDestroy(inferenceStartEvent);
    if (inferenceCompleteEvent) cudaEventDestroy(inferenceCompleteEvent);
    if (copyCompleteEvent) cudaEventDestroy(copyCompleteEvent);

    preprocessStartEvent = nullptr;
    inferenceStartEvent = nullptr;
    inferenceCompleteEvent = nullptr;
    copyCompleteEvent = nullptr;

    cudaEventCreate(&preprocessStartEvent);
    cudaEventCreate(&inferenceStartEvent);
    cudaEventCreate(&inferenceCompleteEvent);
    cudaEventCreate(&copyCompleteEvent);

    useCudaGraph = config.use_cuda_graph;
    if (useCudaGraph)
    {
        captureCudaGraph();
    }

    if (config.verbose)
    {
        std::cout << "[Detector] Initialized. ModelStatic=" << std::boolalpha << isStatic
            << ", NetInput=" << h << "x" << w << " (scale=" << img_scale << ")" << std::endl;
    }
}

size_t TrtDetector::getSizeByDim(const nvinfer1::Dims& dims)
{
    size_t size = 1;
    for (int i = 0; i < dims.nbDims; ++i)
    {
        if (dims.d[i] < 0) return 0;
        size *= dims.d[i];
    }
    return size;
}

size_t TrtDetector::getElementSize(nvinfer1::DataType dtype)
{
    switch (dtype)
    {
    case nvinfer1::DataType::kFLOAT: return 4;
    case nvinfer1::DataType::kHALF: return 2;
    case nvinfer1::DataType::kINT32: return 4;
    case nvinfer1::DataType::kINT8: return 1;
    default: return 0;
    }
}

void TrtDetector::loadEngine(const std::string& modelFile)
{
    std::string engineFilePath;
    std::filesystem::path modelPath(modelFile);
    std::string extension = modelPath.extension().string();

    if (extension == ".engine")
    {
        engineFilePath = modelFile;
    }
    else if (extension == ".onnx")
    {
        engineFilePath = modelPath.replace_extension(".engine").string();

        if (!fileExists(engineFilePath))
        {
            std::cout << "[Detector] Building engine from ONNX model" << std::endl;

            nvinfer1::ICudaEngine* builtEngine = buildEngineFromOnnx(modelFile, gLogger);
            if (builtEngine)
            {
                nvinfer1::IHostMemory* serializedEngine = builtEngine->serialize();

                if (serializedEngine)
                {
                    std::ofstream engineFile(engineFilePath, std::ios::binary);
                    if (engineFile)
                    {
                        engineFile.write(reinterpret_cast<const char*>(serializedEngine->data()), serializedEngine->size());
                        engineFile.close();

                        config.ai_model = std::filesystem::path(engineFilePath).filename().string();
                        config.saveConfig("config.ini");

                        std::cout << "[Detector] Engine saved to: " << engineFilePath << std::endl;
                    }
                    delete serializedEngine;
                }
                delete builtEngine;
            }
        }
    }
    else
    {
        std::cerr << "[Detector] Unsupported model format: " << extension << std::endl;
        return;
    }

    std::cout << "[Detector] Loading engine: " << engineFilePath << std::endl;
    engine.reset(loadEngineFromFile(engineFilePath, runtime.get()));
}

void TrtDetector::processFrame(
    const cv::Mat& detection_frame,
    const cv::Mat& source_frame,
    std::chrono::steady_clock::time_point frameTimestamp)
{
    const auto generation = detectionBuffer.currentGeneration();
    detectionBuffer.withCurrentGeneration(generation, [&] {
        std::unique_lock<std::mutex> lock(inferenceMutex);
        currentFrame = detection_frame;
        currentSourceFrame = source_frame.empty() ? detection_frame : source_frame;
        currentFrameGpu.release();
        currentFrameTimestamp = (frameTimestamp.time_since_epoch().count() != 0)
            ? frameTimestamp
            : std::chrono::steady_clock::now();
        currentFrameGeneration = generation;
        pendingFrameType = PendingFrameType::Cpu;
        frameReady = true;
        inferenceCV.notify_one();
    });
}

void TrtDetector::processFrameGpu(
    const cv::cuda::GpuMat& frame,
    std::chrono::steady_clock::time_point frameTimestamp)
{
    const auto generation = detectionBuffer.currentGeneration();
    detectionBuffer.withCurrentGeneration(generation, [&] {
        std::unique_lock<std::mutex> lock(inferenceMutex);
        currentFrame.release();
        currentSourceFrame.release();
        currentFrameGpu = frame;
        currentFrameTimestamp = (frameTimestamp.time_since_epoch().count() != 0)
            ? frameTimestamp
            : std::chrono::steady_clock::now();
        currentFrameGeneration = generation;
        pendingFrameType = PendingFrameType::Gpu;
        frameReady = true;
        inferenceCV.notify_one();
    });
}

std::vector<Detection> TrtDetector::detect(const cv::Mat& frame)
{
    std::vector<Detection> latestDetections;
    if (!context || frame.empty())
        return latestDetections;

    cudaEventRecord(preprocessStartEvent, stream);
    preProcess(frame);
    cudaEventRecord(inferenceStartEvent, stream);

    const bool usedGraph = useCudaGraph && cudaGraphCaptured;
    if (usedGraph)
    {
        launchCudaGraph();
        cudaEventRecord(copyCompleteEvent, stream);
        cudaEventSynchronize(copyCompleteEvent);
    }
    else
    {
        context->enqueueV3(stream);
        cudaEventRecord(inferenceCompleteEvent, stream);

        for (const auto& name : outputNames)
        {
            const size_t size = outputSizes[name];

            auto itPinned = pinnedOutputBuffers.find(name);
            if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
                continue;

            cudaMemcpyAsync(
                itPinned->second,
                outputBindings[name],
                size,
                cudaMemcpyDeviceToHost,
                stream);
        }

        cudaEventRecord(copyCompleteEvent, stream);
        cudaEventSynchronize(copyCompleteEvent);
    }

    auto tPostStart = std::chrono::steady_clock::now();

    for (const auto& name : outputNames)
    {
        const auto itPinned = pinnedOutputBuffers.find(name);
        if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
            continue;

        nvinfer1::DataType dtype = outputTypes[name];
        if (dtype == nvinfer1::DataType::kHALF)
        {
            const size_t numElements = outputSizes[name] / sizeof(__half);
            const __half* halfPtr = reinterpret_cast<const __half*>(itPinned->second);

            auto& outputDataFloat = fp16OutputScratch[name];
            if (outputDataFloat.size() != numElements)
                outputDataFloat.resize(numElements);

            for (size_t i = 0; i < numElements; ++i)
                outputDataFloat[i] = __half2float(halfPtr[i]);

            latestDetections = postProcess(outputDataFloat.data(), name, &lastNmsTime);
        }
        else if (dtype == nvinfer1::DataType::kFLOAT)
        {
            const float* floatPtr = reinterpret_cast<const float*>(itPinned->second);
            latestDetections = postProcess(floatPtr, name, &lastNmsTime);
        }
    }

    auto tPostEnd = std::chrono::steady_clock::now();

    float preprocessMs = 0.0f;
    float inferenceMs = 0.0f;
    float copyMs = 0.0f;

    cudaEventElapsedTime(&preprocessMs, preprocessStartEvent, inferenceStartEvent);
    cudaEventElapsedTime(&inferenceMs, inferenceStartEvent, inferenceCompleteEvent);
    cudaEventElapsedTime(&copyMs, inferenceCompleteEvent, copyCompleteEvent);

    lastPreprocessTime = std::chrono::duration<double, std::milli>(preprocessMs);
    lastInferenceTime = std::chrono::duration<double, std::milli>(inferenceMs);
    lastCopyTime = std::chrono::duration<double, std::milli>(copyMs);
    lastPostprocessTime = tPostEnd - tPostStart;

    return latestDetections;
}

void TrtDetector::inferenceThread()
{
    while (!shouldExit)
    {
        if (config.backend != "TRT")
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (detector_model_changed.load())
        {
            {
                std::unique_lock<std::mutex> lock(inferenceMutex);
                destroyCudaGraph();
                context.reset();
                engine.reset();

                freePinnedOutputs();

                for (auto& binding : inputBindings)
                    if (binding.second) cudaFree(binding.second);
                inputBindings.clear();
                for (auto& binding : outputBindings)
                    if (binding.second) cudaFree(binding.second);
                outputBindings.clear();

                currentFrame.release();
                currentSourceFrame.release();
                currentFrameGpu.release();
                frameReady = false;
                pendingFrameType = PendingFrameType::None;
            }
            initialize("models/" + config.ai_model);
            detection_resolution_changed.store(true);
            detector_model_changed.store(false);
        }

        if (useCudaGraph != config.use_cuda_graph)
        {
            useCudaGraph = config.use_cuda_graph;
            if (!useCudaGraph)
            {
                destroyCudaGraph();
            }
            else if (context)
            {
                captureCudaGraph();
            }
        }

        cv::Mat frame;
        cv::Mat sourceFrame;
        cv::cuda::GpuMat frameGpu;
        std::chrono::steady_clock::time_point frameTimestamp{};
        std::uint64_t frameGeneration = 0;
        PendingFrameType frameType = PendingFrameType::None;
        bool hasNewFrame = false;

        {
            std::unique_lock<std::mutex> lock(inferenceMutex);
            if (!frameReady && !shouldExit)
                inferenceCV.wait(lock, [this] { return frameReady || shouldExit; });

            if (shouldExit) break;

            if (frameReady)
            {
                frameType = pendingFrameType;
                frameTimestamp = currentFrameTimestamp;
                frameGeneration = currentFrameGeneration;
                if (frameType == PendingFrameType::Gpu)
                {
                    frameGpu = currentFrameGpu;
                    currentFrameGpu.release();
                    currentFrame.release();
                    currentSourceFrame.release();
                }
                else
                {
                    frame = std::move(currentFrame);
                    sourceFrame = std::move(currentSourceFrame);
                    currentFrameGpu.release();
                }
                pendingFrameType = PendingFrameType::None;
                frameReady = false;
                hasNewFrame = true;
            }
        }

        if (!context)
        {
            if (!error_logged)
            {
                std::cerr << "[Detector] Context not initialized" << std::endl;
                error_logged = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        else
        {
            error_logged = false;
        }

        if (hasNewFrame)
        {
            if (!detectionBuffer.withCurrentGeneration(frameGeneration, [] {}))
                continue;
            const bool hasCpuFrame = (frameType == PendingFrameType::Cpu && !frame.empty());
            const bool hasGpuFrame = (frameType == PendingFrameType::Gpu && !frameGpu.empty());
            if (!hasCpuFrame && !hasGpuFrame)
                continue;

            try
            {
                cudaEventRecord(preprocessStartEvent, stream);
                if (hasGpuFrame)
                    preProcess(frameGpu);
                else
                    preProcess(frame);
                cudaEventRecord(inferenceStartEvent, stream);
                bool usedGraph = useCudaGraph && cudaGraphCaptured;
                if (usedGraph)
                {
                    launchCudaGraph();
                    cudaEventRecord(copyCompleteEvent, stream);
                    cudaEventSynchronize(copyCompleteEvent);
                }
                else
                {
                    context->enqueueV3(stream);
                    cudaEventRecord(inferenceCompleteEvent, stream);

                    for (const auto& name : outputNames)
                    {
                        const size_t size = outputSizes[name];

                        auto itPinned = pinnedOutputBuffers.find(name);
                        if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
                            continue;

                        cudaMemcpyAsync(
                            itPinned->second,
                            outputBindings[name],
                            size,
                            cudaMemcpyDeviceToHost,
                            stream
                        );
                    }

                    cudaEventRecord(copyCompleteEvent, stream);
                    cudaEventSynchronize(copyCompleteEvent);
                }

                // Post-processing (CPU)
                auto t_post_start = std::chrono::steady_clock::now();

                std::vector<Detection> latestDetections;
                for (const auto& name : outputNames)
                {
                    const auto itPinned = pinnedOutputBuffers.find(name);
                    if (itPinned == pinnedOutputBuffers.end() || !itPinned->second)
                        continue;

                    nvinfer1::DataType dtype = outputTypes[name];

                    if (dtype == nvinfer1::DataType::kHALF)
                    {
                        // Convert to float on CPU
                        const size_t numElements = outputSizes[name] / sizeof(__half);
                        const __half* halfPtr = reinterpret_cast<const __half*>(itPinned->second);

                        auto& outputDataFloat = fp16OutputScratch[name];
                        if (outputDataFloat.size() != numElements)
                            outputDataFloat.resize(numElements);

                        for (size_t i = 0; i < numElements; ++i)
                            outputDataFloat[i] = __half2float(halfPtr[i]);

                        latestDetections = postProcess(outputDataFloat.data(), name, &lastNmsTime);
                    }
                    else if (dtype == nvinfer1::DataType::kFLOAT)
                    {
                        const float* floatPtr = reinterpret_cast<const float*>(itPinned->second);
                        latestDetections = postProcess(floatPtr, name, &lastNmsTime);
                    }
                }

                std::vector<cv::Rect> boxes;
                std::vector<int> classes;
                std::vector<float> confidences;
                boxes.reserve(latestDetections.size());
                classes.reserve(latestDetections.size());
                confidences.reserve(latestDetections.size());
                for (const auto& det : latestDetections)
                {
                    boxes.push_back(det.box);
                    classes.push_back(det.classId);
                    confidences.push_back(det.confidence);
                }

                if (!detectionBuffer.setForGeneration(frameGeneration, boxes, classes, confidences, frameTimestamp))
                    continue;

                if (hasGpuFrame)
                {
                    cvm::MaybeCollectDataSample(
                        "",
                        config.ai_model.c_str(),
                        frameGpu,
                        boxes,
                        classes,
                        confidences,
                        aiming.load(),
                        config);
                }
                else
                {
                    const cv::Mat& frameForCollection = sourceFrame.empty() ? frame : sourceFrame;
                    cvm::MaybeCollectDataSample(
                        "",
                        config.ai_model.c_str(),
                        frameForCollection,
                        boxes,
                        classes,
                        confidences,
                        aiming.load(),
                        config);
                }

                auto t_post_end = std::chrono::steady_clock::now();

                float preprocessMs = 0.0f;
                float inferenceMs = 0.0f;
                float copyMs = 0.0f;

                cudaEventElapsedTime(&preprocessMs, preprocessStartEvent, inferenceStartEvent);
                cudaEventElapsedTime(&inferenceMs, inferenceStartEvent, inferenceCompleteEvent);
                cudaEventElapsedTime(&copyMs, inferenceCompleteEvent, copyCompleteEvent);

                lastPreprocessTime = std::chrono::duration<double, std::milli>(preprocessMs);
                lastInferenceTime = std::chrono::duration<double, std::milli>(inferenceMs);
                lastCopyTime = std::chrono::duration<double, std::milli>(copyMs);
                lastPostprocessTime = t_post_end - t_post_start;
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Detector] Error during inference: " << e.what() << std::endl;
            }
        }
    }
}

void TrtDetector::preProcess(const cv::Mat& frame)
{
    if (frame.empty())
        return;

    void* inputBuffer = inputBindings[inputName];
    if (!inputBuffer)
        return;

    nvinfer1::Dims dims = context->getTensorShape(inputName.c_str());
    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(dims.d[1], &c)
        || !tryGetPositiveDimInt(dims.d[2], &h)
        || !tryGetPositiveDimInt(dims.d[3], &w))
    {
        return;
    }

    if (c != 3)
        return;

    cv::Mat bgrFrame;
    switch (frame.channels())
    {
    case 4:
        cv::cvtColor(frame, cpuBgrBuffer, cv::COLOR_BGRA2BGR);
        bgrFrame = cpuBgrBuffer;
        break;
    case 1:
        cv::cvtColor(frame, cpuBgrBuffer, cv::COLOR_GRAY2BGR);
        bgrFrame = cpuBgrBuffer;
        break;
    case 3:
        bgrFrame = frame;
        break;
    default:
        return;
    }

    cv::Mat resizedBgr;
    if (bgrFrame.cols != w || bgrFrame.rows != h)
    {
        cv::resize(bgrFrame, cpuResizedBuffer, cv::Size(w, h), 0, 0, cv::INTER_LINEAR);
        resizedBgr = cpuResizedBuffer;
    }
    else
    {
        resizedBgr = bgrFrame;
    }
    resizedBgr.convertTo(cpuFloatBuffer, CV_32FC3, 1.0f / 255.0f);

    copyCpuTensorToDevice(cpuFloatBuffer, w, h, inputBuffer);
}

void TrtDetector::preProcess(const cv::cuda::GpuMat& frame)
{
    if (frame.empty())
        return;

    void* inputBuffer = inputBindings[inputName];
    if (!inputBuffer)
        return;

    nvinfer1::Dims dims = context->getTensorShape(inputName.c_str());
    int c = 0;
    int h = 0;
    int w = 0;
    if (!tryGetPositiveDimInt(dims.d[1], &c)
        || !tryGetPositiveDimInt(dims.d[2], &h)
        || !tryGetPositiveDimInt(dims.d[3], &w))
    {
        return;
    }

    if (c != 3)
        return;

    cv::cuda::GpuMat bgrFrame;
    switch (frame.channels())
    {
    case 4:
        cv::cuda::cvtColor(frame, gpuFrameBuffer, cv::COLOR_BGRA2BGR, 0, cvStream);
        bgrFrame = gpuFrameBuffer;
        break;
    case 1:
        cv::cuda::cvtColor(frame, gpuFrameBuffer, cv::COLOR_GRAY2BGR, 0, cvStream);
        bgrFrame = gpuFrameBuffer;
        break;
    case 3:
        bgrFrame = frame;
        break;
    default:
        return;
    }

    cv::cuda::GpuMat resizedBgr;
    if (bgrFrame.cols != w || bgrFrame.rows != h)
    {
        cv::cuda::resize(bgrFrame, gpuResizedBuffer, cv::Size(w, h), 0, 0, cv::INTER_LINEAR, cvStream);
        resizedBgr = gpuResizedBuffer;
    }
    else
    {
        resizedBgr = bgrFrame;
    }
    resizedBgr.convertTo(gpuFloatBuffer, CV_32FC3, 1.0 / 255.0, 0.0, cvStream);

    launch_hwc_to_chw_norm(
        gpuFloatBuffer.ptr<float>(),
        gpuFloatBuffer.step,
        reinterpret_cast<float*>(inputBuffer),
        w,
        h,
        stream);

    if (config.verbose)
    {
        const cudaError_t err = cudaGetLastError();
        if (err != cudaSuccess)
        {
            std::cerr << "[Detector] preprocess kernel launch error: " << cudaGetErrorString(err) << std::endl;
        }
    }
}

void TrtDetector::copyCpuTensorToDevice(const cv::Mat& bgrFloatFrame, int width, int height, void* inputBuffer)
{
    if (bgrFloatFrame.empty() || bgrFloatFrame.channels() != 3 || !inputBuffer)
        return;

    const size_t channelSize = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t tensorSize = channelSize * 3;
    if (inputHostBuffer.size() != tensorSize)
        inputHostBuffer.resize(tensorSize);

    float* dst = inputHostBuffer.data();
    cv::Mat bgrToRgbPlanes[3] = {
        cv::Mat(height, width, CV_32F, dst + channelSize * 2),
        cv::Mat(height, width, CV_32F, dst + channelSize),
        cv::Mat(height, width, CV_32F, dst)
    };
    cv::split(bgrFloatFrame, bgrToRgbPlanes);

    cudaError_t err = cudaMemcpyAsync(
        inputBuffer,
        inputHostBuffer.data(),
        tensorSize * sizeof(float),
        cudaMemcpyHostToDevice,
        stream);
    if (err != cudaSuccess)
    {
        std::cerr << "[Detector] preprocess input copy failed: " << cudaGetErrorString(err) << std::endl;
    }
}

std::vector<Detection> TrtDetector::postProcess(const float* output, const std::string& outputName, std::chrono::duration<double, std::milli>* nmsTime)
{
    if (!output)
        return {};

    const auto shapeIt = outputShapes.find(outputName);
    if (shapeIt == outputShapes.end())
        return {};

    std::vector<Detection> detections;
    
    detections = postProcessYolo(
        output,
        shapeIt->second,
        numClasses,
        config.confidence_threshold,
        config.nms_threshold,
        std::max(1, config.max_detections),
        nmsTime
    );
    filterDetectionsByDepthMask(detections);
    filterDetectionsByCircleFov(detections);
    return detections;
}
#endif
