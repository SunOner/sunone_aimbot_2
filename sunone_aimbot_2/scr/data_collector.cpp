#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "scr/data_collector.h"
#include "other_tools.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_map>

namespace cvm {
namespace {

namespace fs = std::filesystem;

constexpr int64_t kCollectSaveCooldownNs = 500'000'000;

struct CollectRuntimeState
{
    std::uint64_t frame_counter = 0;
    std::uint64_t sample_counter = 0;
    std::uint64_t saved_image_count = 0;
    std::uint64_t saved_label_count = 0;
    std::uint64_t frame_counter_baseline = 0;
    std::uint64_t sample_counter_baseline = 0;
    int64_t last_collect_save_ns = 0;
    std::string last_output_dir;
    std::string last_status;
};

struct CollectConfigSnapshot
{
    bool enabled = false;
    bool only_when_aimbot_running = false;
    bool only_when_targets_present = false;
    int save_every_n_frames = 1;
    int jpeg_quality = 95;
    std::string output_dir;
    bool auto_label_data = false;
    float auto_label_min_conf = 0.25f;
    int auto_label_max_boxes = 20;
    std::string auto_label_record_classes;
    int64_t cooldown_ns = kCollectSaveCooldownNs;
};

struct CollectAttempt
{
    CollectConfigSnapshot cfg;
    std::uint64_t sample_id = 0;
    int nodeKey = 0;
};

CollectRuntimeState g_collectRuntimeState;
std::unordered_map<int,CollectRuntimeState> g_collectNodeRuntimeStates;
std::mutex g_collectRuntimeMutex;

std::string GetExecutableDir()
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return ".";

    return fs::path(exePath).parent_path().string();
}

std::string BuildCollectSampleStem(std::uint64_t sample_id)
{
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm local_tm{};
    localtime_s(&local_tm, &t);

    char time_buf[80] = {};
    std::snprintf(
        time_buf,
        sizeof(time_buf),
        "%04d%02d%02d_%02d%02d%02d_%03lld_s%06llu",
        local_tm.tm_year + 1900,
        local_tm.tm_mon + 1,
        local_tm.tm_mday,
        local_tm.tm_hour,
        local_tm.tm_min,
        local_tm.tm_sec,
        static_cast<long long>(ms.count()),
        static_cast<unsigned long long>(sample_id));
    return std::string(time_buf);
}

std::set<int> ParseRecordClasses(const char* s)
{
    std::set<int> ids;
    if (!s || !s[0])
        return ids;

    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        item = OtherTools::TrimAscii(item);
        if (item.empty())
            continue;

        try
        {
            ids.insert(std::stoi(item));
        }
        catch (...) {}
    }

    return ids;
}

cv::Mat PrepareFrameForSave(const cv::Mat& frame)
{
    if (frame.empty())
        return {};

    cv::Mat bgr;
    switch (frame.channels())
    {
    case 4:
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);
        break;
    case 3:
        bgr = frame.clone();
        break;
    case 1:
        cv::cvtColor(frame, bgr, cv::COLOR_GRAY2BGR);
        break;
    default:
        break;
    }

    return bgr;
}

std::string ModelNameToFolder(const char* model_name)
{
    if (!model_name || model_name[0] == '\0')
        return "default";

    std::string s = OtherTools::TrimAscii(model_name);
    if (s.empty())
        return "default";

    const fs::path p(s);
    const std::string stem = p.filename().stem().string();
    return stem.empty() ? "default" : stem;
}

CollectConfigSnapshot SnapshotCollectConfig(const Config& cfg)
{
    CollectConfigSnapshot snapshot;
    snapshot.enabled = cfg.collect_data_while_playing;
    snapshot.only_when_aimbot_running = cfg.collect_only_when_aimbot_running;
    snapshot.only_when_targets_present = cfg.collect_only_when_targets_present;
    snapshot.save_every_n_frames = std::max(1, cfg.collect_save_every_n_frames);
    snapshot.jpeg_quality = std::clamp(cfg.collect_jpeg_quality, 1, 100);
    snapshot.output_dir = cfg.collect_output_dir;
    snapshot.auto_label_data = cfg.auto_label_data;
    snapshot.auto_label_min_conf = std::clamp(cfg.auto_label_min_conf, 0.0f, 1.0f);
    snapshot.auto_label_max_boxes = std::max(1, cfg.auto_label_max_boxes);
    snapshot.auto_label_record_classes = cfg.auto_label_record_classes;
    return snapshot;
}

