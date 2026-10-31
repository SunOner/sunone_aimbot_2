#include <algorithm>
#include <numeric>
#include <chrono>
#include <limits>
#include <cmath>

#include "postProcess.h"

namespace
{
void RunBoundedNms(
    std::vector<Detection>& detections,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime);

bool TryPositiveInt64ToInt(int64_t value, int* out)
{
    if (!out || value <= 0 ||
        value > static_cast<int64_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }

    *out = static_cast<int>(value);
    return true;
}

bool ExtractRowsCols(const std::vector<int64_t>& shape, int* rows, int* cols)
{
    if (shape.size() < 2)
        return false;

    return TryPositiveInt64ToInt(shape[shape.size() - 2], rows) &&
        TryPositiveInt64ToInt(shape[shape.size() - 1], cols);
}

bool TryResolveClassLayout(
    int extent,
    int numClassesHint,
    bool preferObjectness,
    int* numClasses,
    bool* usesObjectness)
{
    if (!numClasses || !usesObjectness || extent <= 4)
        return false;

    auto tryHint = [&](int hint) -> bool
    {
        if (hint <= 0 || hint > 10000)
            return false;

        if (extent == 5 + hint)
        {
            *numClasses = hint;
            *usesObjectness = true;
            return true;
        }

        if (extent == 4 + hint)
        {
            *numClasses = hint;
            *usesObjectness = false;
            return true;
        }

        return false;
    };

    if (tryHint(numClassesHint))
        return true;

    if (preferObjectness && extent > 5)
    {
        *numClasses = extent - 5;
        *usesObjectness = true;
        return *numClasses > 0;
    }

    *numClasses = extent - 4;
    *usesObjectness = false;
    return *numClasses > 0;
}

bool LooksLikeXyxyDetections(const float* output, int rows, int cols)
{
    if (!output || rows <= 0 || cols != 6)
        return false;

    const int maxSamples = std::min(rows, 256);
    const int step = std::max(1, rows / maxSamples);
    int considered = 0;
    int validXyxy = 0;
    int integerClassId = 0;

    for (int i = 0; i < rows && considered < maxSamples; i += step)
    {
        const float* det = output + static_cast<size_t>(i) * static_cast<size_t>(cols);
        const float x1 = det[0];
        const float y1 = det[1];
        const float x2 = det[2];
        const float y2 = det[3];
        const float confidence = det[4];
        const float classId = det[5];

        if (!std::isfinite(x1) || !std::isfinite(y1) ||
            !std::isfinite(x2) || !std::isfinite(y2) ||
            !std::isfinite(confidence) || !std::isfinite(classId))
        {
            continue;
        }

        ++considered;

        if (x2 >= x1 && y2 >= y1)
            ++validXyxy;

        const float roundedClassId = std::round(classId);
        if (classId >= -1.0f &&
            classId <= 10000.0f &&
            std::fabs(classId - roundedClassId) <= 1e-3f)
        {
            ++integerClassId;
        }
    }

    if (considered == 0)
        return false;

    return validXyxy * 3 >= considered * 2 &&
        integerClassId * 4 >= considered * 3;
}

void AddXyxyDetection(
    std::vector<Detection>& detections,
    float x1,
    float y1,
    float x2,
    float y2,
    float confidence,
    int classId,
    float scale,
    float confThreshold)
{
    if (confidence <= confThreshold || x2 <= x1 || y2 <= y1)
        return;

    cv::Rect box;
    box.x = static_cast<int>(x1 * scale);
    box.y = static_cast<int>(y1 * scale);
    box.width = static_cast<int>((x2 - x1) * scale);
    box.height = static_cast<int>((y2 - y1) * scale);

    if (box.width <= 0 || box.height <= 0)
        return;

    detections.push_back(Detection{ box, confidence, classId });
}

void AddCxcywhDetection(
    std::vector<Detection>& detections,
    float cx,
    float cy,
    float width,
    float height,
    float confidence,
    int classId,
    float scale,
    float confThreshold)
{
    if (confidence <= confThreshold || width <= 0.0f || height <= 0.0f)
        return;

    const float halfWidth = 0.5f * width;
    const float halfHeight = 0.5f * height;

    cv::Rect box;
    box.x = static_cast<int>((cx - halfWidth) * scale);
    box.y = static_cast<int>((cy - halfHeight) * scale);
    box.width = static_cast<int>(width * scale);
    box.height = static_cast<int>(height * scale);

    if (box.width <= 0 || box.height <= 0)
        return;

    detections.push_back(Detection{ box, confidence, classId });
}

void DecodeXyxyDetections(
    const float* output,
    int rows,
    int cols,
    float confThreshold,
    float scale,
    std::vector<Detection>& detections)
{
    detections.reserve(detections.size() + static_cast<size_t>(rows));

    for (int i = 0; i < rows; ++i)
    {
        const float* det = output + static_cast<size_t>(i) * static_cast<size_t>(cols);
        AddXyxyDetection(
            detections,
            det[0],
            det[1],
            det[2],
            det[3],
            det[4],
            static_cast<int>(std::round(det[5])),
            scale,
            confThreshold);
    }
}

void DecodeFeatureMajorPredictions(
    const float* output,
    int rows,
    int cols,
    int numClasses,
    bool usesObjectness,
    float confThreshold,
    float scale,
    std::vector<Detection>& detections)
{
    const int classBase = usesObjectness ? 5 : 4;
    if (!output || rows < classBase + numClasses || cols <= 0 || numClasses <= 0)
        return;

    detections.reserve(detections.size() + 256);

    for (int i = 0; i < cols; ++i)
    {
        const float cx = output[0 * cols + i];
        const float cy = output[1 * cols + i];
        const float ow = output[2 * cols + i];
        const float oh = output[3 * cols + i];
        const float objectness = usesObjectness ? output[4 * cols + i] : 1.0f;

        float maxClassScore = 0.0f;
        int maxClassId = 0;
        for (int c = 0; c < numClasses; ++c)
        {
            const float score = output[(classBase + c) * cols + i];
            if (score > maxClassScore)
            {
                maxClassScore = score;
                maxClassId = c;
            }
        }

        AddCxcywhDetection(
            detections,
            cx,
            cy,
            ow,
            oh,
            objectness * maxClassScore,
            maxClassId,
            scale,
            confThreshold);
    }
}

void DecodePredictionMajorPredictions(
    const float* output,
    int rows,
    int cols,
    int numClasses,
    bool usesObjectness,
    float confThreshold,
    float scale,
    std::vector<Detection>& detections)
{
    const int classBase = usesObjectness ? 5 : 4;
    if (!output || cols < classBase + numClasses || rows <= 0 || numClasses <= 0)
        return;

    detections.reserve(detections.size() + 256);

    for (int i = 0; i < rows; ++i)
    {
        const float* det = output + static_cast<size_t>(i) * static_cast<size_t>(cols);
        const float objectness = usesObjectness ? det[4] : 1.0f;

        float maxClassScore = 0.0f;
        int maxClassId = 0;
        for (int c = 0; c < numClasses; ++c)
        {
            const float score = det[classBase + c];
            if (score > maxClassScore)
            {
                maxClassScore = score;
                maxClassId = c;
            }
        }

        AddCxcywhDetection(
            detections,
            det[0],
            det[1],
            det[2],
            det[3],
            objectness * maxClassScore,
            maxClassId,
            scale,
            confThreshold);
    }
}

std::vector<Detection> DecodeYoloOutput(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClassesHint,
    float confThreshold,
    float nmsThreshold,
    int maxDetections,
    float scale,
    std::chrono::duration<double, std::milli>* nmsTime)
{
    std::vector<Detection> detections;
    if (!output)
        return detections;

    int rows = 0;
    int cols = 0;
    if (!ExtractRowsCols(shape, &rows, &cols))
        return detections;

    if (cols == 6 && LooksLikeXyxyDetections(output, rows, cols))
    {
        DecodeXyxyDetections(output, rows, cols, confThreshold, scale, detections);
    }
    else if (rows <= cols)
    {
        int classes = 0;
        bool usesObjectness = false;
        if (TryResolveClassLayout(rows, numClassesHint, false, &classes, &usesObjectness))
        {
            DecodeFeatureMajorPredictions(
                output,
                rows,
                cols,
                classes,
                usesObjectness,
                confThreshold,
                scale,
                detections);
        }
    }
    else
    {
        int classes = 0;
        bool usesObjectness = false;
        if (TryResolveClassLayout(cols, numClassesHint, true, &classes, &usesObjectness))
        {
            DecodePredictionMajorPredictions(
                output,
                rows,
                cols,
                classes,
                usesObjectness,
                confThreshold,
                scale,
                detections);
        }
    }

    RunBoundedNms(detections, nmsThreshold, maxDetections, nmsTime);
    return detections;
}

void SortDetectionsByConfidence(std::vector<Detection>& detections)
{
    std::sort(
        detections.begin(),
        detections.end(),
        [](const Detection& a, const Detection& b)
        {
            return a.confidence > b.confidence;
        });
}

void LimitDetectionsByConfidence(std::vector<Detection>& detections, size_t limit)
{
    if (limit == 0 || detections.size() <= limit)
        return;

    const auto kth = detections.begin() + static_cast<std::vector<Detection>::difference_type>(limit);
    std::nth_element(
        detections.begin(),
        kth,
        detections.end(),
        [](const Detection& a, const Detection& b)
        {
            return a.confidence > b.confidence;
        });
    detections.resize(limit);
}

void RunBoundedNms(
    std::vector<Detection>& detections,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime)
{
    constexpr size_t kPreNmsHardLimit = 1000;

    size_t preNmsLimit = kPreNmsHardLimit;
    if (maxDetections > 0)
    {
        const size_t requested = static_cast<size_t>(maxDetections);
        preNmsLimit = std::min(kPreNmsHardLimit, std::max(requested, requested * 8));
    }

    LimitDetectionsByConfidence(detections, preNmsLimit);
    NMS(detections, nmsThreshold, nmsTime);

    if (maxDetections > 0)
        LimitDetectionsByConfidence(detections, static_cast<size_t>(maxDetections));

    SortDetectionsByConfidence(detections);
}
}

