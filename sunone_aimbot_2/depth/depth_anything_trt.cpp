#ifdef USE_CUDA

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "depth_anything_trt.h"
#include "model_path.h"

#include <NvOnnxParser.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

#include "other_tools.h"
#include "tensorrt/trt_monitor.h"

namespace depth_anything
{
    namespace
    {
        constexpr bool kEnableFp16 = true;
        constexpr int kMinInputSize = 160;
        constexpr int kMaxInputSize = 640;
        constexpr int kOptInputSize = 224;

        bool TensorDimToInt(int64_t value, int& out)
        {
            if (value <= 0 || value > static_cast<int64_t>(std::numeric_limits<int>::max()))
            {
                return false;
            }

            out = static_cast<int>(value);
            return true;
        }

        std::string MakeEnginePathFromOnnx(const std::string& onnxPath)
        {
            std::filesystem::path enginePath=std::filesystem::u8path(onnxPath);
            enginePath.replace_extension(".engine");
            return enginePath.u8string();
        }

        bool CheckCuda(cudaError_t status, const char* action, std::string& last_error)
        {
            if (status == cudaSuccess)
            {
                return true;
            }
            last_error = std::string(action) + ": " + cudaGetErrorString(status);
            return false;
        }
    }

    DepthAnythingTrt::DepthAnythingTrt()
        : input_w(kOptInputSize)
        , input_h(kOptInputSize)
        , min_input_size(kMinInputSize)
        , max_input_size(kMaxInputSize)
        , dynamic_input(false)
        , mean{ 123.675f, 116.28f, 103.53f }
        , stddev{ 58.395f, 57.12f, 57.375f }
        , colormap_type(COLORMAP_TWILIGHT)
        , runtime(nullptr)
        , engine(nullptr)
        , context(nullptr)
        , input_name()
        , output_name()
        , input_buffer(nullptr)
        , output_buffer(nullptr)
        , input_capacity(0)
        , output_capacity(0)
        , output_w(0)
        , output_h(0)
        , stream(nullptr)
        , initialized(false)
    {
    }

    DepthAnythingTrt::~DepthAnythingTrt()
    {
        reset();
    }

    bool DepthAnythingTrt::ready() const
    {
        return initialized;
    }

    const std::string& DepthAnythingTrt::lastError() const
    {
        return last_error;
    }

    void DepthAnythingTrt::setColormap(int type)
    {
        if (type < COLORMAP_AUTUMN || type > COLORMAP_DEEPGREEN)
        {
            colormap_type = COLORMAP_TWILIGHT;
            return;
        }
        colormap_type = type;
    }

    int DepthAnythingTrt::colormapType() const
    {
        return colormap_type;
    }

    void DepthAnythingTrt::reset()
    {
        initialized = false;
        last_error.clear();

        if (stream)
        {
            cudaStreamDestroy(stream);
            stream = nullptr;
        }

        if (input_buffer)
        {
            cudaFree(input_buffer);
            input_buffer = nullptr;
        }
        if (output_buffer)
        {
            cudaFree(output_buffer);
            output_buffer = nullptr;
        }
        input_name.clear();
        output_name.clear();
        input_capacity = 0;
        output_capacity = 0;
        output_w = 0;
        output_h = 0;

        depth_data.clear();
        context.reset();
        engine.reset();
        runtime.reset();
    }