void UpdateRuntimeStatus(const std::string& output_dir, const std::string& status, int nodeKey = 0)
{
    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    if (!output_dir.empty())
        g_collectRuntimeState.last_output_dir = output_dir;
    g_collectRuntimeState.last_status = status;
    if(nodeKey)
    {
        auto& state=g_collectNodeRuntimeStates[nodeKey];
        if(!output_dir.empty())state.last_output_dir=output_dir;
        state.last_status=status;
    }
}

std::string WriteYoloLabelFile(const fs::path& label_path,
                               const std::vector<cv::Rect>& boxes,
                               const std::vector<int>& classes,
                               const std::vector<float>& confidences,
                               int frame_width,
                               int frame_height,
                               float min_conf,
                               int max_boxes,
                               const std::set<int>* allowed_classes)
{
    std::ofstream out(label_path, std::ios::trunc);
    if (!out.is_open())
        return "label open failed";

    const float width = std::max(1.0f, static_cast<float>(frame_width));
    const float height = std::max(1.0f, static_cast<float>(frame_height));
    int written = 0;

    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const int cls = (i < classes.size()) ? classes[i] : 0;
        const float conf = (i < confidences.size()) ? confidences[i] : 1.0f;
        if (conf < min_conf)
            continue;

        if (allowed_classes && !allowed_classes->empty() && allowed_classes->count(cls) == 0)
            continue;

        if (written >= std::max(1, max_boxes))
            break;

        const cv::Rect& box = boxes[i];
        const float x1 = std::clamp(static_cast<float>(box.x), 0.0f, width);
        const float y1 = std::clamp(static_cast<float>(box.y), 0.0f, height);
        const float x2 = std::clamp(static_cast<float>(box.x + box.width), 0.0f, width);
        const float y2 = std::clamp(static_cast<float>(box.y + box.height), 0.0f, height);

        const float box_w = std::max(0.0f, x2 - x1) / width;
        const float box_h = std::max(0.0f, y2 - y1) / height;
        if (box_w <= 0.0f || box_h <= 0.0f)
            continue;

        const float cx = std::clamp(((x1 + x2) * 0.5f) / width, 0.0f, 1.0f);
        const float cy = std::clamp(((y1 + y2) * 0.5f) / height, 0.0f, 1.0f);

        out << cls << " " << cx << " " << cy << " " << box_w << " " << box_h << "\n";
        ++written;
    }

    return std::to_string(written) + " label(s)";
}

std::pair<fs::path, fs::path> ResolveModelOutputDirs(const std::string& root_dir,
                                                     const char* model_name,
                                                     const CollectConfigSnapshot& cfg)
{
    const fs::path output_root = ResolveCollectOutputDir(root_dir, cfg.output_dir.c_str());
    const fs::path model_root = output_root / ModelNameToFolder(model_name);
    return { model_root / "images", model_root / "labels" };
}

bool BuildSaveFrame(const cv::Mat& frame, cv::Mat& save_frame)
{
    save_frame = PrepareFrameForSave(frame);
    return !save_frame.empty() && save_frame.cols > 0 && save_frame.rows > 0;
}

bool TryBeginCollectAttempt(const CollectConfigSnapshot& cfg,
                            const std::vector<cv::Rect>& boxes,
                            bool aimbot_enabled,
                            std::uint64_t& sample_id, int nodeKey = 0)
{
    if (!cfg.enabled)
        return false;

    if (cfg.only_when_aimbot_running && !aimbot_enabled)
        return false;

    if (cfg.only_when_targets_present && boxes.empty())
        return false;

    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    auto& state=nodeKey?g_collectNodeRuntimeStates[nodeKey]:g_collectRuntimeState;
    ++state.frame_counter;
    if(nodeKey)++g_collectRuntimeState.frame_counter;
    if ((state.frame_counter % static_cast<std::uint64_t>(cfg.save_every_n_frames)) != 0)
        return false;

    if (state.last_collect_save_ns > 0 &&
        (now_ns - state.last_collect_save_ns) < cfg.cooldown_ns)
    {
        return false;
    }

    state.last_collect_save_ns = now_ns;
    sample_id = ++state.sample_counter;
    if(nodeKey)++g_collectRuntimeState.sample_counter;
    return true;
}

