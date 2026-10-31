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
#include <cmath>
#include <cctype>
#include <filesystem>
#include <limits>
#include <numeric>
#include <vector>
#include <queue>
#include <mutex>
#include <stdexcept>

#include "trt_detector.h"
#include "nvinf.h"
#include "sunone_aimbot_2.h"
#include "other_tools.h"
#include "postProcess.h"
#include "cuda_preprocess.h"
#include "capture.h"
#include "nodes/vision_runtime.h"
#include "runtime/input_state.h"

int model_quant;
std::vector<float> outputData;

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

TrtBuildOptions buildOptions(const NodeGraph::Node& node,int width,int height)
{
    TrtBuildOptions options;
    options.fixedInputSize=NodeGraph::parameter(node,"fixed_input_size")!=0;
    options.enableFp16=NodeGraph::parameter(node,"export_enable_fp16")!=0;
    options.enableFp8=NodeGraph::parameter(node,"export_enable_fp8")!=0;
    options.inputWidth=width;options.inputHeight=height;
    options.minProfileSize=static_cast<int>(NodeGraph::parameter(node,"trt_profile_min_size"));
    options.optProfileSize=static_cast<int>(NodeGraph::parameter(node,"trt_profile_opt_size"));
    options.maxProfileSize=static_cast<int>(NodeGraph::parameter(node,"trt_profile_max_size"));
    return options;
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
    if(!context||inputNames.empty()||outputNames.empty())return false;
    for(const auto& name:inputNames){const auto found=inputBindings.find(name);if(found==inputBindings.end()||!found->second)return false;}
    for(const auto& name:outputNames){const auto found=outputBindings.find(name);const auto host=pinnedOutputBuffers.find(name);
        if(found==outputBindings.end()||!found->second||host==pinnedOutputBuffers.end()||!host->second)return false;}
    return true;
}

void TrtDetector::freePinnedOutputs()
{
    for (auto& kv : pinnedOutputBuffers)
    {
        if (kv.second&&pinnedOutputNames.count(kv.first))
            cudaFreeHost(kv.second);
    }
    pinnedOutputBuffers.clear();
    pinnedOutputNames.clear();pageableOutputBuffers.clear();
}

void TrtDetector::allocatePinnedOutputs()
{
    freePinnedOutputs();

    for (const auto& name : outputNames)
    {
        const size_t bytes = outputSizes[name];
        if (bytes == 0) continue;

        void* hostPtr = nullptr;
        if(!pinnedOutputsEnabled)
        {
            auto& buffer=pageableOutputBuffers[name];buffer.resize(bytes);
            pinnedOutputBuffers[name]=buffer.data();continue;
        }
        cudaError_t err = cudaHostAlloc(&hostPtr, bytes, cudaHostAllocDefault);
        if (err != cudaSuccess)
        {
            std::cerr << "[Detector] cudaHostAlloc failed for output " << name
                << " (" << bytes << " bytes): " << cudaGetErrorString(err) << std::endl;
            continue;
        }

        pinnedOutputBuffers[name] = hostPtr;
        pinnedOutputNames.insert(name);

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
    NodeGraph::Node settings=NodeGraph::defaultNode(NodeGraph::NodeKind::Inference,1);
    int resolution=320;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        settings.parameters["fixed_input_size"]=config.fixed_input_size?1:0;
        settings.parameters["confidence_threshold"]=config.confidence_threshold;
        settings.parameters["nms_threshold"]=config.nms_threshold;
        settings.parameters["max_detections"]=config.max_detections;
        settings.parameters["use_cuda_graph"]=config.use_cuda_graph?1:0;
        settings.parameters["use_pinned_memory"]=config.use_pinned_memory?1:0;
        settings.parameters["export_enable_fp16"]=config.export_enable_fp16?1:0;
        settings.parameters["export_enable_fp8"]=config.export_enable_fp8?1:0;
        resolution=config.detection_resolution;
    }
    initialize(modelFile,settings,resolution,resolution);
}

