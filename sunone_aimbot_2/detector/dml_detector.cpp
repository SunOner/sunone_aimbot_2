#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <dml_provider_factory.h>
#include <wrl/client.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <limits>
#include <algorithm>
#include <cmath>
#include <dxgi.h>
#include <utility>

#include "dml_detector.h"
#include "sunone_aimbot_2.h"
#include "scr/data_collector.h"
#include "postProcess.h"
#include "capture.h"
#include "capture/circle_fov.h"
#include "other_tools.h"
#ifdef USE_CUDA
#include "depth/depth_mask.h"
#endif

extern std::atomic<bool> detector_model_changed;
extern std::atomic<bool> detection_resolution_changed;

namespace
{
bool tryInt64ToInt(int64_t value, int* out)
{
    if (!out)
    {
        return false;
    }

    if (value < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<int64_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    *out = static_cast<int>(value);
    return true;
}

float sigmoidFloat(float x)
{
    if (x >= 0.0f)
    {
        const float z = std::exp(-x);
        return 1.0f / (1.0f + z);
    }
    const float z = std::exp(x);
    return z / (1.0f + z);
}

float softplusFloat(float x)
{
    if (x > 20.0f) return x;
    if (x < -20.0f) return std::exp(x);
    return std::log1p(std::exp(x));
}

bool isShape4(const std::vector<int64_t>& shape)
{
    return shape.size() == 4 && shape[0] > 0 && shape[1] > 0 && shape[2] > 0 && shape[3] > 0;
}

size_t nchwOffset(int batch, int channel, int y, int x, int channels, int height, int width)
{
    return (((static_cast<size_t>(batch) * static_cast<size_t>(channels) + static_cast<size_t>(channel))
        * static_cast<size_t>(height) + static_cast<size_t>(y)) * static_cast<size_t>(width))
        + static_cast<size_t>(x);
}

int findOutputIndex(const std::vector<std::string>& names, const char* wanted)
{
    for (int i = 0; i < static_cast<int>(names.size()); ++i)
    {
        if (names[i] == wanted)
            return i;
    }
    return -1;
}

OrtLoggingLevel getDmlOrtLogLevel()
{
    return config.verbose ? ORT_LOGGING_LEVEL_WARNING : ORT_LOGGING_LEVEL_ERROR;
}

std::vector<Detection> decodeSunPointRaw(
    const float* heat,
    const std::vector<int64_t>& heatShape,
    const float* box,
    const std::vector<int64_t>& boxShape,
    const float* offset,
    const std::vector<int64_t>& offsetShape,
    int batchIndex,
    int targetW,
    int targetH,
    float confThreshold,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime)
{
    std::vector<Detection> detections;
    if (!heat || !box || !offset || !isShape4(heatShape) || !isShape4(boxShape) || !isShape4(offsetShape))
        return detections;

    const int batch = static_cast<int>(heatShape[0]);
    const int classes = static_cast<int>(heatShape[1]);
    const int gridH = static_cast<int>(heatShape[2]);
    const int gridW = static_cast<int>(heatShape[3]);
    if (batchIndex < 0 || batchIndex >= batch || classes <= 0 || gridH <= 0 || gridW <= 0)
        return detections;
    if (boxShape[0] != heatShape[0] || boxShape[1] != 4 || boxShape[2] != heatShape[2] || boxShape[3] != heatShape[3])
        return detections;
    if (offsetShape[0] != heatShape[0] || offsetShape[1] != 2 || offsetShape[2] != heatShape[2] || offsetShape[3] != heatShape[3])
        return detections;

    const float strideX = static_cast<float>(targetW) / static_cast<float>(gridW);
    const float strideY = static_cast<float>(targetH) / static_cast<float>(gridH);
    detections.reserve(static_cast<size_t>(std::max(maxDetections, 16)));

    for (int c = 0; c < classes; ++c)
    {
        for (int y = 0; y < gridH; ++y)
        {
            for (int x = 0; x < gridW; ++x)
            {
                const size_t heatIdx = nchwOffset(batchIndex, c, y, x, classes, gridH, gridW);
                const float heatLogit = heat[heatIdx];
                const float score = sigmoidFloat(heatLogit);
                if (score <= confThreshold)
                    continue;

                bool isPeak = true;
                for (int yy = std::max(0, y - 1); yy <= std::min(gridH - 1, y + 1) && isPeak; ++yy)
                {
                    for (int xx = std::max(0, x - 1); xx <= std::min(gridW - 1, x + 1); ++xx)
                    {
                        if (yy == y && xx == x)
                            continue;
                        const size_t neighborIdx = nchwOffset(batchIndex, c, yy, xx, classes, gridH, gridW);
                        if (heat[neighborIdx] > heatLogit)
                        {
                            isPeak = false;
                            break;
                        }
                    }
                }
                if (!isPeak)
                    continue;

                const float offX = sigmoidFloat(offset[nchwOffset(batchIndex, 0, y, x, 2, gridH, gridW)]);
                const float offY = sigmoidFloat(offset[nchwOffset(batchIndex, 1, y, x, 2, gridH, gridW)]);
                const float centerX = (static_cast<float>(x) + offX) * strideX;
                const float centerY = (static_cast<float>(y) + offY) * strideY;

                const float left = softplusFloat(box[nchwOffset(batchIndex, 0, y, x, 4, gridH, gridW)]) * strideX;
                const float top = softplusFloat(box[nchwOffset(batchIndex, 1, y, x, 4, gridH, gridW)]) * strideY;
                const float right = softplusFloat(box[nchwOffset(batchIndex, 2, y, x, 4, gridH, gridW)]) * strideX;
                const float bottom = softplusFloat(box[nchwOffset(batchIndex, 3, y, x, 4, gridH, gridW)]) * strideY;

                const float x1 = std::clamp(centerX - left, 0.0f, static_cast<float>(targetW));
                const float y1 = std::clamp(centerY - top, 0.0f, static_cast<float>(targetH));
                const float x2 = std::clamp(centerX + right, 0.0f, static_cast<float>(targetW));
                const float y2 = std::clamp(centerY + bottom, 0.0f, static_cast<float>(targetH));
                if (x2 <= x1 || y2 <= y1)
                    continue;

                cv::Rect rect;
                rect.x = static_cast<int>(x1);
                rect.y = static_cast<int>(y1);
                rect.width = std::max(1, static_cast<int>(x2 - x1));
                rect.height = std::max(1, static_cast<int>(y2 - y1));
                detections.push_back(Detection{ rect, score, c });
            }
        }
    }

    if (maxDetections > 0 && detections.size() > static_cast<size_t>(maxDetections))
    {
        const auto kth = detections.begin() + maxDetections;
        std::nth_element(
            detections.begin(),
            kth,
            detections.end(),
            [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
        detections.resize(static_cast<size_t>(maxDetections));
    }

    std::sort(
        detections.begin(),
        detections.end(),
        [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
    NMS(detections, nmsThreshold, nmsTime);
    return detections;
}

#ifdef USE_CUDA
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
#else
void filterDetectionsByDepthMask(std::vector<Detection>&)
{
}
#endif
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

std::string GetDMLDeviceName(int deviceId)
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory))))
        return "Unknown";

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(dxgiFactory->EnumAdapters1(deviceId, &adapter)))
        return "Invalid device ID";

