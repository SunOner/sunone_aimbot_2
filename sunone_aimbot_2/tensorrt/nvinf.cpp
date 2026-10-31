#ifdef USE_CUDA
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <NvInferVersion.h>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>

#include "nvinf.h"
#include "trt_monitor.h"

Logger gLogger;

void Logger::log(nvinfer1::ILogger::Severity severity, const char* msg) noexcept
{
    if (severity <= nvinfer1::ILogger::Severity::kWARNING)
    {
        std::string devMsg = msg;

        std::string magicTag = "Serialization assertion plan->header.magicTag == rt::kPLAN_MAGIC_TAG failed.";
        std::string old_deserialization = "Using old deserialization call on a weight-separated plan file.";
        if (devMsg.find(magicTag) != std::string::npos || devMsg.find(old_deserialization) != std::string::npos)
        {
            std::cout << "[TensorRT] ERROR: This engine model is not suitable for execution. Please delete this engine model and set the ONNX version of this model in the settings. The program will export the model automatically." << std::endl;
        }
        else
        {
            std::cout << "[TensorRT] " << severityLevelName(severity) << ": " << msg << std::endl;
        }
    }
}

const char* Logger::severityLevelName(nvinfer1::ILogger::Severity severity)
{
    switch (severity)
    {
        case nvinfer1::ILogger::Severity::kINTERNAL_ERROR: return "INTERNAL_ERROR";
        case nvinfer1::ILogger::Severity::kERROR:          return "ERROR";
        case nvinfer1::ILogger::Severity::kWARNING:        return "WARNING";
        case nvinfer1::ILogger::Severity::kINFO:           return "INFO";
        case nvinfer1::ILogger::Severity::kVERBOSE:        return "VERBOSE";
        default:                                           return "UNKNOWN";
    }
}

nvinfer1::IBuilder* createInferBuilder()
{
    return nvinfer1::createInferBuilder(gLogger);
}

nvinfer1::INetworkDefinition* createNetwork(nvinfer1::IBuilder* builder)
{
    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    return builder->createNetworkV2(explicitBatch);
}

nvinfer1::IBuilderConfig* createBuilderConfig(nvinfer1::IBuilder* builder)
{
    return builder->createBuilderConfig();
}

nvinfer1::ICudaEngine* loadEngineFromFile(const std::string& engineFile, nvinfer1::IRuntime* runtime)
{
    if(!runtime)return nullptr;
    std::ifstream file(std::filesystem::u8path(engineFile), std::ios::binary);
    if (!file.good())
    {
        std::cerr << "[TensorRT] Error opening the engine file: " << engineFile << std::endl;
        return nullptr;
    }

    file.seekg(0, std::ios::end);
    const auto length=file.tellg();
    if(length<=0)return nullptr;
    const size_t size=static_cast<size_t>(length);
    file.seekg(0, std::ios::beg);
    std::vector<char> engineData(size);
    if(!file.read(engineData.data(), static_cast<std::streamsize>(size)))return nullptr;
    file.close();

    nvinfer1::ICudaEngine* engine = runtime->deserializeCudaEngine(engineData.data(), size);
    if (!engine)
    {
        std::cerr << "[TensorRT] Engine deserialization error from file: " << engineFile << std::endl;
        return nullptr;
    }

    return engine;
}

std::string trtEngineCachePath(const std::string& onnxFile, const TrtBuildOptions& options)
{
    std::ostringstream identity;
    identity << NV_TENSORRT_MAJOR << '.' << NV_TENSORRT_MINOR << '.' << NV_TENSORRT_PATCH
        << '|' << options.fixedInputSize << '|' << options.enableFp16 << '|' << options.enableFp8
        << '|' << options.minProfileSize << '|' << options.optProfileSize << '|' << options.maxProfileSize;
    if(options.fixedInputSize)identity << '|' << options.inputWidth << 'x' << options.inputHeight;
    std::error_code error;
    const std::filesystem::path path=std::filesystem::u8path(onnxFile);
    const auto modified=std::filesystem::last_write_time(path,error);
    if(!error)identity << '|' << modified.time_since_epoch().count();
    error.clear();
    const auto bytes=std::filesystem::file_size(path,error);
    if(!error)identity << '|' << bytes;
    int device=0;cudaDeviceProp properties{};
    if(cudaGetDevice(&device)==cudaSuccess&&cudaGetDeviceProperties(&properties,device)==cudaSuccess)
        identity << '|' << properties.name << '|' << properties.major << '.' << properties.minor;
    std::uint64_t hash=14695981039346656037ull;
    for(const unsigned char value:identity.str()){hash^=value;hash*=1099511628211ull;}
    std::ostringstream suffix;suffix << ".nodes-" << std::hex << hash << ".engine";
    return (path.parent_path()/(path.stem().string()+suffix.str())).string();
}