void TrtDetector::initialize(const std::string& modelFile,const NodeGraph::Node& settings,int width,int height)
{
    destroyCudaGraph();
    context.reset();engine.reset();runtime.reset();inputNames.clear();outputNames.clear();
    inferenceSettings=settings;inferenceWidth=width;inferenceHeight=height>0?height:width;
    pinnedOutputsEnabled=NodeGraph::parameter(settings,"use_pinned_memory")!=0;
    runtime.reset(nvinfer1::createInferRuntime(gLogger));
    if(!runtime)throw std::runtime_error("TensorRT runtime creation failed.");
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
    if(modelInputDims.nbDims!=4||(modelInputDims.d[0]>0&&modelInputDims.d[0]!=1)||
        (modelInputDims.d[1]>0&&modelInputDims.d[1]!=3))
        throw std::runtime_error("Inference requires a single NCHW RGB image input.");
    bool isStatic = true;
    for (int i = 0; i < modelInputDims.nbDims; ++i)
        if (modelInputDims.d[i] <= 0) isStatic = false;

    nvinfer1::Dims inputDims = modelInputDims;
    if (!isStatic)
    {
        nvinfer1::Dims4 newShape{ 1, 3, inferenceHeight, inferenceWidth };
        if (!context->setInputShape(inputName.c_str(), newShape)||!context->allInputDimensionsSpecified())
        {
            throw std::runtime_error("The frame size is outside this TensorRT engine's input profile.");
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

    img_scale = static_cast<float>(inferenceWidth) / w;
    imageScaleY=static_cast<double>(inferenceHeight)/h;

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

    useCudaGraph = NodeGraph::parameter(settings,"use_cuda_graph")!=0&&pinnedOutputsEnabled;
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
    const auto path=std::filesystem::u8path(modelFile);
    auto extension=path.extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char value){return static_cast<char>(std::tolower(value));});
    if(extension==".engine")
    {
        engine.reset(loadEngineFromFile(modelFile,runtime.get()));
        return;
    }
    if(extension!=".onnx")throw std::runtime_error("TensorRT requires an ONNX model or a prebuilt engine.");
    const auto options=buildOptions(inferenceSettings,inferenceWidth,inferenceHeight);
    const auto cachedPath=trtEngineCachePath(modelFile,options);
    if(fileExists(cachedPath))engine.reset(loadEngineFromFile(cachedPath,runtime.get()));
    if(engine)return;
    engine.reset(buildEngineFromOnnx(modelFile,gLogger,options,*runtime));
    if(!engine)return;
    std::unique_ptr<nvinfer1::IHostMemory> serialized(engine->serialize());
    if(!serialized)return;
    // A model can still run when its directory is read-only; cache writes are
    // an optimization rather than a requirement for graph evaluation.
    std::ofstream output(cachedPath,std::ios::binary|std::ios::trunc);
    if(output)
    {
        output.write(static_cast<const char*>(serialized->data()),static_cast<std::streamsize>(serialized->size()));
        if(output)std::cout << "[TensorRT] Node engine cached: " << cachedPath << std::endl;
    }
}

void TrtDetector::processFrame(const cv::Mat& detection_frame,const cv::Mat& source_frame,
    std::chrono::steady_clock::time_point timestamp)
{
    const auto generation=detectionBuffer.currentGeneration();
    detectionBuffer.withCurrentGeneration(generation,[&] {
        auto packet=NodeVision::makeFrame(detection_frame,source_frame,timestamp,generation);
        std::lock_guard<std::mutex> lock(inferenceMutex);
        currentGraphFrame=std::move(packet);frameReady=true;inferenceCV.notify_one();
    });
}

void TrtDetector::processFrameGpu(const cv::cuda::GpuMat& image,std::chrono::steady_clock::time_point timestamp)
{
    const auto generation=detectionBuffer.currentGeneration();
    detectionBuffer.withCurrentGeneration(generation,[&] {
        auto packet=NodeVision::makeFrameGpu(image,timestamp,generation);
        std::lock_guard<std::mutex> lock(inferenceMutex);
        currentGraphFrame=std::move(packet);frameReady=true;inferenceCV.notify_one();
    });
}

std::vector<Detection> TrtDetector::detect(const cv::Mat& frame)
{
    // Provider benchmark compatibility; production passes a node-owned snapshot.
    NodeGraph::Node settings=inferenceSettings;
    {std::lock_guard<std::mutex> lock(configMutex);
        settings.parameters["confidence_threshold"]=config.confidence_threshold;
        settings.parameters["nms_threshold"]=config.nms_threshold;
        settings.parameters["max_detections"]=config.max_detections;}
    return inferGraphFrame(frame,cv::cuda::GpuMat(),settings);
}

std::vector<Detection> TrtDetector::detect(const NodeGraph::FrameStream& frame,const NodeGraph::Node& settings)
{
    const auto storage=NodeVision::storage(frame);
    if(!storage)return {};
    if(frame->gpu&&!storage->gpu.empty())return inferGraphFrame(cv::Mat(),storage->gpu,settings);
    return inferGraphFrame(NodeVision::cpuImage(frame),cv::cuda::GpuMat(),settings);
}