    DXGI_ADAPTER_DESC1 desc;
    if (FAILED(adapter->GetDesc1(&desc)))
        return "Failed to get description";

    std::wstring wname(desc.Description);
    return WideToUtf8(wname);
}

DirectMLDetector::DirectMLDetector(const std::string& model_path)
    :
    env(getDmlOrtLogLevel(), "DML_Detector"),
    memory_info(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
{
    if (!initializeModel(model_path))
    {
        std::cerr << "[DML] Detector started without an active model. "
                  << "Select another ONNX model or fix the model/provider compatibility." << std::endl;
    }
}

DirectMLDetector::~DirectMLDetector()
{
    shouldExit = true;
    inferenceCV.notify_all();
}

bool DirectMLDetector::isReady() const
{
    return model_ready.load();
}

Ort::SessionOptions DirectMLDetector::createSessionOptions(
    bool useDirectML,
    GraphOptimizationLevel graphOptimizationLevel)
{
    Ort::SessionOptions options;
    options.SetLogId("DML_Detector");
    options.SetLogSeverityLevel(static_cast<int>(getDmlOrtLogLevel()));
    options.SetGraphOptimizationLevel(graphOptimizationLevel);
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.DisableMemPattern();
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);

    if (useDirectML)
    {
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options, config.dml_device_id));
    }

    return options;
}