bool SaveCollectedFrame(const std::string& root_dir,
                        const char* model_name,
                        const cv::Mat& frame,
                        const std::vector<cv::Rect>& boxes,
                        const std::vector<int>& classes,
                        const std::vector<float>& confidences,
                        const CollectAttempt& attempt)
{
    cv::Mat save_frame;
    if (!BuildSaveFrame(frame, save_frame))
        return false;

    const auto [images_dir, labels_dir] = ResolveModelOutputDirs(root_dir, model_name, attempt.cfg);
    const fs::path model_root = images_dir.parent_path();

    std::error_code ec;
    fs::create_directories(images_dir, ec);
    if (ec)
    {
        UpdateRuntimeStatus(model_root.string(), "Collect save failed: create images folder.",attempt.nodeKey);
        return false;
    }

    ec.clear();
    fs::create_directories(labels_dir, ec);
    if (ec)
    {
        UpdateRuntimeStatus(model_root.string(), "Collect save failed: create labels folder.",attempt.nodeKey);
        return false;
    }

    const std::string stem = BuildCollectSampleStem(attempt.sample_id)+(attempt.nodeKey?"_n"+std::to_string(attempt.nodeKey):std::string());
    const fs::path image_path = images_dir / (stem + ".jpg");
    const fs::path label_path = labels_dir / (stem + ".txt");

    const std::vector<int> imwrite_params = {
        cv::IMWRITE_JPEG_QUALITY,
        attempt.cfg.jpeg_quality
    };

    bool image_ok = false;
    try
    {
        image_ok = cv::imwrite(image_path.string(), save_frame, imwrite_params);
    }
    catch (...)
    {
        image_ok = false;
    }

    if (!image_ok)
    {
        UpdateRuntimeStatus(model_root.string(), "Collect save failed: image write.",attempt.nodeKey);
        return false;
    }

    bool label_ok = true;
    std::string label_result = "auto-label disabled";
    if (attempt.cfg.auto_label_data)
    {
        const std::set<int> allowed = ParseRecordClasses(attempt.cfg.auto_label_record_classes.c_str());
        const std::set<int>* allowed_ptr = allowed.empty() ? nullptr : &allowed;
        label_result = WriteYoloLabelFile(
            label_path,
            boxes,
            classes,
            confidences,
            save_frame.cols,
            save_frame.rows,
            attempt.cfg.auto_label_min_conf,
            attempt.cfg.auto_label_max_boxes,
            allowed_ptr);
        label_ok = (label_result != "label open failed");
    }

    {
        std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
        g_collectRuntimeState.saved_image_count += 1;
        if (attempt.cfg.auto_label_data && label_ok)
            g_collectRuntimeState.saved_label_count += 1;
        g_collectRuntimeState.last_output_dir = model_root.string();
        g_collectRuntimeState.last_status = attempt.cfg.auto_label_data
            ? ("Saved image + " + label_result)
            : "Saved image only";
        if(attempt.nodeKey)
        {
            auto& state=g_collectNodeRuntimeStates[attempt.nodeKey];
            ++state.saved_image_count;if(attempt.cfg.auto_label_data&&label_ok)++state.saved_label_count;
            state.last_output_dir=model_root.string();state.last_status=g_collectRuntimeState.last_status;
        }
    }
    return label_ok;
}

}  // namespace

std::filesystem::path ResolveCollectOutputDir(const std::string& root_dir, const char* output_dir_raw)
{
    const std::string cleaned = OtherTools::TrimAscii(output_dir_raw ? std::string(output_dir_raw) : std::string());
    if (cleaned.empty())
    {
        const std::string base_dir = root_dir.empty() ? GetExecutableDir() : root_dir;
        return fs::path(base_dir) / "data_collector" / "Collected_data";
    }

    fs::path out(cleaned);
    if (out.is_absolute())
        return out;

    const std::string base_dir = root_dir.empty() ? GetExecutableDir() : root_dir;
    return fs::path(base_dir) / out;
}

bool IsDataCollectionEnabled(const Config& cfg)
{
    return cfg.collect_data_while_playing;
}

DataCollectionUiState GetDataCollectionUiState(const std::string& root_dir, const char* model_name, const Config& cfg)
{
    DataCollectionUiState ui;
    ui.enabled = IsDataCollectionEnabled(cfg);

    const CollectConfigSnapshot snapshot = SnapshotCollectConfig(cfg);
    const fs::path model_root = ResolveCollectOutputDir(root_dir, snapshot.output_dir.c_str()) / ModelNameToFolder(model_name);
    ui.resolved_output_dir = model_root.string();

    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    ui.observed_frame_count = g_collectRuntimeState.frame_counter;
    ui.attempted_sample_count = g_collectRuntimeState.sample_counter;
    ui.saved_image_count = g_collectRuntimeState.saved_image_count;
    ui.saved_label_count = g_collectRuntimeState.saved_label_count;
    ui.status = g_collectRuntimeState.last_status;
    return ui;
}