nvinfer1::ICudaEngine* buildEngineFromOnnx(const std::string& onnxFile,nvinfer1::ILogger& logger,
    const TrtBuildOptions& options,nvinfer1::IRuntime& runtime)
{
    if(options.inputWidth<=0||options.inputHeight<=0||options.minProfileSize<=0||
        options.minProfileSize>options.optProfileSize||options.optProfileSize>options.maxProfileSize)
        throw std::runtime_error("TensorRT profile requires minimum <= optimal <= maximum and positive input dimensions.");
    std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(logger));
    if(!builder)throw std::runtime_error("TensorRT builder creation failed.");
    const auto explicitBatch=1U<<static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
    std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(explicitBatch));
    if(!network)throw std::runtime_error("TensorRT network creation failed.");
    ImGuiProgressMonitor progressMonitor;
    std::unique_ptr<nvinfer1::IBuilderConfig> settings(builder->createBuilderConfig());
    if(!settings)throw std::runtime_error("TensorRT build configuration creation failed.");
    std::unique_ptr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network,logger));
    if(!parser)throw std::runtime_error("ONNX parser creation failed.");
    settings->setProgressMonitor(&progressMonitor);
    TrtExportResetState();gIsTrtExporting=true;
    struct ScopedExportState
    {
        ~ScopedExportState()
        {
            std::lock_guard<std::mutex> lock(gProgressMutex);
            gProgressPhases.clear();gIsTrtExporting=false;gTrtExportCancelRequested=false;
            gTrtExportLastUpdateMs=TrtNowMs();
        }
    } exportState;
    if(!parser->parseFromFile(onnxFile.c_str(),static_cast<int>(nvinfer1::ILogger::Severity::kWARNING)))
        throw std::runtime_error("TensorRT could not parse this ONNX model.");
    if(network->getNbInputs()!=1)throw std::runtime_error("Inference requires a model with one image input.");
    auto* input=network->getInput(0);const auto dimensions=input->getDimensions();
    if(dimensions.nbDims!=4||(dimensions.d[0]>0&&dimensions.d[0]!=1)||(dimensions.d[1]>0&&dimensions.d[1]!=3))
        throw std::runtime_error("Inference requires a single NCHW RGB image input.");
    const int height=dimensions.d[2],width=dimensions.d[3];
    const bool fixed=(height>0&&width>0)||options.fixedInputSize;
    // TensorRT's builder owns optimization profiles.
    auto* profile=builder->createOptimizationProfile();
    if(!profile)throw std::runtime_error("TensorRT optimization profile creation failed.");
    auto shape=[&](int size){return nvinfer1::Dims4{1,3,height>0?height:size,width>0?width:size};};
    const auto minimum=fixed?nvinfer1::Dims4{1,3,height>0?height:options.inputHeight,width>0?width:options.inputWidth}:shape(options.minProfileSize);
    const auto optimal=fixed?minimum:shape(options.optProfileSize);
    const auto maximum=fixed?minimum:shape(options.maxProfileSize);
    if(!profile->setDimensions(input->getName(),nvinfer1::OptProfileSelector::kMIN,minimum)||
        !profile->setDimensions(input->getName(),nvinfer1::OptProfileSelector::kOPT,optimal)||
        !profile->setDimensions(input->getName(),nvinfer1::OptProfileSelector::kMAX,maximum)||
        settings->addOptimizationProfile(profile)<0)
        throw std::runtime_error("TensorRT rejected this node's input profile.");
    if(options.enableFp16)settings->setFlag(nvinfer1::BuilderFlag::kFP16);
    if(options.enableFp8)settings->setFlag(nvinfer1::BuilderFlag::kFP8);
    std::cout << "[TensorRT] Building engine with the Inference node's profile and precision..." << std::endl;
    std::unique_ptr<nvinfer1::IHostMemory> plan(builder->buildSerializedNetwork(*network,*settings));
    if(!plan)throw std::runtime_error("TensorRT engine build failed or was cancelled.");
    auto* engine=runtime.deserializeCudaEngine(plan->data(),plan->size());
    if(!engine)throw std::runtime_error("TensorRT could not deserialize the built engine.");
    return engine;
}
#endif