bool DirectMLDetector::tryInitializeModel(
    const std::string& model_path,
    bool useDirectML,
    GraphOptimizationLevel graphOptimizationLevel,
    const char* providerLabel,
    std::string* error)
{
    try
    {
        Ort::SessionOptions options = createSessionOptions(useDirectML, graphOptimizationLevel);
        std::wstring model_path_wide(model_path.begin(), model_path.end());
        Ort::Session newSession(env, model_path_wide.c_str(), options);

        std::string newInputName = newSession.GetInputNameAllocated(0, allocator).get();

        std::vector<std::string> newOutputNames;
        const size_t outputCount = newSession.GetOutputCount();
        newOutputNames.reserve(outputCount);
        for (size_t i = 0; i < outputCount; ++i)
        {
            newOutputNames.emplace_back(newSession.GetOutputNameAllocated(i, allocator).get());
        }

        const int newHeatOutputIndex = findOutputIndex(newOutputNames, "heat");
        const int newBoxOutputIndex = findOutputIndex(newOutputNames, "box");
        const int newOffsetOutputIndex = findOutputIndex(newOutputNames, "offset");
        const bool newSunpointRawOutput =
            newHeatOutputIndex >= 0 &&
            newBoxOutputIndex >= 0 &&
            newOffsetOutputIndex >= 0;

        Ort::TypeInfo input_type_info = newSession.GetInputTypeInfo(0);
        auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
        std::vector<int64_t> newInputShape = input_tensor_info.GetShape();

        int newModelInputH = -1;
        int newModelInputW = -1;
        if (newInputShape.size() > 2)
        {
            int converted = 0;
            if (tryInt64ToInt(newInputShape[2], &converted))
                newModelInputH = converted;
        }
        if (newInputShape.size() > 3)
        {
            int converted = 0;
            if (tryInt64ToInt(newInputShape[3], &converted))
                newModelInputW = converted;
        }

        bool isStatic = true;
        for (auto d : newInputShape) if (d <= 0) isStatic = false;

        session = std::move(newSession);
        input_name = std::move(newInputName);
        output_names = std::move(newOutputNames);
        output_name = output_names.empty() ? std::string() : output_names.front();
        output_name_ptrs.clear();
        output_name_ptrs.reserve(output_names.size());
        for (const auto& name : output_names)
            output_name_ptrs.push_back(name.c_str());

        input_shape = std::move(newInputShape);
        model_input_h = newModelInputH;
        model_input_w = newModelInputW;
        heat_output_index = newHeatOutputIndex;
        box_output_index = newBoxOutputIndex;
        offset_output_index = newOffsetOutputIndex;
        sunpoint_raw_output = newSunpointRawOutput;
        using_directml_provider = useDirectML;
        model_ready.store(true);

        if (isStatic != config.fixed_input_size)
        {
            config.fixed_input_size = isStatic;
            detector_model_changed.store(true);
            std::cout << "[DML] Automatically set fixed_input_size = "
                      << (isStatic ? "true" : "false") << std::endl;
        }

        std::cout << "[DML] Model initialized with " << providerLabel
                  << " provider: " << model_path << std::endl;

        if (useDirectML && config.verbose)
            std::cout << "[DirectML] Using adapter: " << GetDMLDeviceName(config.dml_device_id) << std::endl;

        if (config.verbose)
        {
            std::cout << "[DML] Outputs:";
            for (const auto& name : output_names)
                std::cout << " " << name;
            std::cout << (sunpoint_raw_output ? " (SunPoint raw)" : "") << std::endl;
        }

        return true;
    }
    catch (const Ort::Exception& e)
    {
        if (error) *error = e.what();
    }
    catch (const std::exception& e)
    {
        if (error) *error = e.what();
    }
    catch (...)
    {
        if (error) *error = "unknown exception";
    }

    return false;
}