void ResetDataCollectionRuntime()
{
    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    g_collectRuntimeState = {};
    g_collectNodeRuntimeStates.clear();
    g_collectRuntimeState.last_status = "Counters reset.";
}
void ResetDataCollectionNodeCounters(int nodeKey)
{
    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    const auto found=g_collectNodeRuntimeStates.find(nodeKey);
    if(found==g_collectNodeRuntimeStates.end())return;
    auto& state=found->second;
    // Keep the sampling sequence/cooldown so a UI reset cannot overwrite a
    // sample or change the cadence of an in-flight recording operation.
    state.frame_counter_baseline=state.frame_counter;
    state.sample_counter_baseline=state.sample_counter;
    state.saved_image_count=state.saved_label_count=0;
    state.last_status="Counters reset.";
}

DataCollectionUiState GetDataCollectionNodeUiState(int nodeKey)
{
    DataCollectionUiState ui;
    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    const auto found=g_collectNodeRuntimeStates.find(nodeKey);if(found==g_collectNodeRuntimeStates.end())return ui;
    const auto& state=found->second;ui.enabled=true;ui.observed_frame_count=state.frame_counter-state.frame_counter_baseline;
    ui.attempted_sample_count=state.sample_counter-state.sample_counter_baseline;ui.saved_image_count=state.saved_image_count;ui.saved_label_count=state.saved_label_count;
    ui.resolved_output_dir=state.last_output_dir;ui.status=state.last_status;return ui;
}

void RetainDataCollectionNodes(const std::vector<int>& activeKeys)
{
    std::lock_guard<std::mutex> lock(g_collectRuntimeMutex);
    for(auto it=g_collectNodeRuntimeStates.begin();it!=g_collectNodeRuntimeStates.end();)
        if(std::find(activeKeys.begin(),activeKeys.end(),it->first)==activeKeys.end())it=g_collectNodeRuntimeStates.erase(it);else++it;
}

bool CollectDataSampleForNode(int nodeKey,const std::string& root_dir,const char* model_name,
    const cv::Mat& frame,const std::vector<cv::Rect>& boxes,const std::vector<int>& classes,
    const std::vector<float>& confidences,bool aimbot_enabled,const Config& settings,double cooldownMs)
{
    if(nodeKey<=0||frame.empty())return false;
    auto snapshot=SnapshotCollectConfig(settings);
    snapshot.cooldown_ns=static_cast<int64_t>(std::clamp(cooldownMs,0.0,60000.0)*1000000.0);
    std::uint64_t sampleId=0;
    if(!TryBeginCollectAttempt(snapshot,boxes,aimbot_enabled,sampleId,nodeKey))return false;
    return SaveCollectedFrame(root_dir,model_name,frame,boxes,classes,confidences,CollectAttempt{snapshot,sampleId,nodeKey});
}

void MaybeCollectDataSample(const std::string& root_dir,
                            const char* model_name,
                            const cv::Mat& frame,
                            const std::vector<cv::Rect>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences,
                            bool aimbot_enabled,
                            const Config& cfg)
{
    if (frame.empty() || frame.cols <= 0 || frame.rows <= 0)
        return;

    const CollectConfigSnapshot snapshot = SnapshotCollectConfig(cfg);
    std::uint64_t sample_id = 0;
    if (!TryBeginCollectAttempt(snapshot, boxes, aimbot_enabled, sample_id))
        return;

    SaveCollectedFrame(
        root_dir,
        model_name,
        frame,
        boxes,
        classes,
        confidences,
        CollectAttempt{ std::move(snapshot), sample_id });
}

#ifdef USE_CUDA
void MaybeCollectDataSample(const std::string& root_dir,
                            const char* model_name,
                            const cv::cuda::GpuMat& frame,
                            const std::vector<cv::Rect>& boxes,
                            const std::vector<int>& classes,
                            const std::vector<float>& confidences,
                            bool aimbot_enabled,
                            const Config& cfg)
{
    if (frame.empty())
        return;

    const CollectConfigSnapshot snapshot = SnapshotCollectConfig(cfg);
    std::uint64_t sample_id = 0;
    if (!TryBeginCollectAttempt(snapshot, boxes, aimbot_enabled, sample_id))
        return;

    cv::Mat downloaded;
    try
    {
        frame.download(downloaded);
    }
    catch (...)
    {
        UpdateRuntimeStatus("", "Collect save failed: GPU download.");
        return;
    }

    SaveCollectedFrame(
        root_dir,
        model_name,
        downloaded,
        boxes,
        classes,
        confidences,
        CollectAttempt{ std::move(snapshot), sample_id });
}
#endif

}  // namespace cvm