    bool DepthAnythingTrt::initialize(const std::string& modelPath, nvinfer1::ILogger& logger)
    {
        reset();
        dynamic_input = false;
        min_input_size = kMinInputSize;
        max_input_size = kMaxInputSize;

        std::string resolvedPath;
        if (!resolveModelPath(modelPath, resolvedPath, last_error))
        {
            return false;
        }

        if (!std::filesystem::exists(std::filesystem::u8path(resolvedPath)))
        {
            last_error = "Depth model file not found: " + resolvedPath;
            return false;
        }

        if (!loadEngine(resolvedPath, logger))
        {
            if (last_error.empty())
            {
                last_error = "Failed to load depth model: " + resolvedPath;
            }
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        const int nb_io = engine->getNbIOTensors();
        if (nb_io <= 0)
        {
            last_error = "Depth engine has no I/O tensors.";
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        int input_count = 0;
        int output_count = 0;
        for (int i = 0; i < nb_io; i++)
        {
            const char* name = engine->getIOTensorName(i);
            if (!name)
            {
                last_error = "Depth engine has invalid tensor name.";
                auto err = last_error;
                reset();
                last_error = err;
                return false;
            }

            if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT)
            {
                input_count++;
                if (input_name.empty())
                {
                    input_name = name;
                }
            }
            else
            {
                output_count++;
                if (output_name.empty())
                {
                    output_name = name;
                }
            }
        }

        if (input_count != 1 || output_count != 1 || input_name.empty() || output_name.empty())
        {
            last_error = "Depth engine must have exactly 1 input and 1 output; got " + std::to_string(input_count) + " inputs and " + std::to_string(output_count) + " outputs.";
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        auto input_dims = engine->getTensorShape(input_name.c_str());
        if (input_dims.nbDims < 3)
        {
            last_error = "Depth input dimensions are invalid.";
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }
        bool has_dynamic = false;
        for (int i = 0; i < input_dims.nbDims; i++)
        {
            if (input_dims.d[i] == -1)
            {
                has_dynamic = true;
                break;
            }
        }

        if (input_dims.nbDims >= 3 && input_dims.d[input_dims.nbDims - 3] > 0 && input_dims.d[input_dims.nbDims - 3] != 3)
        {
            last_error = "Depth input must have 3 channels.";
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        if (has_dynamic)
        {
            dynamic_input = true;
            min_input_size = kMinInputSize;
            max_input_size = kMaxInputSize;
            input_h = max_input_size;
            input_w = max_input_size;
            if (!setInputShape(input_w, input_h))
            {
                auto err = last_error;
                reset();
                last_error = err;
                return false;
            }
        }
        else
        {
            if (!TensorDimToInt(input_dims.d[input_dims.nbDims - 2], input_h) ||
                !TensorDimToInt(input_dims.d[input_dims.nbDims - 1], input_w))
            {
                last_error = "Depth input dimensions are invalid.";
                auto err = last_error;
                reset();
                last_error = err;
                return false;
            }
            min_input_size = input_w;
            max_input_size = input_w;
        }

        if (!CheckCuda(cudaStreamCreate(&stream), "cudaStreamCreate", last_error))
        {
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        const size_t input_elements = static_cast<size_t>(3) * static_cast<size_t>(input_h) * static_cast<size_t>(input_w);
        if (!ensureInputCapacity(input_elements))
        {
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        int out_h = 0;
        int out_w = 0;
        size_t output_elements = 0;
        if (!getOutputShape(out_h, out_w, output_elements))
        {
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }
        if (!ensureOutputCapacity(output_elements))
        {
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        if (!setTensorAddresses())
        {
            auto err = last_error;
            reset();
            last_error = err;
            return false;
        }

        initialized = true;
        return true;
    }

    bool DepthAnythingTrt::exportEngine(const std::string& modelPath, nvinfer1::ILogger& logger, std::string* enginePath)
    {
        std::lock_guard<std::recursive_mutex> buildLock(gTrtBuildMutex);
        reset();

        std::string resolvedPath;
        if (!resolveModelPath(modelPath, resolvedPath, last_error))
        {
            return false;
        }

        if (!OtherTools::HasExtensionCaseInsensitive(resolvedPath, ".onnx"))
        {
            last_error = "Depth export expects an .onnx model path.";
            return false;
        }

        if (!std::filesystem::exists(std::filesystem::u8path(resolvedPath)))
        {
            last_error = "Depth model file not found: " + resolvedPath;
            return false;
        }

        if (!buildEngine(resolvedPath, logger))
        {
            if (last_error.empty())
            {
                last_error = "Failed to build depth engine from ONNX: " + resolvedPath;
            }
            return false;
        }

        return saveEngine(resolvedPath, enginePath);
    }

    bool DepthAnythingTrt::preprocess(const cv::Mat& image, std::vector<float>& input_tensor)
    {
        if (image.empty())
        {
            last_error = "Depth input image is empty.";
            return false;
        }

        cv::Mat input_image;
        if (image.channels() == 3)
        {
            input_image = image;
        }
        else if (image.channels() == 4)
        {
            cv::cvtColor(image, input_image, cv::COLOR_BGRA2BGR);
        }
        else if (image.channels() == 1)
        {
            cv::cvtColor(image, input_image, cv::COLOR_GRAY2BGR);
        }
        else
        {
            last_error = "Depth input image must have 1, 3, or 4 channels.";
            return false;
        }

        if (input_image.depth() != CV_8U)
        {
            cv::Mat converted;
            input_image.convertTo(converted, CV_8U);
            input_image = converted;
        }

        auto resized = resize_depth(input_image, input_w, input_h);
        cv::Mat resized_image = std::get<0>(resized);
        if (resized_image.empty())
        {
            last_error = "Failed to resize depth input image.";
            return false;
        }
        if (resized_image.rows != input_h || resized_image.cols != input_w)
        {
            last_error = "Depth input resize produced unexpected dimensions.";
            return false;
        }

        input_tensor.resize(static_cast<size_t>(3) * static_cast<size_t>(input_h) * static_cast<size_t>(input_w));
        size_t idx = 0;
        for (int k = 0; k < 3; k++)
        {
            for (int i = 0; i < resized_image.rows; i++)
            {
                const cv::Vec3b* row = resized_image.ptr<cv::Vec3b>(i);
                for (int j = 0; j < resized_image.cols; j++)
                {
                    input_tensor[idx++] = (static_cast<float>(row[j][k]) - mean[k]) / stddev[k];
                }
            }
        }
        return true;
    }

    bool DepthAnythingTrt::runInference(const cv::Mat& image, cv::Mat& depth_norm)
    {
        if (!initialized || image.empty())
        {
            return false;
        }

        int target_size = selectInputSize(image);
        if (dynamic_input && (target_size != input_w || target_size != input_h))
        {
            if (!setInputShape(target_size, target_size))
            {
                return false;
            }
        }

        std::vector<float> input;
        if (!preprocess(image, input))
        {
            return false;
        }
        if (!ensureInputCapacity(input.size()))
        {
            return false;
        }

        int out_h = 0;
        int out_w = 0;
        size_t output_elements = 0;
        if (!getOutputShape(out_h, out_w, output_elements))
        {
            return false;
        }
        if (!ensureOutputCapacity(output_elements))
        {
            return false;
        }

        if (!setTensorAddresses())
        {
            return false;
        }

        const size_t input_bytes = input.size() * sizeof(float);
        if (!CheckCuda(cudaMemcpyAsync(input_buffer, input.data(), input_bytes, cudaMemcpyHostToDevice, stream), "cudaMemcpyAsync input", last_error))
        {
            return false;
        }

        if (!context->enqueueV3(stream))
        {
            last_error = "Failed to execute depth inference.";
            return false;
        }

        const size_t output_bytes = output_elements * sizeof(float);
        if (!CheckCuda(cudaMemcpyAsync(depth_data.data(), output_buffer, output_bytes, cudaMemcpyDeviceToHost, stream), "cudaMemcpyAsync output", last_error))
        {
            return false;
        }
        if (!CheckCuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize", last_error))
        {
            return false;
        }

        cv::Mat depth_mat(out_h, out_w, CV_32FC1, depth_data.data());
        cv::normalize(depth_mat, depth_norm, 0, 255, cv::NORM_MINMAX, CV_8U);
        return true;
    }

    cv::Mat DepthAnythingTrt::predict(const cv::Mat& image)
    {
        cv::Mat depth_norm;
        if (!runInference(image, depth_norm))
        {
            return {};
        }

        cv::Mat colormap;
        cv::applyColorMap(depth_norm, colormap, colormap_type);

        cv::Mat output;
        cv::resize(colormap, output, image.size());
        return output;
    }

    cv::Mat DepthAnythingTrt::predictDepth(const cv::Mat& image)
    {
        cv::Mat depth_norm;
        if (!runInference(image, depth_norm))
        {
            return {};
        }

        cv::Mat output;
        cv::resize(depth_norm, output, image.size(), 0.0, 0.0, cv::INTER_LINEAR);
        return output;
    }

    int DepthAnythingTrt::selectInputSize(const cv::Mat& image) const
    {
        if (min_input_size <= 0 || max_input_size <= 0)
        {
            return input_w;
        }

        int long_side = std::max(image.cols, image.rows);
        return std::clamp(long_side, min_input_size, max_input_size);
    }

    bool DepthAnythingTrt::setInputShape(int w, int h)
    {
        if (input_name.empty())
        {
            last_error = "Invalid depth input tensor name.";
            return false;
        }
        if (!context->setInputShape(input_name.c_str(), nvinfer1::Dims4{ 1, 3, h, w }))
        {
            last_error = "Failed to set depth input shape.";
            return false;
        }

        input_w = w;
        input_h = h;
        return true;
    }

    bool DepthAnythingTrt::loadEngine(const std::string& modelPath, nvinfer1::ILogger& logger)
    {
        std::lock_guard<std::recursive_mutex> buildLock(gTrtBuildMutex);
        if (OtherTools::HasExtensionCaseInsensitive(modelPath, ".onnx"))
        {
            if (!buildEngine(modelPath, logger))
            {
                return false;
            }
            if (!saveEngine(modelPath))
            {
                return false;
            }
            return true;
        }

        std::ifstream engineStream(std::filesystem::u8path(modelPath), std::ios::binary);
        if (!engineStream.is_open())
        {
            last_error = "Unable to open depth engine: " + modelPath;
            return false;
        }

        engineStream.seekg(0, std::ios::end);
        const std::streampos endPos = engineStream.tellg();
        if (endPos <= 0)
        {
            last_error = "Depth engine file is empty: " + modelPath;
            return false;
        }
        const size_t modelSize = static_cast<size_t>(endPos);
        engineStream.seekg(0, std::ios::beg);
        std::vector<char> engineData(modelSize);
        engineStream.read(engineData.data(), modelSize);
        const bool read_ok = engineStream.good() || engineStream.eof();
        engineStream.close();
        if (!read_ok)
        {
            last_error = "Failed to read depth engine: " + modelPath;
            return false;
        }

        runtime.reset(nvinfer1::createInferRuntime(logger));
        if (!runtime)
        {
            last_error = "Failed to create depth runtime.";
            return false;
        }
        engine.reset(runtime->deserializeCudaEngine(engineData.data(), modelSize));
        if (!engine)
        {
            last_error = "Failed to deserialize depth engine: " + modelPath;
            return false;
        }
        context.reset(engine->createExecutionContext());
        if (!context)
        {
            last_error = "Failed to create depth execution context.";
            return false;
        }
        return true;
    }

    bool DepthAnythingTrt::buildEngine(const std::string& onnxPath, nvinfer1::ILogger& logger)
    {
        std::lock_guard<std::recursive_mutex> buildLock(gTrtBuildMutex);
        auto builder = nvinfer1::createInferBuilder(logger);
        if (!builder)
        {
            last_error = "Failed to create TensorRT builder.";
            return false;
        }

        const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
        nvinfer1::INetworkDefinition* network = builder->createNetworkV2(explicitBatch);
        nvinfer1::IBuilderConfig* config = builder->createBuilderConfig();
        if (!network || !config)
        {
            last_error = "Failed to create TensorRT network or config.";
            delete config;
            delete network;
            delete builder;
            return false;
        }

        ImGuiProgressMonitor progressMonitor;
        config->setProgressMonitor(&progressMonitor);
        TrtExportResetState();
        gIsTrtExporting = true;

        struct ScopedExportState
        {
            ~ScopedExportState()
            {
                std::lock_guard<std::mutex> lock(gProgressMutex);
                gProgressPhases.clear();
                gIsTrtExporting = false;
                gTrtExportCancelRequested = false;
                gTrtExportLastUpdateMs = TrtNowMs();
            }
        } exportState;

        if (kEnableFp16)
        {
            config->setFlag(nvinfer1::BuilderFlag::kFP16);
        }

        nvonnxparser::IParser* parser = nvonnxparser::createParser(*network, logger);
        if (!parser)
        {
            last_error = "Failed to create ONNX parser.";
            delete config;
            delete network;
            delete builder;
            return false;
        }
        if (!parser->parseFromFile(onnxPath.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kINFO)))
        {
            last_error = "Failed to parse depth ONNX model.";
            delete parser;
            delete config;
            delete network;
            delete builder;
            return false;
        }

        auto input = network->getInput(0);
        if (input)
        {
            auto input_dims = input->getDimensions();
            bool has_dynamic = false;
            for (int i = 0; i < input_dims.nbDims; i++)
            {
                if (input_dims.d[i] == -1)
                {
                    has_dynamic = true;
                    break;
                }
            }

            if (has_dynamic)
            {
                auto profile = builder->createOptimizationProfile();
                int opt_size = std::clamp(kOptInputSize, kMinInputSize, kMaxInputSize);
                const char* input_tensor_name = input->getName();
                bool ok = profile->setDimensions(input_tensor_name, nvinfer1::OptProfileSelector::kMIN, nvinfer1::Dims4{ 1, 3, kMinInputSize, kMinInputSize });
                ok = ok && profile->setDimensions(input_tensor_name, nvinfer1::OptProfileSelector::kOPT, nvinfer1::Dims4{ 1, 3, opt_size, opt_size });
                ok = ok && profile->setDimensions(input_tensor_name, nvinfer1::OptProfileSelector::kMAX, nvinfer1::Dims4{ 1, 3, kMaxInputSize, kMaxInputSize });
                if (!ok || !profile->isValid())
                {
                    last_error = "Failed to set depth input optimization profile.";
                    delete parser;
                    delete config;
                    delete network;
                    delete builder;
                    return false;
                }
                config->addOptimizationProfile(profile);
            }
        }

        nvinfer1::IHostMemory* plan = builder->buildSerializedNetwork(*network, *config);
        if (!plan)
        {
            last_error = gTrtExportCancelRequested.load()
                ? "Depth export canceled."
                : "Failed to build depth engine from ONNX.";
            delete parser;
            delete config;
            delete network;
            delete builder;
            return false;
        }

        runtime.reset(nvinfer1::createInferRuntime(logger));
        if (!runtime)
        {
            last_error = "Failed to create depth runtime.";
            delete plan;
            delete parser;
            delete config;
            delete network;
            delete builder;
            return false;
        }
        engine.reset(runtime->deserializeCudaEngine(plan->data(), plan->size()));
        if (!engine)
        {
            last_error = "Failed to deserialize depth engine.";
            delete plan;
            delete parser;
            delete config;
            delete network;
            delete builder;
            return false;
        }
        context.reset(engine->createExecutionContext());

        delete plan;
        delete parser;
        delete config;
        delete network;
        delete builder;

        if (!engine || !context)
        {
            last_error = "Failed to build depth engine from ONNX.";
            return false;
        }

        return true;
    }

    bool DepthAnythingTrt::saveEngine(const std::string& onnxPath, std::string* enginePath)
    {
        if (!engine)
        {
            last_error = "Depth engine is not available for serialization.";
            return false;
        }

        std::string engine_path = MakeEnginePathFromOnnx(onnxPath);
        if (engine_path.empty() || engine_path == onnxPath)
        {
            last_error = "Failed to resolve depth engine output path.";
            return false;
        }

        nvinfer1::IHostMemory* data = engine->serialize();
        if (!data)
        {
            last_error = "Failed to serialize depth engine.";
            return false;
        }
        std::ofstream file(std::filesystem::u8path(engine_path), std::ios::binary | std::ios::out);
        if (!file.is_open())
        {
            last_error = "Unable to write depth engine: " + engine_path;
            delete data;
            return false;
        }

        file.write(reinterpret_cast<const char*>(data->data()), data->size());
        if (!file.good())
        {
            last_error = "Failed to write depth engine: " + engine_path;
            file.close();
            delete data;
            return false;
        }
        file.close();
        delete data;

        if (enginePath)
        {
            *enginePath = engine_path;
        }
        last_error.clear();
        return true;
    }

    bool DepthAnythingTrt::getOutputShape(int& out_h, int& out_w, size_t& out_elements)
    {
        if (output_name.empty())
        {
            last_error = "Invalid depth output tensor name.";
            return false;
        }

        auto output_dims = context->getTensorShape(output_name.c_str());
        if (output_dims.nbDims < 2)
        {
            last_error = "Depth output dimensions are invalid.";
            return false;
        }

        size_t elements = 1;
        for (int i = 0; i < output_dims.nbDims; i++)
        {
            if (output_dims.d[i] <= 0)
            {
                last_error = "Depth output dimensions are not fully specified.";
                return false;
            }
            elements *= static_cast<size_t>(output_dims.d[i]);
        }

        if (!TensorDimToInt(output_dims.d[output_dims.nbDims - 2], out_h) ||
            !TensorDimToInt(output_dims.d[output_dims.nbDims - 1], out_w))
        {
            last_error = "Depth output dimensions are invalid.";
            return false;
        }

        const size_t spatial = static_cast<size_t>(out_h) * static_cast<size_t>(out_w);
        if (spatial == 0 || elements % spatial != 0 || (elements / spatial) != 1)
        {
            last_error = "Depth output must be single-channel.";
            return false;
        }

        out_elements = elements;
        output_h = out_h;
        output_w = out_w;
        return true;
    }

    bool DepthAnythingTrt::ensureInputCapacity(size_t elements)
    {
        if (elements == 0)
        {
            last_error = "Depth input size is zero.";
            return false;
        }
        if (elements <= input_capacity && input_buffer)
        {
            return true;
        }
        if (input_buffer)
        {
            cudaFree(input_buffer);
            input_buffer = nullptr;
        }
        if (!CheckCuda(cudaMalloc(&input_buffer, elements * sizeof(float)), "cudaMalloc input", last_error))
        {
            return false;
        }
        input_capacity = elements;
        return true;
    }

    bool DepthAnythingTrt::ensureOutputCapacity(size_t elements)
    {
        if (elements == 0)
        {
            last_error = "Depth output size is zero.";
            return false;
        }
        if (elements <= output_capacity && output_buffer)
        {
            if (depth_data.size() < output_capacity)
            {
                depth_data.resize(output_capacity);
            }
            return true;
        }
        if (output_buffer)
        {
            cudaFree(output_buffer);
            output_buffer = nullptr;
        }
        if (!CheckCuda(cudaMalloc(&output_buffer, elements * sizeof(float)), "cudaMalloc output", last_error))
        {
            return false;
        }
        output_capacity = elements;
        if (depth_data.size() < output_capacity)
        {
            depth_data.resize(output_capacity);
        }
        return true;
    }

    bool DepthAnythingTrt::setTensorAddresses()
    {
        if (input_name.empty() || output_name.empty())
        {
            last_error = "Depth tensor names are not set.";
            return false;
        }
        if (!input_buffer || !output_buffer)
        {
            last_error = "Depth tensor buffers are not allocated.";
            return false;
        }
        if (!context->setTensorAddress(input_name.c_str(), input_buffer))
        {
            last_error = "Failed to set depth input tensor address.";
            return false;
        }
        if (!context->setTensorAddress(output_name.c_str(), output_buffer))
        {
            last_error = "Failed to set depth output tensor address.";
            return false;
        }
        return true;
    }
}

#endif