bool DirectMLDetector::initializeModel(const std::string& model_path)
{
    env.UpdateEnvWithCustomLogLevel(getDmlOrtLogLevel());
    const bool hadReadySession = model_ready.load();

    std::string directMlError;
    if (tryInitializeModel(
        model_path,
        true,
        GraphOptimizationLevel::ORT_ENABLE_ALL,
        "DirectML",
        &directMlError))
    {
        return true;
    }

    std::cerr << "[DML] DirectML initialization failed for " << model_path
              << ": " << directMlError << std::endl;

    std::string directMlCompatError;
    std::cerr << "[DML] Retrying DirectML with compatibility graph optimizations." << std::endl;
    if (tryInitializeModel(
        model_path,
        true,
        GraphOptimizationLevel::ORT_ENABLE_BASIC,
        "DirectML compatibility",
        &directMlCompatError))
    {
        return true;
    }

    std::cerr << "[DML] DirectML compatibility initialization failed for " << model_path
              << ": " << directMlCompatError << std::endl;

    std::string cpuError;
    std::cerr << "[DML] Falling back to ONNX Runtime CPU provider. Detection may be slower." << std::endl;
    if (tryInitializeModel(
        model_path,
        false,
        GraphOptimizationLevel::ORT_ENABLE_ALL,
        "CPU",
        &cpuError))
    {
        return true;
    }

    std::cerr << "[DML] CPU fallback initialization failed for " << model_path
              << ": " << cpuError << std::endl;

    if (hadReadySession)
    {
        std::cerr << "[DML] Keeping the previous working detector session." << std::endl;
    }
    else
    {
        model_ready.store(false);
        using_directml_provider = false;
    }

    return false;
}

void DirectMLDetector::preprocessFrameToTensor(const cv::Mat& frame, float* dst, int target_w, int target_h)
{
    if (!dst || target_w <= 0 || target_h <= 0)
        return;

    const size_t channelSize = static_cast<size_t>(target_w) * static_cast<size_t>(target_h);
    cv::Mat rgbPlanes[3] = {
        cv::Mat(target_h, target_w, CV_32F, dst),
        cv::Mat(target_h, target_w, CV_32F, dst + channelSize),
        cv::Mat(target_h, target_w, CV_32F, dst + channelSize * 2)
    };

    auto clearTensor = [&]()
    {
        rgbPlanes[0].setTo(0.0f);
        rgbPlanes[1].setTo(0.0f);
        rgbPlanes[2].setTo(0.0f);
    };

    if (frame.empty())
    {
        clearTensor();
        return;
    }

    if (frame.channels() == 1)
    {
        cv::Mat grayResized;
        if (frame.cols != target_w || frame.rows != target_h)
        {
            cv::resize(frame, preprocessGrayResizeBuffer, cv::Size(target_w, target_h), 0, 0, cv::INTER_LINEAR);
            grayResized = preprocessGrayResizeBuffer;
        }
        else
        {
            grayResized = frame;
        }

        grayResized.convertTo(preprocessGrayFloatBuffer, CV_32F, 1.0f / 255.0f);
        preprocessGrayFloatBuffer.copyTo(rgbPlanes[0]);
        preprocessGrayFloatBuffer.copyTo(rgbPlanes[1]);
        preprocessGrayFloatBuffer.copyTo(rgbPlanes[2]);
        return;
    }

    cv::Mat bgrFrame;
    switch (frame.channels())
    {
    case 4:
        cv::cvtColor(frame, preprocessBgrBuffer, cv::COLOR_BGRA2BGR);
        bgrFrame = preprocessBgrBuffer;
        break;
    case 3:
        bgrFrame = frame;
        break;
    default:
        clearTensor();
        return;
    }

    cv::Mat resizedBgr;
    if (bgrFrame.cols != target_w || bgrFrame.rows != target_h)
    {
        cv::resize(bgrFrame, preprocessResizeBuffer, cv::Size(target_w, target_h), 0, 0, cv::INTER_LINEAR);
        resizedBgr = preprocessResizeBuffer;
    }
    else
    {
        resizedBgr = bgrFrame;
    }

    resizedBgr.convertTo(preprocessFloatBuffer, CV_32FC3, 1.0f / 255.0f);

    cv::Mat bgrToRgbPlanes[3] = {
        rgbPlanes[2],
        rgbPlanes[1],
        rgbPlanes[0]
    };
    cv::split(preprocessFloatBuffer, bgrToRgbPlanes);
}