std::vector<Detection> TrtDetector::inferGraphFrame(const cv::Mat& cpu,const cv::cuda::GpuMat& gpu,const NodeGraph::Node& settings)
{
    std::vector<Detection> detections;
    if(!isInitialized()||(cpu.empty()&&gpu.empty()))return detections;
    inferenceSettings=settings;
    auto checked=[](cudaError_t result,const char* operation) {
        if(result!=cudaSuccess)throw std::runtime_error(std::string(operation)+": "+cudaGetErrorString(result));
    };
    checked(cudaEventRecord(preprocessStartEvent,stream),"Preprocess timing");
    if(!gpu.empty())preProcess(gpu);else preProcess(cpu);
    checked(cudaEventRecord(inferenceStartEvent,stream),"Inference timing");
    if(useCudaGraph&&cudaGraphCaptured)
    {
        launchCudaGraph();
        checked(cudaEventRecord(copyCompleteEvent,stream),"Copy timing");
    }
    else
    {
        if(!context->enqueueV3(stream))throw std::runtime_error("TensorRT enqueue failed.");
        checked(cudaEventRecord(inferenceCompleteEvent,stream),"Inference completion");
        for(const auto& name:outputNames)
        {
            const auto host=pinnedOutputBuffers.find(name);
            if(host==pinnedOutputBuffers.end()||!host->second)throw std::runtime_error("Missing model output buffer.");
            checked(cudaMemcpyAsync(host->second,outputBindings[name],outputSizes[name],cudaMemcpyDeviceToHost,stream),"Output copy");
        }
        checked(cudaEventRecord(copyCompleteEvent,stream),"Copy completion");
    }
    checked(cudaEventSynchronize(copyCompleteEvent),"Inference execution");
    const auto postStarted=std::chrono::steady_clock::now();
    for(const auto& name:outputNames)
    {
        const auto host=pinnedOutputBuffers.find(name);
        if(host==pinnedOutputBuffers.end()||!host->second)continue;
        if(outputTypes[name]==nvinfer1::DataType::kHALF)
        {
            const size_t count=outputSizes[name]/sizeof(__half);
            const auto* half=reinterpret_cast<const __half*>(host->second);
            auto& converted=fp16OutputScratch[name];converted.resize(count);
            for(size_t i=0;i<count;++i)converted[i]=__half2float(half[i]);
            detections=postProcess(converted.data(),name,&lastNmsTime);
        }
        else if(outputTypes[name]==nvinfer1::DataType::kFLOAT)
            detections=postProcess(reinterpret_cast<const float*>(host->second),name,&lastNmsTime);
    }
    float preprocessMs=0,inferenceMs=0,copyMs=0;
    cudaEventElapsedTime(&preprocessMs,preprocessStartEvent,inferenceStartEvent);
    cudaEventElapsedTime(&inferenceMs,inferenceStartEvent,inferenceCompleteEvent);
    cudaEventElapsedTime(&copyMs,inferenceCompleteEvent,copyCompleteEvent);
    lastPreprocessTime=std::chrono::duration<double,std::milli>(preprocessMs);
    lastInferenceTime=std::chrono::duration<double,std::milli>(inferenceMs);
    lastCopyTime=std::chrono::duration<double,std::milli>(copyMs);
    lastPostprocessTime=std::chrono::steady_clock::now()-postStarted;
    return detections;
}