void NMS(std::vector<Detection>& detections, float nmsThreshold, std::chrono::duration<double, std::milli>* nmsTime)
{
    if (detections.empty()) return;

    if (nmsThreshold <= 0.0f)
    {
        if (nmsTime)
        {
            *nmsTime = std::chrono::duration<double, std::milli>(0);
        }
        return;
    }

    auto t0 = std::chrono::steady_clock::now();

    SortDetectionsByConfidence(detections);

    // Raw Inference nodes expose candidates to a separate graph NMS stage.
    // A threshold of one cannot suppress any box; avoid a quadratic scan of
    // the full model output in this deliberately disabled mode.
    if(nmsThreshold>=1.0f)
    {
        if(nmsTime)*nmsTime=std::chrono::steady_clock::now()-t0;
        return;
    }

    std::vector<bool> suppress(detections.size(), false);
    std::vector<Detection> result;
    result.reserve(detections.size());

    for (size_t i = 0; i < detections.size(); ++i)
    {
        if (suppress[i]) continue;

        result.push_back(detections[i]);

        const cv::Rect& box_i = detections[i].box;
        const float area_i = static_cast<float>(box_i.area());

        for (size_t j = i + 1; j < detections.size(); ++j)
        {
            if (suppress[j]) continue;

            const cv::Rect& box_j = detections[j].box;
            const cv::Rect intersection = box_i & box_j;

            if (intersection.width > 0 && intersection.height > 0)
            {
                const float intersection_area = static_cast<float>(intersection.area());
                const float union_area = area_i + static_cast<float>(box_j.area()) - intersection_area;

                if (intersection_area / union_area > nmsThreshold)
                {
                    suppress[j] = true;
                }
            }
        }
    }

    detections = std::move(result);

    auto t1 = std::chrono::steady_clock::now();
    if (nmsTime)
    {
        *nmsTime = t1 - t0;
    }
}

#ifdef USE_CUDA
std::vector<Detection> postProcessYolo(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime,
    float imageScale
)
{
    return DecodeYoloOutput(
        output,
        shape,
        numClasses,
        confThreshold,
        nmsThreshold,
        maxDetections,
        imageScale,
        nmsTime);
}
#endif

#ifndef USE_CUDA
std::vector<Detection> postProcessYoloDML(
    const float* output,
    const std::vector<int64_t>& shape,
    int numClasses,
    float confThreshold,
    float nmsThreshold,
    int maxDetections,
    std::chrono::duration<double, std::milli>* nmsTime
)
{
    return DecodeYoloOutput(
        output,
        shape,
        numClasses,
        confThreshold,
        nmsThreshold,
        maxDetections,
        1.0f,
        nmsTime);
}
#endif