std::vector<Detection> DirectMLDetector::detect(const cv::Mat& input_frame)
{
    std::vector<cv::Mat> batch = { input_frame };
    auto batchResult = detectBatch(batch);
    if (!batchResult.empty())
        return batchResult[0];
    else
        return {};
}

std::vector<std::vector<Detection>> DirectMLDetector::detectBatch(const std::vector<cv::Mat>& frames)
{
    std::vector<std::vector<Detection>> empty;
    if (!isReady()) return empty;
    if (frames.empty()) return empty;
    const size_t batch_size = frames.size();

    const int model_h = model_input_h;
    const int model_w = model_input_w;
    const bool useFixed = config.fixed_input_size && model_h > 0 && model_w > 0;

    const int target_h = useFixed ? model_h : config.detection_resolution;
    const int target_w = useFixed ? model_w : config.detection_resolution;

    auto t0 = std::chrono::steady_clock::now();
    const size_t frameTensorSize =
        static_cast<size_t>(3) * static_cast<size_t>(target_h) * static_cast<size_t>(target_w);
    input_tensor_values.resize(batch_size * frameTensorSize);

    for (size_t b = 0; b < batch_size; ++b)
    {
        float* dst = input_tensor_values.data() + b * frameTensorSize;
        preprocessFrameToTensor(frames[b], dst, target_w, target_h);
    }
    auto t1 = std::chrono::steady_clock::now();

    std::vector<int64_t> ort_input_shape{
        static_cast<int64_t>(batch_size),
        3,
        static_cast<int64_t>(target_h),
        static_cast<int64_t>(target_w)
    };
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, input_tensor_values.data(), input_tensor_values.size(),
        ort_input_shape.data(), ort_input_shape.size());

    const char* input_names[] = { input_name.c_str() };
    if (output_name_ptrs.empty())
    {
        std::cerr << "[DirectMLDetector] No output tensors are defined." << std::endl;
        return empty;
    }

    auto t2 = std::chrono::steady_clock::now();
    auto output_tensors = session.Run(Ort::RunOptions{ nullptr },
        input_names, &input_tensor, 1,
        output_name_ptrs.data(), output_name_ptrs.size());
    auto t3 = std::chrono::steady_clock::now();

    std::vector<std::vector<Detection>> batchDetections(batch_size);
    float conf_thr = config.confidence_threshold;
    float nms_thr = config.nms_threshold;
    int max_detections = std::max(1, config.max_detections);

    auto t4 = std::chrono::steady_clock::now();
    std::chrono::duration<double, std::milli> nmsTimeTmp{ 0 };

    if (sunpoint_raw_output)
    {
        const int heatIdx = heat_output_index;
        const int boxIdx = box_output_index;
        const int offsetIdx = offset_output_index;
        if (heatIdx < 0 || boxIdx < 0 || offsetIdx < 0)
        {
            std::cerr << "[DirectMLDetector] SunPoint raw outputs are missing." << std::endl;
            return empty;
        }

        float* heatData = output_tensors[heatIdx].GetTensorMutableData<float>();
        float* boxData = output_tensors[boxIdx].GetTensorMutableData<float>();
        float* offsetData = output_tensors[offsetIdx].GetTensorMutableData<float>();
        std::vector<int64_t> heatShape = output_tensors[heatIdx].GetTensorTypeAndShapeInfo().GetShape();
        std::vector<int64_t> boxShape = output_tensors[boxIdx].GetTensorTypeAndShapeInfo().GetShape();
        std::vector<int64_t> offsetShape = output_tensors[offsetIdx].GetTensorTypeAndShapeInfo().GetShape();

        for (size_t b = 0; b < batch_size; ++b)
        {
            std::vector<Detection> detections = decodeSunPointRaw(
                heatData,
                heatShape,
                boxData,
                boxShape,
                offsetData,
                offsetShape,
                static_cast<int>(b),
                target_w,
                target_h,
                conf_thr,
                nms_thr,
                max_detections,
                &nmsTimeTmp);

            if (useFixed && (target_w != config.detection_resolution || target_h != config.detection_resolution))
            {
                float scaleX = static_cast<float>(config.detection_resolution) / target_w;
                float scaleY = static_cast<float>(config.detection_resolution) / target_h;
                for (auto& d : detections)
                {
                    d.box.x = static_cast<int>(d.box.x * scaleX);
                    d.box.y = static_cast<int>(d.box.y * scaleY);
                    d.box.width = static_cast<int>(d.box.width * scaleX);
                    d.box.height = static_cast<int>(d.box.height * scaleY);
                }
            }

            batchDetections[b] = std::move(detections);
        }

        auto t5 = std::chrono::steady_clock::now();
        lastPreprocessTimeDML = t1 - t0;
        lastInferenceTimeDML = t3 - t2;
        lastCopyTimeDML = t4 - t3;
        lastPostprocessTimeDML = t5 - t4;
        lastNmsTimeDML = nmsTimeTmp;
        return batchDetections;
    }

    float* outData = output_tensors.front().GetTensorMutableData<float>();
    Ort::TensorTypeAndShapeInfo outInfo = output_tensors.front().GetTensorTypeAndShapeInfo();
    std::vector<int64_t> outShape = outInfo.GetShape(); // [B, rows, cols]
    if (outShape.size() < 3)
    {
        std::cerr << "[DirectMLDetector] Unexpected output tensor rank." << std::endl;
        return empty;
    }

    int rows = 0;
    int cols = 0;
    if (!tryInt64ToInt(outShape[1], &rows) || !tryInt64ToInt(outShape[2], &cols) || rows <= 0 || cols <= 0)
    {
        std::cerr << "[DirectMLDetector] Output tensor dimensions are invalid." << std::endl;
        return empty;
    }
    const int num_classes = rows - 4;

    for (size_t b = 0; b < batch_size; ++b)
    {
        const float* ptr = outData + b * rows * cols;
        std::vector<Detection> detections;

        std::vector<int64_t> shp = { static_cast<int64_t>(rows), static_cast<int64_t>(cols) };
        detections = postProcessYoloDML(ptr, shp, num_classes, conf_thr, nms_thr, max_detections, &nmsTimeTmp);

        if (useFixed && (target_w != config.detection_resolution || target_h != config.detection_resolution))
        {
            float scaleX = static_cast<float>(config.detection_resolution) / target_w;
            float scaleY = static_cast<float>(config.detection_resolution) / target_h;
            for (auto& d : detections)
            {
                d.box.x = static_cast<int>(d.box.x * scaleX);
                d.box.y = static_cast<int>(d.box.y * scaleY);
                d.box.width = static_cast<int>(d.box.width * scaleX);
                d.box.height = static_cast<int>(d.box.height * scaleY);
            }
        }

        batchDetections[b] = std::move(detections);
    }
    auto t5 = std::chrono::steady_clock::now();

    lastPreprocessTimeDML = t1 - t0;
    lastInferenceTimeDML = t3 - t2;
    lastCopyTimeDML = t4 - t3;
    lastPostprocessTimeDML = t5 - t4;
    lastNmsTimeDML = nmsTimeTmp;

    return batchDetections;
}