void TrtDetector::inferenceThread()
{
    struct ModelSlot
    {
        std::unique_ptr<TrtDetector> detector;
        std::string fingerprint;
        std::string error;
        std::chrono::steady_clock::time_point attemptedAt{};
    };
    std::unordered_map<int,ModelSlot> models;
    auto infer=[&](int nodeKey,const NodeGraph::FrameStream& packet,const NodeGraph::Node& settings)->NodeGraph::DetectionStream {
        try
        {
            if(NodeGraph::textParameter(settings,"backend")!="TRT")throw std::runtime_error("This build supports TensorRT inference only.");
            const std::string path=NodeVision::resolveModelPath(settings);
            if(path.empty())throw std::runtime_error("Select a model for this Inference node.");
            const std::string fingerprint=path+"|"+std::to_string(packet->width)+"x"+std::to_string(packet->height)+"|"+
                std::to_string(NodeGraph::parameter(settings,"fixed_input_size"))+"|"+
                std::to_string(NodeGraph::parameter(settings,"use_cuda_graph"))+"|"+std::to_string(NodeGraph::parameter(settings,"use_pinned_memory"))+"|"+
                std::to_string(NodeGraph::parameter(settings,"export_enable_fp16"))+"|"+std::to_string(NodeGraph::parameter(settings,"export_enable_fp8"))+"|"+
                std::to_string(NodeGraph::parameter(settings,"trt_profile_min_size"))+"|"+std::to_string(NodeGraph::parameter(settings,"trt_profile_opt_size"))+"|"+
                std::to_string(NodeGraph::parameter(settings,"trt_profile_max_size"));
            auto& slot=models[nodeKey];const auto now=std::chrono::steady_clock::now();
            if(slot.fingerprint!=fingerprint||!slot.detector||!slot.detector->isInitialized())
            {
                if(slot.fingerprint==fingerprint&&now-slot.attemptedAt<std::chrono::seconds(2))
                {NodeVision::reportError(nodeKey,slot.error);return {};}
                slot.fingerprint=fingerprint;slot.attemptedAt=now;slot.detector.reset();
                slot.detector=std::make_unique<TrtDetector>();slot.detector->initialize(path,settings,packet->width,packet->height);
                if(!slot.detector->isInitialized())throw std::runtime_error("The selected TensorRT model could not be initialized.");
            }
            const auto detected=slot.detector->detect(packet,settings);
            auto result=std::make_shared<NodeGraph::DetectionFrame>();
            result->version=packet->version;result->generation=packet->generation;result->timestampSeconds=packet->timestampSeconds;
            result->width=packet->width;result->height=packet->height;result->fresh=true;
            result->modelName=NodeGraph::textParameter(settings,"model_path");
            result->objects.reserve(detected.size());
            for(const auto& detection:detected)result->objects.push_back({static_cast<double>(detection.box.x),static_cast<double>(detection.box.y),
                static_cast<double>(detection.box.width),static_cast<double>(detection.box.height),detection.classId,detection.confidence});
            lastPreprocessTime=slot.detector->lastPreprocessTime;lastInferenceTime=slot.detector->lastInferenceTime;
            lastCopyTime=slot.detector->lastCopyTime;lastPostprocessTime=slot.detector->lastPostprocessTime;lastNmsTime=slot.detector->lastNmsTime;
            slot.error.clear();NodeVision::reportError(nodeKey,"");return result;
        }
        catch(const std::exception& error)
        {
            const auto slot=models.find(nodeKey);if(slot!=models.end())slot->second.error=error.what();
            NodeVision::reportError(nodeKey,error.what());return {};
        }
    };
    std::uint64_t providerRevision=0;
    while(!shouldExit)
    {
        const auto revision=NodeGraph::service().revision();
        if(revision!=providerRevision)
        {
            const auto active=NodeGraph::service().providerNodes(NodeGraph::NodeKind::Inference);
            for(auto it=models.begin();it!=models.end();)
            {
                const bool keep=std::any_of(active.begin(),active.end(),[&](const NodeGraph::Node& node){return
                    static_cast<int>(NodeGraph::GraphKind::Vision)*1000001+node.id==it->first&&NodeGraph::parameter(node,"enabled")!=0;});
                if(!keep)it=models.erase(it);else ++it;
            }
            providerRevision=revision;
        }
        NodeGraph::FrameStream frame;
        {
            std::unique_lock<std::mutex> lock(inferenceMutex);
            inferenceCV.wait_for(lock,std::chrono::milliseconds(50),[&]{return frameReady||shouldExit;});
            if(shouldExit)break;
            if(!frameReady)continue;
            frame=std::move(currentGraphFrame);frameReady=false;
        }
        if(!frame||detectionBuffer.isPaused()||!detectionBuffer.withCurrentGeneration(frame->generation,[]{}))continue;
        try{NodeVision::processFrame(frame,infer);}
        catch(const std::exception& error){std::cerr<<"[TRT] Vision evaluation failed: "<<error.what()<<std::endl;}
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
        NodeGraph::parameter(inferenceSettings,"class_count")>0?static_cast<int>(NodeGraph::parameter(inferenceSettings,"class_count")):numClasses,
        static_cast<float>(NodeGraph::parameter(inferenceSettings,"confidence_threshold")),
        static_cast<float>(NodeGraph::parameter(inferenceSettings,"nms_threshold")),
        std::max(1,static_cast<int>(NodeGraph::parameter(inferenceSettings,"max_detections"))),
        nmsTime,img_scale
    );
    if(img_scale>0&&std::abs(imageScaleY-img_scale)>1e-6)
    {
        const double ratio=imageScaleY/img_scale;
        for(auto& detection:detections){detection.box.y=static_cast<int>(detection.box.y*ratio);detection.box.height=static_cast<int>(detection.box.height*ratio);}
    }
    return detections;
}
#endif
