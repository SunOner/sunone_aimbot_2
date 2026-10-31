#ifdef USE_CUDA
#ifndef NVINF_H
#define NVINF_H

#include "NvInfer.h"
#include <string>

class Logger : public nvinfer1::ILogger
{
public:
    void log(Severity severity, const char* msg) noexcept override;
    static const char* severityLevelName(Severity severity);
};

extern Logger gLogger;

nvinfer1::IBuilder* createInferBuilder();
nvinfer1::INetworkDefinition* createNetwork(nvinfer1::IBuilder* builder);
nvinfer1::IBuilderConfig* createBuilderConfig(nvinfer1::IBuilder* builder);

struct TrtBuildOptions
{
    bool fixedInputSize = false;
    bool enableFp16 = true;
    bool enableFp8 = false;
    int inputWidth = 320;
    int inputHeight = 320;
    int minProfileSize = 160;
    int optProfileSize = 320;
    int maxProfileSize = 640;
};

nvinfer1::ICudaEngine* loadEngineFromFile(const std::string& engineFile, nvinfer1::IRuntime* runtime);
std::string trtEngineCachePath(const std::string& onnxFile, const TrtBuildOptions& options);
nvinfer1::ICudaEngine* buildEngineFromOnnx(const std::string& onnxFile, nvinfer1::ILogger& logger,
    const TrtBuildOptions& options, nvinfer1::IRuntime& runtime);

#endif // NVINF_H
#endif