int DirectMLDetector::getNumberOfClasses()
{
    if (!isReady())
        return -1;

    if (sunpoint_raw_output)
        return 2;

    Ort::TypeInfo output_type_info = session.GetOutputTypeInfo(0);
    auto tensor_info = output_type_info.GetTensorTypeAndShapeInfo();
    std::vector<int64_t> output_shape = tensor_info.GetShape();

    if (output_shape.size() == 3)
    {
        int channels = 0;
        if (!tryInt64ToInt(output_shape[1], &channels))
        {
            std::cerr << "[DirectMLDetector] Output tensor channel dimension is invalid." << std::endl;
            return -1;
        }
        int num_classes = channels - 4;
        return num_classes;
    }
    else
    {
        std::cerr << "[DirectMLDetector] Unexpected output tensor shape." << std::endl;
        return -1;
    }
}

void DirectMLDetector::processFrame(
    const cv::Mat& detection_frame,
    const cv::Mat& source_frame,
    std::chrono::steady_clock::time_point frameTimestamp)
{
    const auto generation = detectionBuffer.currentGeneration();
    detectionBuffer.withCurrentGeneration(generation, [&] {
        std::unique_lock<std::mutex> lock(inferenceMutex);
        currentFrame = detection_frame;
        currentSourceFrame = source_frame.empty() ? detection_frame : source_frame;
        currentFrameTimestamp = (frameTimestamp.time_since_epoch().count() != 0)
            ? frameTimestamp
            : std::chrono::steady_clock::now();
        currentFrameGeneration = generation;
        frameReady = true;
        inferenceCV.notify_one();
    });
}

void DirectMLDetector::dmlInferenceThread()
{
    try
    {
        while (!shouldExit)
        {
            if (config.backend != "DML")
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            if (detector_model_changed.load())
            {
                const bool reloaded = initializeModel("models/" + config.ai_model);
                if (reloaded)
                {
                    detection_resolution_changed.store(true);
                    std::cout << "[DML] Detector reloaded: " << config.ai_model << std::endl;
                }
                detector_model_changed.store(false);
                if (!reloaded && !isReady())
                    detectionBuffer.clear();
            }

            if (!isReady())
            {
                detectionBuffer.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (detectionBuffer.isPaused())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            cv::Mat frame;
            cv::Mat sourceFrame;
            std::chrono::steady_clock::time_point frameTimestamp{};
            std::uint64_t frameGeneration = 0;
            bool hasNewFrame = false;
            {
                std::unique_lock<std::mutex> lock(inferenceMutex);
                if (!frameReady && !shouldExit)
                    inferenceCV.wait(lock, [this] { return frameReady || shouldExit; });

                if (shouldExit) break;

                if (frameReady)
                {
                    frame = std::move(currentFrame);
                    sourceFrame = std::move(currentSourceFrame);
                    frameTimestamp = currentFrameTimestamp;
                    frameGeneration = currentFrameGeneration;
                    frameReady = false;
                    hasNewFrame = true;
                }
            }

            if (hasNewFrame && !frame.empty())
            {
                if (!detectionBuffer.withCurrentGeneration(frameGeneration, [] {}))
                    continue;
                std::vector<cv::Mat> batchFrames = { frame };
                auto detectionsBatch = detectBatch(batchFrames);
                if (detectionsBatch.empty())
                {
                    continue;
                }
                const std::vector<Detection>& detections = detectionsBatch.back();
                std::vector<Detection> filteredDetections = detections;
                filterDetectionsByDepthMask(filteredDetections);
                filterDetectionsByCircleFov(filteredDetections);

                std::vector<cv::Rect> boxes;
                std::vector<int> classes;
                std::vector<float> confidences;
                boxes.reserve(filteredDetections.size());
                classes.reserve(filteredDetections.size());
                confidences.reserve(filteredDetections.size());
                for (const auto& d : filteredDetections)
                {
                    boxes.push_back(d.box);
                    classes.push_back(d.classId);
                    confidences.push_back(d.confidence);
                }

                if (!detectionBuffer.setForGeneration(frameGeneration, boxes, classes, confidences, frameTimestamp))
                    continue;

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
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "[DML] Inference thread crashed: " << e.what() << std::endl;
        shouldExit = true;
        inferenceCV.notify_all();
        detectionBuffer.cv.notify_all();
    }
    catch (...)
    {
        std::cerr << "[DML] Inference thread crashed: unknown exception." << std::endl;
        shouldExit = true;
        inferenceCV.notify_all();
        detectionBuffer.cv.notify_all();
    }
}
