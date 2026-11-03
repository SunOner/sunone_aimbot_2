#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#ifdef USE_CUDA

#include <opencv2/opencv.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"
#include "other_tools.h"
#include "provider_benchmark.h"
#include "trt_detector.h"

extern Config config;
extern TrtDetector trt_detector;

namespace benchmarks
{
namespace
{
using Clock = std::chrono::steady_clock;

struct CliOptions
{
    bool help = false;
    bool listDevices = false;
    bool saveResults = true;
    std::string providersRequested = "cuda";
    std::string modelPath;
    std::string cudaModelPath;
    std::string imagePath;
    std::string resultsPath = "benchmark_results/provider_benchmark_cuda.csv";
    int runs = 100;
    int warmupRuns = 10;
    int resolution = 0;
};

struct BenchmarkResult
{
    std::string provider = "cuda";
    std::string providerModel;
    std::string status = "ok";
    std::string error;
    int inputW = 0;
    int inputH = 0;
    int runs = 0;
    int warmupRuns = 0;
    size_t lastDetections = 0;
    double loadSeconds = 0.0;
    double warmupSeconds = 0.0;
    double totalSeconds = 0.0;
    double preprocessSeconds = 0.0;
    double inferenceSeconds = 0.0;
    double postprocessSeconds = 0.0;
};

bool StartsWith(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

bool ParseInt(const std::string& text, int* out)
{
    if (!out)
        return false;
    try
    {
        size_t used = 0;
        int value = std::stoi(text, &used, 10);
        if (used != text.size())
            return false;
        *out = value;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ConsumeValue(int argc, char** argv, int* index, const std::string& arg, const std::string& option, std::string* value)
{
    const std::string prefix = option + "=";
    if (StartsWith(arg, prefix))
    {
        *value = arg.substr(prefix.size());
        return true;
    }

    if (arg == option)
    {
        if (*index + 1 >= argc)
            return false;
        const std::string next = argv[*index + 1] ? argv[*index + 1] : "";
        if (StartsWith(next, "--"))
            return false;
        *value = argv[++(*index)];
        return true;
    }

    return false;
}

CliOptions ParseCli(int argc, char** argv)
{
    CliOptions options;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i] ? argv[i] : "";
        std::string value;

        if (arg == "--benchmark-help" || arg == "--bench-help")
        {
            options.help = true;
            continue;
        }
        if (arg == "--bench-list-devices")
        {
            options.listDevices = true;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--benchmark-providers", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--bench-providers", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--providers", &value))
        {
            options.providersRequested = value.empty() ? "cuda" : value;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-model", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--model", &value))
        {
            options.modelPath = value;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-cuda-model", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--cuda-model", &value))
        {
            options.cudaModelPath = value;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-image", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--image", &value))
        {
            options.imagePath = value;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-results", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--results", &value))
        {
            options.resultsPath = value;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-runs", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--runs", &value))
        {
            ParseInt(value, &options.runs);
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-warmup", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--warmup", &value))
        {
            ParseInt(value, &options.warmupRuns);
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-resolution", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--resolution", &value))
        {
            ParseInt(value, &options.resolution);
            continue;
        }
        if (arg == "--bench-no-save")
        {
            options.saveResults = false;
            continue;
        }
    }

    options.runs = std::max(1, options.runs);
    options.warmupRuns = std::max(0, options.warmupRuns);
    return options;
}

void PrintHelp()
{
    std::cout
        << "Usage:\n"
        << "  ai.exe --benchmark-providers [cuda] [options]\n\n"
        << "Options:\n"
        << "  --bench-cuda-model <path>        TensorRT .engine or source .onnx model.\n"
        << "  --bench-model <path>             Alias used when --bench-cuda-model is omitted.\n"
        << "  --bench-image <path>             Optional image used as benchmark input.\n"
        << "  --bench-results <path>           CSV append path. Default: benchmark_results/provider_benchmark_cuda.csv.\n"
        << "  --bench-runs <n>                 Measured runs. Default: 100.\n"
        << "  --bench-warmup <n>               Warmup runs before timing. Default: 10.\n"
        << "  --bench-resolution <n>           Input size. Default: config detection_resolution.\n"
        << "  --bench-no-save                  Do not append the summary row to CSV.\n"
        << "  --bench-list-devices             Print CUDA devices and exit.\n"
        << "  --benchmark-help                 Show this help.\n";
}

std::filesystem::path FindRepoRoot()
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::current_path(ec);
    if (ec)
        return {};

    while (!path.empty())
    {
        if (std::filesystem::exists(path / ".git"))
            return path;
        if (!path.has_parent_path() || path.parent_path() == path)
            break;
        path = path.parent_path();
    }
    return {};
}

std::string QuoteForCommand(const std::filesystem::path& path)
{
    std::string value = path.string();
    std::string quoted = "\"";
    for (char ch : value)
        quoted += (ch == '"') ? "\\\"" : std::string(1, ch);
    quoted += '"';
    return quoted;
}

std::string CaptureCommandOutput(const std::string& command)
{
    std::array<char, 256> buffer{};
    std::string output;
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe)
        return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        output += buffer.data();
    _pclose(pipe);

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ' || output.back() == '\t'))
        output.pop_back();
    return output;
}

std::string GetGitCommitId(const std::filesystem::path& repoRoot)
{
    if (repoRoot.empty())
        return "unknown";
    std::string commit = CaptureCommandOutput("git -C " + QuoteForCommand(repoRoot) + " rev-parse --short HEAD 2>NUL");
    return commit.empty() ? "unknown" : commit;
}

bool GetGitDirty(const std::filesystem::path& repoRoot)
{
    if (repoRoot.empty())
        return false;
    return !CaptureCommandOutput("git -C " + QuoteForCommand(repoRoot) + " status --porcelain 2>NUL").empty();
}

std::string CurrentTimestampLocal()
{
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &now);
    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string CsvEscape(const std::string& value)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    for (unsigned char ch : value)
    {
        if (ch == '\n' || ch == '\r' || ch == '\t')
            sanitized += ' ';
        else if (ch >= 32 && ch < 127)
            sanitized += static_cast<char>(ch);
    }

    if (sanitized.find_first_of(",\"") == std::string::npos)
        return sanitized;

    std::string escaped = "\"";
    for (char ch : sanitized)
        escaped += (ch == '"') ? "\"\"" : std::string(1, ch);
    escaped += '"';
    return escaped;
}

std::filesystem::path ResolveBenchmarkModelPath(const CliOptions& options)
{
    if (!options.cudaModelPath.empty())
        return options.cudaModelPath;
    if (!options.modelPath.empty())
        return options.modelPath;
    if (!config.ai_model.empty())
        return std::filesystem::path("models") / config.ai_model;

    std::vector<std::string> models = getAvailableModels();
    if (!models.empty())
        return std::filesystem::path("models") / models.front();
    return {};
}

cv::Mat LoadBenchmarkFrame(const CliOptions& options)
{
    if (!options.imagePath.empty())
    {
        cv::Mat image = cv::imread(options.imagePath, cv::IMREAD_COLOR);
        if (!image.empty())
        {
            if (image.cols != options.resolution || image.rows != options.resolution)
                cv::resize(image, image, cv::Size(options.resolution, options.resolution), 0, 0, cv::INTER_LINEAR);
            return image;
        }
    }

    cv::Mat frame(options.resolution, options.resolution, CV_8UC3);
    for (int y = 0; y < frame.rows; ++y)
    {
        cv::Vec3b* row = frame.ptr<cv::Vec3b>(y);
        for (int x = 0; x < frame.cols; ++x)
        {
            row[x] = cv::Vec3b(
                static_cast<unsigned char>((x * 3 + y) % 256),
                static_cast<unsigned char>((x + y * 5) % 256),
                static_cast<unsigned char>((x * 7 + y * 11) % 256));
        }
    }
    return frame;
}

void PrintCudaDevices()
{
    int count = 0;
    cudaError_t status = cudaGetDeviceCount(&count);
    if (status != cudaSuccess)
    {
        std::cout << "CUDA devices: unavailable (" << cudaGetErrorString(status) << ")\n";
        return;
    }

    std::cout << "CUDA devices:\n";
    for (int i = 0; i < count; ++i)
    {
        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, i) == cudaSuccess)
        {
            std::cout << "  id=" << i
                      << " name=" << prop.name
                      << " capability=" << prop.major << "." << prop.minor
                      << " memory_mb=" << (prop.totalGlobalMem / (1024 * 1024))
                      << "\n";
        }
    }
}

BenchmarkResult RunCudaBenchmark(const CliOptions& options, const std::filesystem::path& modelPath, const cv::Mat& frame)
{
    BenchmarkResult result;
    result.providerModel = modelPath.string();
    result.inputW = frame.cols;
    result.inputH = frame.rows;
    result.runs = options.runs;
    result.warmupRuns = options.warmupRuns;

    auto loadStart = Clock::now();
    try
    {
        auto settings=NodeGraph::defaultNode(NodeGraph::NodeKind::Inference,1);
        settings.parameters["fixed_input_size"]=config.fixed_input_size?1:0;
        settings.parameters["confidence_threshold"]=config.confidence_threshold;settings.parameters["nms_threshold"]=config.nms_threshold;
        settings.parameters["max_detections"]=config.max_detections;settings.parameters["use_cuda_graph"]=config.use_cuda_graph?1:0;
        settings.parameters["use_pinned_memory"]=config.use_pinned_memory?1:0;
        settings.parameters["export_enable_fp16"]=config.export_enable_fp16?1:0;settings.parameters["export_enable_fp8"]=config.export_enable_fp8?1:0;
        trt_detector.initialize(modelPath.string(),settings,frame.cols,frame.rows);
        result.loadSeconds = std::chrono::duration<double>(Clock::now() - loadStart).count();
        if (!trt_detector.isInitialized())
            throw std::runtime_error("TensorRT detector did not initialize.");

        auto warmupStart = Clock::now();
        for (int i = 0; i < options.warmupRuns; ++i)
        {
            auto detections = trt_detector.detect(frame,settings);
            result.lastDetections = detections.size();
        }
        result.warmupSeconds = std::chrono::duration<double>(Clock::now() - warmupStart).count();

        auto totalStart = Clock::now();
        for (int i = 0; i < options.runs; ++i)
        {
            auto detections = trt_detector.detect(frame,settings);
            result.lastDetections = detections.size();
            result.preprocessSeconds += trt_detector.lastPreprocessTime.count() / 1000.0;
            result.inferenceSeconds +=
                (trt_detector.lastInferenceTime.count() + trt_detector.lastCopyTime.count()) / 1000.0;
            result.postprocessSeconds += trt_detector.lastPostprocessTime.count() / 1000.0;
        }
        result.totalSeconds = std::chrono::duration<double>(Clock::now() - totalStart).count();
    }
    catch (const std::exception& e)
    {
        result.status = "failed";
        result.error = e.what();
        result.loadSeconds = std::chrono::duration<double>(Clock::now() - loadStart).count();
    }

    trt_detector.requestStop();
    return result;
}

void PrintSummary(const CliOptions& options, const BenchmarkResult& result)
{
    std::cout << "\nCUDA benchmark summary (seconds)\n";
    std::cout << "model=" << result.providerModel << "\n";
    std::cout << "providers_requested=" << options.providersRequested << "\n";
    std::cout << "resolution=" << options.resolution
              << " runs=" << options.runs
              << " warmup=" << options.warmupRuns
              << "\n\n";

    const double avgRun = (result.runs > 0 && result.totalSeconds > 0.0)
        ? result.totalSeconds / static_cast<double>(result.runs)
        : 0.0;
    const double fps = (result.totalSeconds > 0.0)
        ? static_cast<double>(result.runs) / result.totalSeconds
        : 0.0;

    std::cout << std::fixed << std::setprecision(6)
        << "provider,provider_model,status,runs,warmup,input_w,input_h,"
        << "load_s,warmup_s,total_s,preprocess_s,inference_s,postprocess_s,avg_run_s,fps,last_detections,error\n"
        << result.provider << ","
        << CsvEscape(result.providerModel) << ","
        << result.status << ","
        << result.runs << ","
        << result.warmupRuns << ","
        << result.inputW << ","
        << result.inputH << ","
        << result.loadSeconds << ","
        << result.warmupSeconds << ","
        << result.totalSeconds << ","
        << result.preprocessSeconds << ","
        << result.inferenceSeconds << ","
        << result.postprocessSeconds << ","
        << avgRun << ","
        << fps << ","
        << result.lastDetections << ","
        << CsvEscape(result.error)
        << "\n";
}

bool AppendBenchmarkCsv(const CliOptions& options, const BenchmarkResult& result, std::filesystem::path* writtenPath)
{
    std::filesystem::path csvPath = options.resultsPath.empty()
        ? std::filesystem::path("benchmark_results/provider_benchmark_cuda.csv")
        : std::filesystem::path(options.resultsPath);
    const std::filesystem::path repoRoot = FindRepoRoot();
    if (csvPath.is_relative() && !repoRoot.empty())
        csvPath = repoRoot / csvPath;

    std::error_code ec;
    const std::filesystem::path parent = csvPath.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    if (ec)
        return false;

    const std::string header =
        "timestamp_local,commit_id,git_dirty,build_backend,providers_requested,provider,provider_model,status,"
        "runs,warmup,input_w,input_h,load_s,warmup_s,total_s,preprocess_s,inference_s,postprocess_s,"
        "avg_run_s,fps,last_detections,error";

    bool writeHeader = true;
    if (std::filesystem::exists(csvPath, ec) && !ec && std::filesystem::file_size(csvPath, ec) > 0)
    {
        std::ifstream existing(csvPath);
        std::string firstLine;
        std::getline(existing, firstLine);
        if (!firstLine.empty() && firstLine.back() == '\r')
            firstLine.pop_back();
        writeHeader = firstLine != header;
    }

    std::ofstream file(csvPath, std::ios::app);
    if (!file)
        return false;

    if (writeHeader)
        file << header << "\n";

    const double avgRun = (result.runs > 0 && result.totalSeconds > 0.0)
        ? result.totalSeconds / static_cast<double>(result.runs)
        : 0.0;
    const double fps = (result.totalSeconds > 0.0)
        ? static_cast<double>(result.runs) / result.totalSeconds
        : 0.0;

    file << std::fixed << std::setprecision(6)
        << CsvEscape(CurrentTimestampLocal()) << ","
        << CsvEscape(GetGitCommitId(repoRoot)) << ","
        << (GetGitDirty(repoRoot) ? "true" : "false") << ","
        << "cuda,"
        << CsvEscape(options.providersRequested) << ","
        << result.provider << ","
        << CsvEscape(result.providerModel) << ","
        << result.status << ","
        << result.runs << ","
        << result.warmupRuns << ","
        << result.inputW << ","
        << result.inputH << ","
        << result.loadSeconds << ","
        << result.warmupSeconds << ","
        << result.totalSeconds << ","
        << result.preprocessSeconds << ","
        << result.inferenceSeconds << ","
        << result.postprocessSeconds << ","
        << avgRun << ","
        << fps << ","
        << result.lastDetections << ","
        << CsvEscape(result.error)
        << "\n";

    if (writtenPath)
        *writtenPath = csvPath;
    return true;
}
} // namespace

bool IsProviderBenchmarkRequested(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--benchmark-providers" ||
            StartsWith(arg, "--benchmark-providers=") ||
            arg == "--bench-providers" ||
            StartsWith(arg, "--bench-providers=") ||
            arg == "--benchmark-help" ||
            arg == "--bench-help" ||
            arg == "--bench-list-devices")
        {
            return true;
        }
    }
    return false;
}

int RunProviderBenchmarkCli(int argc, char** argv)
{
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_FATAL);

    CliOptions options = ParseCli(argc, argv);
    if (options.help)
    {
        PrintHelp();
        return 0;
    }

    if (options.listDevices)
    {
        PrintCudaDevices();
        return 0;
    }

    if (!config.loadConfig())
    {
        std::cerr << "[Benchmark] Failed to load config.ini." << std::endl;
        return 2;
    }
    config.backend = "TRT";

    if (options.resolution <= 0)
        options.resolution = config.detection_resolution > 0 ? config.detection_resolution : 320;
    options.resolution = std::clamp(options.resolution, 32, 4096);

    std::filesystem::path modelPath = ResolveBenchmarkModelPath(options);
    if (modelPath.empty() || !std::filesystem::exists(modelPath))
    {
        std::cerr << "[Benchmark] TensorRT model was not found. Pass --bench-cuda-model <path> or put a model in models." << std::endl;
        return 2;
    }

    cv::Mat frame = LoadBenchmarkFrame(options);
    if (frame.empty())
    {
        std::cerr << "[Benchmark] Failed to prepare benchmark input frame." << std::endl;
        return 2;
    }

    BenchmarkResult result = RunCudaBenchmark(options, modelPath, frame);
    PrintSummary(options, result);
    if (options.saveResults)
    {
        std::filesystem::path csvPath;
        if (AppendBenchmarkCsv(options, result, &csvPath))
        {
            std::cout << "results_csv=" << csvPath.string() << "\n";
            std::cout << "results_csv_base=repo_root\n";
        }
    }

    return result.status == "ok" ? 0 : 3;
}
} // namespace benchmarks

#else
#include <dxgi1_6.h>
#include <wrl/client.h>

#ifdef AIMBOT_HAS_DIRECTML_EX
#include <d3d12.h>
#include <DirectML.h>
#endif

#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <opencv2/core/utils/logger.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"
#include "postProcess.h"
#include "provider_benchmark.h"

extern Config config;

namespace benchmarks
{
namespace
{
using Clock = std::chrono::steady_clock;

enum class ProviderKind
{
    Cpu,
    DmlGpu,
    DmlCpu
};

struct CliOptions
{
    bool benchmarkRequested = false;
    bool runBenchmark = false;
    bool help = false;
    bool listDevices = false;
    bool postprocess = true;
    bool disableCpuFallback = false;
    bool saveResults = true;
    std::vector<ProviderKind> providers;
    std::string modelPath;
    std::string imagePath;
    std::string resultsPath = "benchmark_results/provider_benchmark.csv";
    int runs = 100;
    int warmupRuns = 10;
    int resolution = 0;
    int batch = 1;
    int dmlDeviceId = -1;
};

struct ModelInfo
{
    std::string inputName;
    std::vector<std::string> outputNames;
    std::vector<const char*> outputNamePtrs;
    int batch = 1;
    int channels = 3;
    int inputH = 0;
    int inputW = 0;
};

struct PreprocessWorkspace
{
    cv::Mat bgrBuffer;
    cv::Mat resizeBuffer;
    cv::Mat floatBuffer;
    cv::Mat grayResizeBuffer;
    cv::Mat grayFloatBuffer;
};

struct ProviderResult
{
    std::string provider;
    std::string providerModel;
    std::string status = "ok";
    std::string error;
    int requestedBatch = 1;
    int effectiveBatch = 1;
    int inputW = 0;
    int inputH = 0;
    int runs = 0;
    int warmupRuns = 0;
    size_t lastDetections = 0;
    double loadSeconds = 0.0;
    double warmupSeconds = 0.0;
    double totalSeconds = 0.0;
    double preprocessSeconds = 0.0;
    double inferenceSeconds = 0.0;
    double postprocessSeconds = 0.0;
};

class ScopedStreamSilencer
{
public:
    explicit ScopedStreamSilencer(std::ostream& stream)
        : stream_(stream),
          oldBuffer_(stream.rdbuf(sink_.rdbuf()))
    {
    }

    ~ScopedStreamSilencer()
    {
        stream_.rdbuf(oldBuffer_);
    }

    ScopedStreamSilencer(const ScopedStreamSilencer&) = delete;
    ScopedStreamSilencer& operator=(const ScopedStreamSilencer&) = delete;

private:
    std::ostream& stream_;
    std::streambuf* oldBuffer_;
    std::ostringstream sink_;
};

#ifdef AIMBOT_HAS_DIRECTML_EX
struct DmlCpuResources
{
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue;
    Microsoft::WRL::ComPtr<IDMLDevice> dmlDevice;
};
#else
struct DmlCpuResources {};
#endif

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool StartsWith(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string WideToUtf8Local(const wchar_t* value)
{
    if (!value)
        return {};

    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
        return {};

    std::string out(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), required, nullptr, nullptr);
    return out;
}

std::string HResultToString(HRESULT hr)
{
    std::ostringstream oss;
    oss << "HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return oss.str();
}

std::string ProviderName(ProviderKind kind)
{
    switch (kind)
    {
    case ProviderKind::Cpu: return "cpu";
    case ProviderKind::DmlGpu: return "dml-gpu";
    case ProviderKind::DmlCpu: return "dml-cpu";
    }
    return "unknown";
}

std::optional<ProviderKind> ParseProviderName(const std::string& value)
{
    const std::string name = ToLower(value);
    if (name == "cpu")
        return ProviderKind::Cpu;
    if (name == "dml" || name == "dml-gpu" || name == "dml_gpu" || name == "dml+gpu" || name == "directml")
        return ProviderKind::DmlGpu;
    if (name == "dml-cpu" || name == "dml_cpu" || name == "dml+cpu" || name == "warp" || name == "dml-warp")
        return ProviderKind::DmlCpu;
    return std::nullopt;
}

std::vector<std::string> SplitCommaList(const std::string& value)
{
    std::vector<std::string> out;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), item.end());
        if (!item.empty())
            out.push_back(item);
    }
    return out;
}

bool ParseInt(const std::string& text, int* out)
{
    if (!out)
        return false;

    try
    {
        size_t used = 0;
        const int value = std::stoi(text, &used, 10);
        if (used != text.size())
            return false;
        *out = value;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ConsumeValue(int argc, char** argv, int* index, const std::string& arg, const std::string& option, std::string* value)
{
    const std::string prefix = option + "=";
    if (StartsWith(arg, prefix))
    {
        *value = arg.substr(prefix.size());
        return true;
    }

    if (arg == option)
    {
        if (*index + 1 >= argc)
            return false;
        const std::string next = argv[*index + 1] ? argv[*index + 1] : "";
        if (StartsWith(next, "--"))
            return false;
        *value = argv[++(*index)];
        return true;
    }

    return false;
}

void AddProvidersFromList(const std::string& list, std::vector<ProviderKind>* providers)
{
    if (!providers)
        return;

    providers->clear();
    for (const std::string& item : SplitCommaList(list))
    {
        auto provider = ParseProviderName(item);
        if (provider)
            providers->push_back(*provider);
        else
            std::cerr << "[Benchmark] Unknown provider ignored: " << item << std::endl;
    }
}

CliOptions ParseCli(int argc, char** argv)
{
    CliOptions options;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i] ? argv[i] : "";
        std::string value;

        if (arg == "--benchmark-help" || arg == "--bench-help")
        {
            options.benchmarkRequested = true;
            options.help = true;
            continue;
        }
        if (arg == "--bench-list-devices")
        {
            options.benchmarkRequested = true;
            options.listDevices = true;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--benchmark-providers", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--bench-providers", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--providers", &value))
        {
            options.benchmarkRequested = true;
            options.runBenchmark = true;
            AddProvidersFromList(value, &options.providers);
            continue;
        }
        if (arg == "--benchmark-providers" || arg == "--bench-providers")
        {
            options.benchmarkRequested = true;
            options.runBenchmark = true;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-model", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--model", &value))
        {
            options.modelPath = value;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-image", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--image", &value))
        {
            options.imagePath = value;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-results", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--results", &value))
        {
            options.resultsPath = value;
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-runs", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--runs", &value))
        {
            ParseInt(value, &options.runs);
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-warmup", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--warmup", &value))
        {
            ParseInt(value, &options.warmupRuns);
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-resolution", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--resolution", &value))
        {
            ParseInt(value, &options.resolution);
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-batch", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--batch", &value))
        {
            ParseInt(value, &options.batch);
            continue;
        }
        if (ConsumeValue(argc, argv, &i, arg, "--bench-dml-device", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--dml-device", &value))
        {
            ParseInt(value, &options.dmlDeviceId);
            continue;
        }
        if (arg == "--bench-no-postprocess")
        {
            options.postprocess = false;
            continue;
        }
        if (arg == "--bench-disable-cpu-fallback")
        {
            options.disableCpuFallback = true;
            continue;
        }
        if (arg == "--bench-no-save")
        {
            options.saveResults = false;
            continue;
        }
    }

    if (options.providers.empty())
    {
        options.providers = {
            ProviderKind::Cpu,
            ProviderKind::DmlGpu,
            ProviderKind::DmlCpu
        };
    }

    return options;
}

bool HasOrtProvider(const std::vector<std::string>& availableProviders, const std::string& provider)
{
    return std::find(availableProviders.begin(), availableProviders.end(), provider) != availableProviders.end();
}

std::string JoinStrings(const std::vector<std::string>& values, const std::string& separator)
{
    std::ostringstream oss;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
            oss << separator;
        oss << values[i];
    }
    return oss.str();
}

std::string CsvEscape(const std::string& value)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    for (unsigned char ch : value)
    {
        if (ch == '\n' || ch == '\r')
            sanitized += ' ';
        else if (ch >= 32 && ch < 127)
            sanitized += static_cast<char>(ch);
        else if (ch == '\t')
            sanitized += ' ';
    }

    if (value.find("80070057") != std::string::npos &&
        sanitized.find("invalid parameter") == std::string::npos)
    {
        sanitized += " (invalid parameter)";
    }

    if (sanitized.find_first_of(",\"") == std::string::npos)
        return sanitized;

    std::string escaped = "\"";
    for (char ch : sanitized)
    {
        if (ch == '"')
            escaped += "\"\"";
        else
            escaped += ch;
    }
    escaped += '"';
    return escaped;
}

std::string TrimWhitespace(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::string QuoteForCommand(const std::filesystem::path& path)
{
    std::string value = path.string();
    std::string quoted = "\"";
    for (char ch : value)
    {
        if (ch == '"')
            quoted += "\\\"";
        else
            quoted += ch;
    }
    quoted += '"';
    return quoted;
}

std::string CaptureCommandOutput(const std::string& command)
{
    std::array<char, 256> buffer{};
    std::string output;

#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe)
        return {};

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
    {
        output += buffer.data();
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return TrimWhitespace(output);
}

std::filesystem::path FindRepoRoot()
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::current_path(ec);
    if (ec)
        return {};

    while (!path.empty())
    {
        if (std::filesystem::exists(path / ".git"))
            return path;
        if (!path.has_parent_path() || path.parent_path() == path)
            break;
        path = path.parent_path();
    }

    return {};
}

std::string GetGitCommitId(const std::filesystem::path& repoRoot)
{
    if (repoRoot.empty())
        return "unknown";

    std::string commit = CaptureCommandOutput("git -C " + QuoteForCommand(repoRoot) + " rev-parse --short HEAD 2>NUL");
    return commit.empty() ? "unknown" : commit;
}

bool GetGitDirty(const std::filesystem::path& repoRoot)
{
    if (repoRoot.empty())
        return false;

    std::string status = CaptureCommandOutput("git -C " + QuoteForCommand(repoRoot) + " status --porcelain 2>NUL");
    return !status.empty();
}

std::string CurrentTimestampLocal()
{
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif
    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void PrintHelp()
{
    std::cout
        << "Usage:\n"
        << "  ai.exe --benchmark-providers [cpu,dml-gpu,dml-cpu] [options]\n\n"
        << "Options:\n"
        << "  --bench-model <path>             ONNX model path. Defaults to config ai_model if it is .onnx, otherwise first models/*.onnx.\n"
        << "  --bench-image <path>             Image to preprocess for every run. Defaults to deterministic synthetic input.\n"
        << "  --bench-results <path>           CSV append path. Relative paths resolve from the repository root.\n"
        << "                                   Default: benchmark_results/provider_benchmark.csv.\n"
        << "  --bench-runs <n>                 Measured runs. Default: 100.\n"
        << "  --bench-warmup <n>               Warmup runs before timing. Default: 10.\n"
        << "  --bench-resolution <n>           Dynamic input size. Default: config detection_resolution.\n"
        << "  --bench-batch <n>                Requested batch size. Static-batch models keep their own batch.\n"
        << "  --bench-dml-device <id>          DXGI adapter id for dml-gpu. Default: config dml_device_id.\n"
        << "  --bench-no-postprocess           Measure preprocess + session.Run only.\n"
        << "  --bench-disable-cpu-fallback     Disable ORT fallback to CPU EP for non-CPU providers.\n"
        << "  --bench-no-save                  Do not append the summary rows to CSV.\n"
        << "  --bench-list-devices             Print DXGI adapter ids and exit.\n"
        << "  --benchmark-help                 Show this help.\n\n"
        << "Examples:\n"
        << "  ai.exe --benchmark-providers\n"
        << "  ai.exe --benchmark-providers cpu,dml-gpu --bench-runs 200 --bench-warmup 20\n";
}

std::string AdapterName(int deviceId)
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return "unknown";

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(static_cast<UINT>(deviceId), &adapter)))
        return "invalid";

    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)))
        return "unknown";

    return WideToUtf8Local(desc.Description);
}

void PrintDxgiAdapters()
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        std::cout << "DXGI adapters: unavailable (" << HResultToString(hr) << ")\n";
        return;
    }

    std::cout << "DXGI adapters for DML GPU:\n";
    for (UINT i = 0;; ++i)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr))
            break;

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)))
            continue;

        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        std::cout << "  id=" << i
                  << " name=" << WideToUtf8Local(desc.Description)
                  << " dedicated_vram_mb=" << (desc.DedicatedVideoMemory / (1024 * 1024))
                  << (software ? " software" : "")
                  << "\n";
    }
    std::cout << "DML CPU uses the WARP software adapter through DirectML when available.\n";
}

std::vector<std::filesystem::path> CollectModelCandidates(const CliOptions& options)
{
    if (!options.modelPath.empty())
        return { std::filesystem::path(options.modelPath) };

    std::vector<std::filesystem::path> candidates;
    auto addCandidate = [&candidates](const std::filesystem::path& path)
    {
        if (path.empty() || ToLower(path.extension().string()) != ".onnx" || !std::filesystem::exists(path))
            return;
        const std::filesystem::path normalized = path.lexically_normal();
        if (std::find(candidates.begin(), candidates.end(), normalized) == candidates.end())
            candidates.push_back(normalized);
    };

    if (!config.ai_model.empty())
    {
        std::filesystem::path configured = std::filesystem::path("models") / config.ai_model;
        if (ToLower(configured.extension().string()) == ".onnx")
        {
            addCandidate(configured);
        }
        else
        {
            addCandidate(configured.replace_extension(".onnx"));
        }
    }

    std::filesystem::path modelsDir("models");
    if (std::filesystem::exists(modelsDir))
    {
        std::vector<std::filesystem::path> candidates;
        for (const auto& entry : std::filesystem::directory_iterator(modelsDir))
        {
            if (entry.is_regular_file() && ToLower(entry.path().extension().string()) == ".onnx")
                candidates.push_back(entry.path());
        }

        if (!candidates.empty())
        {
            std::sort(candidates.begin(), candidates.end());
            for (const auto& candidate : candidates)
                addCandidate(candidate);
        }
    }

    return candidates;
}

cv::Mat LoadBenchmarkFrame(const CliOptions& options)
{
    if (!options.imagePath.empty())
    {
        cv::Mat image = cv::imread(options.imagePath, cv::IMREAD_COLOR);
        if (!image.empty())
            return image;
        std::cerr << "[Benchmark] Failed to read image, using synthetic input: " << options.imagePath << std::endl;
    }

    cv::Mat synthetic(options.resolution, options.resolution, CV_8UC3);
    cv::RNG rng(0x5A17);
    rng.fill(synthetic, cv::RNG::UNIFORM, 0, 256);
    return synthetic;
}

void PreprocessFrameToTensor(
    const cv::Mat& frame,
    float* dst,
    int targetW,
    int targetH,
    PreprocessWorkspace* workspace)
{
    if (!dst || targetW <= 0 || targetH <= 0 || !workspace)
        return;

    const size_t channelSize = static_cast<size_t>(targetW) * static_cast<size_t>(targetH);
    cv::Mat rgbPlanes[3] = {
        cv::Mat(targetH, targetW, CV_32F, dst),
        cv::Mat(targetH, targetW, CV_32F, dst + channelSize),
        cv::Mat(targetH, targetW, CV_32F, dst + channelSize * 2)
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
        if (frame.cols != targetW || frame.rows != targetH)
        {
            cv::resize(frame, workspace->grayResizeBuffer, cv::Size(targetW, targetH), 0, 0, cv::INTER_LINEAR);
            grayResized = workspace->grayResizeBuffer;
        }
        else
        {
            grayResized = frame;
        }

        grayResized.convertTo(workspace->grayFloatBuffer, CV_32F, 1.0f / 255.0f);
        workspace->grayFloatBuffer.copyTo(rgbPlanes[0]);
        workspace->grayFloatBuffer.copyTo(rgbPlanes[1]);
        workspace->grayFloatBuffer.copyTo(rgbPlanes[2]);
        return;
    }

    cv::Mat bgrFrame;
    switch (frame.channels())
    {
    case 4:
        cv::cvtColor(frame, workspace->bgrBuffer, cv::COLOR_BGRA2BGR);
        bgrFrame = workspace->bgrBuffer;
        break;
    case 3:
        bgrFrame = frame;
        break;
    default:
        clearTensor();
        return;
    }

    cv::Mat resizedBgr;
    if (bgrFrame.cols != targetW || bgrFrame.rows != targetH)
    {
        cv::resize(bgrFrame, workspace->resizeBuffer, cv::Size(targetW, targetH), 0, 0, cv::INTER_LINEAR);
        resizedBgr = workspace->resizeBuffer;
    }
    else
    {
        resizedBgr = bgrFrame;
    }

    resizedBgr.convertTo(workspace->floatBuffer, CV_32FC3, 1.0f / 255.0f);

    cv::Mat bgrToRgbPlanes[3] = {
        rgbPlanes[2],
        rgbPlanes[1],
        rgbPlanes[0]
    };
    cv::split(workspace->floatBuffer, bgrToRgbPlanes);
}

void PreprocessBatch(
    const cv::Mat& frame,
    int batch,
    int targetW,
    int targetH,
    std::vector<float>* tensor,
    PreprocessWorkspace* workspace)
{
    const size_t frameTensorSize = static_cast<size_t>(3) * static_cast<size_t>(targetH) * static_cast<size_t>(targetW);
    tensor->resize(static_cast<size_t>(batch) * frameTensorSize);
    for (int b = 0; b < batch; ++b)
    {
        float* dst = tensor->data() + static_cast<size_t>(b) * frameTensorSize;
        PreprocessFrameToTensor(frame, dst, targetW, targetH, workspace);
    }
}

bool TryInt64ToInt(int64_t value, int* out)
{
    if (!out)
        return false;
    if (value < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<int64_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

float SigmoidFloat(float x)
{
    if (x >= 0.0f)
    {
        const float z = std::exp(-x);
        return 1.0f / (1.0f + z);
    }
    const float z = std::exp(x);
    return z / (1.0f + z);
}

float SoftplusFloat(float x)
{
    if (x > 20.0f) return x;
    if (x < -20.0f) return std::exp(x);
    return std::log1p(std::exp(x));
}

bool IsShape4(const std::vector<int64_t>& shape)
{
    return shape.size() == 4 && shape[0] > 0 && shape[1] > 0 && shape[2] > 0 && shape[3] > 0;
}

size_t NchwOffset(int batch, int channel, int y, int x, int channels, int height, int width)
{
    return (((static_cast<size_t>(batch) * static_cast<size_t>(channels) + static_cast<size_t>(channel))
        * static_cast<size_t>(height) + static_cast<size_t>(y)) * static_cast<size_t>(width))
        + static_cast<size_t>(x);
}

int FindOutputIndex(const std::vector<std::string>& names, const char* wanted)
{
    for (int i = 0; i < static_cast<int>(names.size()); ++i)
    {
        if (names[i] == wanted)
            return i;
    }
    return -1;
}

std::vector<Detection> DecodeSunPointRaw(
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
    float nmsThreshold)
{
    std::vector<Detection> detections;
    if (!heat || !box || !offset || !IsShape4(heatShape) || !IsShape4(boxShape) || !IsShape4(offsetShape))
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
    detections.reserve(static_cast<size_t>(std::max(config.max_detections, 16)));

    for (int c = 0; c < classes; ++c)
    {
        for (int y = 0; y < gridH; ++y)
        {
            for (int x = 0; x < gridW; ++x)
            {
                const size_t heatIdx = NchwOffset(batchIndex, c, y, x, classes, gridH, gridW);
                const float heatLogit = heat[heatIdx];
                const float score = SigmoidFloat(heatLogit);
                if (score <= confThreshold)
                    continue;

                bool isPeak = true;
                for (int yy = std::max(0, y - 1); yy <= std::min(gridH - 1, y + 1) && isPeak; ++yy)
                {
                    for (int xx = std::max(0, x - 1); xx <= std::min(gridW - 1, x + 1); ++xx)
                    {
                        if (yy == y && xx == x)
                            continue;
                        const size_t neighborIdx = NchwOffset(batchIndex, c, yy, xx, classes, gridH, gridW);
                        if (heat[neighborIdx] > heatLogit)
                        {
                            isPeak = false;
                            break;
                        }
                    }
                }
                if (!isPeak)
                    continue;

                const float offX = SigmoidFloat(offset[NchwOffset(batchIndex, 0, y, x, 2, gridH, gridW)]);
                const float offY = SigmoidFloat(offset[NchwOffset(batchIndex, 1, y, x, 2, gridH, gridW)]);
                const float centerX = (static_cast<float>(x) + offX) * strideX;
                const float centerY = (static_cast<float>(y) + offY) * strideY;

                const float left = SoftplusFloat(box[NchwOffset(batchIndex, 0, y, x, 4, gridH, gridW)]) * strideX;
                const float top = SoftplusFloat(box[NchwOffset(batchIndex, 1, y, x, 4, gridH, gridW)]) * strideY;
                const float right = SoftplusFloat(box[NchwOffset(batchIndex, 2, y, x, 4, gridH, gridW)]) * strideX;
                const float bottom = SoftplusFloat(box[NchwOffset(batchIndex, 3, y, x, 4, gridH, gridW)]) * strideY;

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

    if (config.max_detections > 0 && detections.size() > static_cast<size_t>(config.max_detections))
    {
        const auto kth = detections.begin() + config.max_detections;
        std::nth_element(
            detections.begin(),
            kth,
            detections.end(),
            [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
        detections.resize(static_cast<size_t>(config.max_detections));
    }

    std::sort(
        detections.begin(),
        detections.end(),
        [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
    NMS(detections, nmsThreshold);
    return detections;
}

size_t PostprocessOutputs(
    std::vector<Ort::Value>& outputTensors,
    const ModelInfo& info,
    int targetW,
    int targetH,
    int outputResolution)
{
    if (outputTensors.empty())
        return 0;

    const float confThreshold = config.confidence_threshold;
    const float nmsThreshold = config.nms_threshold;
    size_t detectionCount = 0;

    const int heatIdx = FindOutputIndex(info.outputNames, "heat");
    const int boxIdx = FindOutputIndex(info.outputNames, "box");
    const int offsetIdx = FindOutputIndex(info.outputNames, "offset");
    if (heatIdx >= 0 && boxIdx >= 0 && offsetIdx >= 0 &&
        heatIdx < static_cast<int>(outputTensors.size()) &&
        boxIdx < static_cast<int>(outputTensors.size()) &&
        offsetIdx < static_cast<int>(outputTensors.size()))
    {
        float* heatData = outputTensors[heatIdx].GetTensorMutableData<float>();
        float* boxData = outputTensors[boxIdx].GetTensorMutableData<float>();
        float* offsetData = outputTensors[offsetIdx].GetTensorMutableData<float>();
        const std::vector<int64_t> heatShape = outputTensors[heatIdx].GetTensorTypeAndShapeInfo().GetShape();
        const std::vector<int64_t> boxShape = outputTensors[boxIdx].GetTensorTypeAndShapeInfo().GetShape();
        const std::vector<int64_t> offsetShape = outputTensors[offsetIdx].GetTensorTypeAndShapeInfo().GetShape();
        const int batch = IsShape4(heatShape) ? static_cast<int>(heatShape[0]) : info.batch;

        for (int b = 0; b < batch; ++b)
        {
            auto detections = DecodeSunPointRaw(
                heatData, heatShape, boxData, boxShape, offsetData, offsetShape,
                b, targetW, targetH, confThreshold, nmsThreshold);
            detectionCount += detections.size();
        }
        return detectionCount;
    }

    Ort::TensorTypeAndShapeInfo outInfo = outputTensors.front().GetTensorTypeAndShapeInfo();
    std::vector<int64_t> outShape = outInfo.GetShape();
    if (outShape.size() < 2)
        return 0;

    float* outData = outputTensors.front().GetTensorMutableData<float>();
    if (!outData)
        return 0;

    int batch = 1;
    int rows = 0;
    int cols = 0;
    if (outShape.size() == 3)
    {
        if (!TryInt64ToInt(outShape[0], &batch) ||
            !TryInt64ToInt(outShape[1], &rows) ||
            !TryInt64ToInt(outShape[2], &cols))
        {
            return 0;
        }
    }
    else
    {
        if (!TryInt64ToInt(outShape[0], &rows) ||
            !TryInt64ToInt(outShape[1], &cols))
        {
            return 0;
        }
    }

    if (batch <= 0 || rows <= 0 || cols <= 0)
        return 0;

    const int numClasses = rows - 4;
    const size_t frameOutputSize = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    for (int b = 0; b < batch; ++b)
    {
        const float* ptr = outData + static_cast<size_t>(b) * frameOutputSize;
        std::vector<int64_t> shape2d = { rows, cols };
        auto detections = postProcessYoloDML(ptr, shape2d, numClasses, confThreshold, nmsThreshold);

        if (targetW != outputResolution || targetH != outputResolution)
        {
            const float scaleX = static_cast<float>(outputResolution) / static_cast<float>(targetW);
            const float scaleY = static_cast<float>(outputResolution) / static_cast<float>(targetH);
            for (auto& det : detections)
            {
                det.box.x = static_cast<int>(det.box.x * scaleX);
                det.box.y = static_cast<int>(det.box.y * scaleY);
                det.box.width = static_cast<int>(det.box.width * scaleX);
                det.box.height = static_cast<int>(det.box.height * scaleY);
            }
        }

        detectionCount += detections.size();
    }

    return detectionCount;
}

void ConfigureCommonSessionOptions(Ort::SessionOptions& sessionOptions, const CliOptions& options, ProviderKind kind)
{
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    sessionOptions.SetLogSeverityLevel(static_cast<int>(ORT_LOGGING_LEVEL_FATAL));

    if (kind == ProviderKind::DmlGpu || kind == ProviderKind::DmlCpu)
    {
        sessionOptions.DisableMemPattern();
        sessionOptions.SetIntraOpNumThreads(1);
        sessionOptions.SetInterOpNumThreads(1);
    }

    if (options.disableCpuFallback && kind != ProviderKind::Cpu)
    {
        sessionOptions.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    }
}

#ifdef AIMBOT_HAS_DIRECTML_EX
bool CreateDmlWarpResources(DmlCpuResources* resources, std::string* error)
{
    if (!resources)
        return false;

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        if (error) *error = "CreateDXGIFactory1 failed: " + HResultToString(hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> warpAdapter;
    hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
    if (FAILED(hr))
    {
        if (error) *error = "EnumWarpAdapter failed: " + HResultToString(hr);
        return false;
    }

    hr = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&resources->d3d12Device));
    if (FAILED(hr))
    {
        if (error) *error = "D3D12CreateDevice(WARP) failed: " + HResultToString(hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    hr = resources->d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&resources->commandQueue));
    if (FAILED(hr))
    {
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = resources->d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&resources->commandQueue));
    }
    if (FAILED(hr))
    {
        if (error) *error = "CreateCommandQueue(WARP) failed: " + HResultToString(hr);
        return false;
    }

    hr = DMLCreateDevice(resources->d3d12Device.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&resources->dmlDevice));
    if (FAILED(hr))
    {
        if (error) *error = "DMLCreateDevice(WARP) failed: " + HResultToString(hr);
        return false;
    }

    return true;
}
#endif

void AppendProvider(Ort::SessionOptions& sessionOptions, ProviderKind kind, const CliOptions& options, DmlCpuResources* dmlCpuResources)
{
    switch (kind)
    {
    case ProviderKind::Cpu:
        return;
    case ProviderKind::DmlGpu:
    {
        const int deviceId = options.dmlDeviceId >= 0 ? options.dmlDeviceId : config.dml_device_id;
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(sessionOptions, deviceId));
        return;
    }
    case ProviderKind::DmlCpu:
    {
#ifdef AIMBOT_HAS_DIRECTML_EX
        std::string error;
        if (!CreateDmlWarpResources(dmlCpuResources, &error))
            throw std::runtime_error(error);
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProviderEx_DML(
            sessionOptions,
            dmlCpuResources->dmlDevice.Get(),
            dmlCpuResources->commandQueue.Get()));
        return;
#else
        throw std::runtime_error("DirectML WARP support is unavailable because DirectML.lib was not linked.");
#endif
    }
    }
}

ModelInfo ReadModelInfo(Ort::Session& session, Ort::AllocatorWithDefaultOptions& allocator, const CliOptions& options)
{
    ModelInfo info;

    auto inputName = session.GetInputNameAllocated(0, allocator);
    info.inputName = inputName.get();

    Ort::TypeInfo inputTypeInfo = session.GetInputTypeInfo(0);
    auto inputTensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
    if (inputTensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("Only float32 model inputs are supported by the provider benchmark.");

    std::vector<int64_t> inputShape = inputTensorInfo.GetShape();
    if (inputShape.size() != 4)
        throw std::runtime_error("Only NCHW 4D model inputs are supported by the provider benchmark.");

    if (inputShape[0] > 0)
    {
        if (!TryInt64ToInt(inputShape[0], &info.batch) || info.batch <= 0)
            throw std::runtime_error("Invalid static batch size in model input.");
    }
    else
    {
        info.batch = options.batch;
    }

    if (inputShape[1] > 0)
    {
        if (!TryInt64ToInt(inputShape[1], &info.channels) || info.channels != 3)
            throw std::runtime_error("Only 3-channel NCHW model inputs are supported by the provider benchmark.");
    }
    else
    {
        info.channels = 3;
    }

    if (inputShape[2] > 0)
    {
        if (!TryInt64ToInt(inputShape[2], &info.inputH) || info.inputH <= 0)
            throw std::runtime_error("Invalid static input height in model.");
    }
    else
    {
        info.inputH = options.resolution;
    }

    if (inputShape[3] > 0)
    {
        if (!TryInt64ToInt(inputShape[3], &info.inputW) || info.inputW <= 0)
            throw std::runtime_error("Invalid static input width in model.");
    }
    else
    {
        info.inputW = options.resolution;
    }

    const size_t outputCount = session.GetOutputCount();
    if (outputCount == 0)
        throw std::runtime_error("Model has no outputs.");

    info.outputNames.reserve(outputCount);
    for (size_t i = 0; i < outputCount; ++i)
    {
        auto outputName = session.GetOutputNameAllocated(i, allocator);
        info.outputNames.emplace_back(outputName.get());
    }

    info.outputNamePtrs.reserve(info.outputNames.size());
    for (const auto& name : info.outputNames)
    {
        info.outputNamePtrs.push_back(name.c_str());
    }

    return info;
}

bool IsProviderAvailableForOrt(const std::vector<std::string>& availableProviders, ProviderKind provider)
{
    switch (provider)
    {
    case ProviderKind::Cpu:
        return true;
    case ProviderKind::DmlGpu:
    case ProviderKind::DmlCpu:
        return HasOrtProvider(availableProviders, "DmlExecutionProvider");
    }
    return false;
}

bool CanInitializeModelForProvider(
    Ort::Env& env,
    const std::filesystem::path& modelPath,
    ProviderKind provider,
    const CliOptions& options)
{
    try
    {
        Ort::SessionOptions sessionOptions;
        ConfigureCommonSessionOptions(sessionOptions, options, provider);

        DmlCpuResources dmlCpuResources;
        AppendProvider(sessionOptions, provider, options, &dmlCpuResources);

        const std::wstring modelPathWide = modelPath.wstring();
        Ort::Session session(env, modelPathWide.c_str(), sessionOptions);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::filesystem::path SelectModelPath(
    Ort::Env& env,
    const CliOptions& options,
    const std::vector<std::string>& availableProviders,
    std::string* selectionNote)
{
    const std::vector<std::filesystem::path> candidates = CollectModelCandidates(options);
    if (candidates.empty())
        return {};

    if (!options.modelPath.empty())
    {
        if (selectionNote)
            *selectionNote = "explicit";
        return candidates.front();
    }

    std::vector<ProviderKind> providersToProbe;
    for (ProviderKind provider : options.providers)
    {
        if (IsProviderAvailableForOrt(availableProviders, provider))
            providersToProbe.push_back(provider);
    }

    if (providersToProbe.empty())
    {
        if (selectionNote)
            *selectionNote = "auto:first-onnx";
        return candidates.front();
    }

    for (const auto& candidate : candidates)
    {
        bool compatible = true;
        for (ProviderKind provider : providersToProbe)
        {
            if (!CanInitializeModelForProvider(env, candidate, provider, options))
            {
                compatible = false;
                break;
            }
        }

        if (compatible)
        {
            if (selectionNote)
            {
                *selectionNote = (candidate == candidates.front())
                    ? "auto:first-compatible"
                    : "auto:skipped-incompatible-onnx";
            }
            return candidate;
        }
    }

    if (selectionNote)
        *selectionNote = "auto:no-compatible-candidate; using first ONNX";
    return candidates.front();
}

void RunOneIteration(
    Ort::Session& session,
    const ModelInfo& info,
    const CliOptions& options,
    const cv::Mat& frame,
    std::vector<float>* inputTensorValues,
    PreprocessWorkspace* preprocessWorkspace,
    double* preprocessSeconds,
    double* inferenceSeconds,
    double* postprocessSeconds,
    size_t* detections)
{
    auto t0 = Clock::now();
    PreprocessBatch(frame, info.batch, info.inputW, info.inputH, inputTensorValues, preprocessWorkspace);

    std::vector<int64_t> inputShape{
        static_cast<int64_t>(info.batch),
        static_cast<int64_t>(info.channels),
        static_cast<int64_t>(info.inputH),
        static_cast<int64_t>(info.inputW)
    };

    Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memoryInfo,
        inputTensorValues->data(),
        inputTensorValues->size(),
        inputShape.data(),
        inputShape.size());
    auto t1 = Clock::now();

    const char* inputNames[] = { info.inputName.c_str() };
    auto outputTensors = session.Run(
        Ort::RunOptions{ nullptr },
        inputNames,
        &inputTensor,
        1,
        info.outputNamePtrs.data(),
        info.outputNamePtrs.size());
    auto t2 = Clock::now();

    size_t detectionCount = 0;
    if (options.postprocess)
    {
        detectionCount = PostprocessOutputs(outputTensors, info, info.inputW, info.inputH, options.resolution);
    }
    auto t3 = Clock::now();

    if (preprocessSeconds)
        *preprocessSeconds += std::chrono::duration<double>(t1 - t0).count();
    if (inferenceSeconds)
        *inferenceSeconds += std::chrono::duration<double>(t2 - t1).count();
    if (postprocessSeconds)
        *postprocessSeconds += std::chrono::duration<double>(t3 - t2).count();
    if (detections)
        *detections = detectionCount;
}

ProviderResult RunProviderBenchmark(
    Ort::Env& env,
    ProviderKind provider,
    const CliOptions& options,
    const std::filesystem::path& modelPath,
    const cv::Mat& frame,
    const std::vector<std::string>& availableProviders)
{
    ProviderResult result;
    result.provider = ProviderName(provider);
    result.providerModel = modelPath.string();
    result.requestedBatch = options.batch;
    result.runs = options.runs;
    result.warmupRuns = options.warmupRuns;

    if ((provider == ProviderKind::DmlGpu || provider == ProviderKind::DmlCpu) &&
        !HasOrtProvider(availableProviders, "DmlExecutionProvider"))
    {
        result.status = "unavailable";
        result.error = "DmlExecutionProvider is not available in the current ONNX Runtime package/runtime DLLs.";
        return result;
    }

    auto loadStart = Clock::now();
    try
    {
        Ort::SessionOptions sessionOptions;
        ConfigureCommonSessionOptions(sessionOptions, options, provider);

        DmlCpuResources dmlCpuResources;
        AppendProvider(sessionOptions, provider, options, &dmlCpuResources);

        const std::wstring modelPathWide = modelPath.wstring();
        Ort::Session session(env, modelPathWide.c_str(), sessionOptions);
        Ort::AllocatorWithDefaultOptions allocator;
        ModelInfo info = ReadModelInfo(session, allocator, options);

        result.effectiveBatch = info.batch;
        result.inputW = info.inputW;
        result.inputH = info.inputH;
        result.loadSeconds = std::chrono::duration<double>(Clock::now() - loadStart).count();

        std::vector<float> inputTensorValues;
        PreprocessWorkspace preprocessWorkspace;

        auto warmupStart = Clock::now();
        for (int i = 0; i < options.warmupRuns; ++i)
        {
            RunOneIteration(
                session,
                info,
                options,
                frame,
                &inputTensorValues,
                &preprocessWorkspace,
                nullptr,
                nullptr,
                nullptr,
                &result.lastDetections);
        }
        result.warmupSeconds = std::chrono::duration<double>(Clock::now() - warmupStart).count();

        auto totalStart = Clock::now();
        for (int i = 0; i < options.runs; ++i)
        {
            RunOneIteration(
                session,
                info,
                options,
                frame,
                &inputTensorValues,
                &preprocessWorkspace,
                &result.preprocessSeconds,
                &result.inferenceSeconds,
                &result.postprocessSeconds,
                &result.lastDetections);
        }
        result.totalSeconds = std::chrono::duration<double>(Clock::now() - totalStart).count();
    }
    catch (const std::exception& e)
    {
        result.status = "failed";
        result.error = e.what();
        result.loadSeconds = std::chrono::duration<double>(Clock::now() - loadStart).count();
    }

    return result;
}

std::string ModelFamilyName(const std::filesystem::path& modelPath);

void PrintSummary(
    const CliOptions& options,
    const std::filesystem::path& modelPath,
    const std::string& modelSelection,
    const std::vector<std::string>& availableProviders,
    const std::vector<ProviderResult>& results)
{
    std::cout << "\nProvider benchmark summary (seconds)\n";
    std::cout << "model_family=" << ModelFamilyName(modelPath) << "\n";
    std::cout << "onnx_model=" << modelPath.string() << "\n";
    if (!modelSelection.empty())
        std::cout << "model_selection=" << modelSelection << "\n";
    std::cout << "providers_requested=";
    for (size_t i = 0; i < options.providers.size(); ++i)
    {
        if (i != 0) std::cout << ",";
        std::cout << ProviderName(options.providers[i]);
    }
    std::cout << "\n";
    std::cout << "available_ort_providers=" << JoinStrings(availableProviders, "|") << "\n";
    std::cout << "resolution=" << options.resolution
              << " requested_batch=" << options.batch
              << " runs=" << options.runs
              << " warmup=" << options.warmupRuns
              << " postprocess=" << (options.postprocess ? "true" : "false")
              << " disable_cpu_fallback=" << (options.disableCpuFallback ? "true" : "false")
              << "\n";
    const int dmlDeviceId = options.dmlDeviceId >= 0 ? options.dmlDeviceId : config.dml_device_id;
    std::cout << "dml_gpu_device_id=" << dmlDeviceId
              << " dml_gpu_device_name=" << AdapterName(dmlDeviceId)
              << "\n\n";

    std::cout
        << "provider,provider_model,status,runs,warmup,requested_batch,effective_batch,input_w,input_h,"
        << "load_s,warmup_s,total_s,preprocess_s,inference_s,postprocess_s,avg_run_s,avg_frame_s,fps,last_detections,error\n";

    std::cout << std::fixed << std::setprecision(6);
    for (const ProviderResult& result : results)
    {
        const double avgRun = (result.runs > 0 && result.totalSeconds > 0.0)
            ? result.totalSeconds / static_cast<double>(result.runs)
            : 0.0;
        const double totalFrames = static_cast<double>(std::max(result.runs, 0)) * static_cast<double>(std::max(result.effectiveBatch, 1));
        const double avgFrame = (totalFrames > 0.0 && result.totalSeconds > 0.0)
            ? result.totalSeconds / totalFrames
            : 0.0;
        const double fps = (result.totalSeconds > 0.0) ? totalFrames / result.totalSeconds : 0.0;

        std::cout
            << result.provider << ","
            << CsvEscape(result.providerModel) << ","
            << result.status << ","
            << result.runs << ","
            << result.warmupRuns << ","
            << result.requestedBatch << ","
            << result.effectiveBatch << ","
            << result.inputW << ","
            << result.inputH << ","
            << result.loadSeconds << ","
            << result.warmupSeconds << ","
            << result.totalSeconds << ","
            << result.preprocessSeconds << ","
            << result.inferenceSeconds << ","
            << result.postprocessSeconds << ","
            << avgRun << ","
            << avgFrame << ","
            << fps << ","
            << result.lastDetections << ","
            << CsvEscape(result.error)
            << "\n";
    }
}

std::string ProvidersRequestedString(const std::vector<ProviderKind>& providers)
{
    std::ostringstream oss;
    for (size_t i = 0; i < providers.size(); ++i)
    {
        if (i != 0)
            oss << "|";
        oss << ProviderName(providers[i]);
    }
    return oss.str();
}

std::string ModelFamilyName(const std::filesystem::path& modelPath)
{
    return modelPath.stem().string();
}

std::string BenchmarkCsvHeader()
{
    return
        "timestamp_local,commit_id,git_dirty,build_backend,model_family,onnx_model,model_selection,providers_requested,"
        "available_ort_providers,resolution,postprocess,disable_cpu_fallback,dml_gpu_device_id,"
        "dml_gpu_device_name,provider,provider_model,status,runs,warmup,requested_batch,"
        "effective_batch,input_w,input_h,load_s,warmup_s,total_s,preprocess_s,inference_s,"
        "postprocess_s,avg_run_s,avg_frame_s,fps,last_detections,error";
}

std::filesystem::path NextLegacyCsvPath(const std::filesystem::path& csvPath)
{
    const std::filesystem::path parent = csvPath.parent_path();
    const std::string stem = csvPath.stem().string();
    const std::string ext = csvPath.extension().string();

    for (int i = 1; i < 1000; ++i)
    {
        const std::string suffix = (i == 1) ? ".legacy" : ".legacy." + std::to_string(i);
        std::filesystem::path candidate = parent / (stem + suffix + ext);
        if (!std::filesystem::exists(candidate))
            return candidate;
    }

    return parent / (stem + ".legacy.latest" + ext);
}

bool PrepareCsvForAppend(
    const std::filesystem::path& csvPath,
    const std::string& expectedHeader,
    bool* writeHeader)
{
    *writeHeader = true;

    std::error_code ec;
    if (!std::filesystem::exists(csvPath, ec))
        return true;

    const uintmax_t size = std::filesystem::file_size(csvPath, ec);
    if (ec)
    {
        std::cerr << "[Benchmark] Failed to inspect results CSV: " << csvPath.string()
                  << " (" << ec.message() << ")" << std::endl;
        return false;
    }
    if (size == 0)
        return true;

    std::ifstream existing(csvPath);
    if (!existing)
    {
        std::cerr << "[Benchmark] Failed to read results CSV header: " << csvPath.string() << std::endl;
        return false;
    }

    std::string firstLine;
    std::getline(existing, firstLine);
    if (!firstLine.empty() && firstLine.back() == '\r')
        firstLine.pop_back();

    if (firstLine == expectedHeader)
    {
        *writeHeader = false;
        return true;
    }
    existing.close();

    const std::filesystem::path legacyPath = NextLegacyCsvPath(csvPath);
    std::filesystem::rename(csvPath, legacyPath, ec);
    if (ec)
    {
        std::cerr << "[Benchmark] Failed to rotate incompatible results CSV: " << csvPath.string()
                  << " -> " << legacyPath.string()
                  << " (" << ec.message() << ")" << std::endl;
        return false;
    }

    return true;
}

bool AppendBenchmarkCsv(
    const CliOptions& options,
    const std::filesystem::path& modelPath,
    const std::string& modelSelection,
    const std::vector<std::string>& availableProviders,
    const std::vector<ProviderResult>& results,
    std::filesystem::path* writtenPath)
{
    std::filesystem::path csvPath = options.resultsPath.empty()
        ? std::filesystem::path("benchmark_results/provider_benchmark.csv")
        : std::filesystem::path(options.resultsPath);
    const std::filesystem::path repoRoot = FindRepoRoot();
    if (csvPath.is_relative() && !repoRoot.empty())
        csvPath = repoRoot / csvPath;

    std::error_code ec;
    const std::filesystem::path parent = csvPath.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    if (ec)
    {
        std::cerr << "[Benchmark] Failed to create results directory: " << parent.string()
                  << " (" << ec.message() << ")" << std::endl;
        return false;
    }

    const std::string csvHeader = BenchmarkCsvHeader();
    bool writeHeader = true;
    if (!PrepareCsvForAppend(csvPath, csvHeader, &writeHeader))
        return false;

    std::ofstream file(csvPath, std::ios::app);
    if (!file)
    {
        std::cerr << "[Benchmark] Failed to open results CSV: " << csvPath.string() << std::endl;
        return false;
    }

    if (writeHeader)
    {
        file << csvHeader << "\n";
    }

    const std::string commitId = GetGitCommitId(repoRoot);
    const bool gitDirty = GetGitDirty(repoRoot);
    const std::string timestamp = CurrentTimestampLocal();
    const std::string requestedProviders = ProvidersRequestedString(options.providers);
    const std::string availableProviderText = JoinStrings(availableProviders, "|");
    const std::string modelFamily = ModelFamilyName(modelPath);
    const int dmlDeviceId = options.dmlDeviceId >= 0 ? options.dmlDeviceId : config.dml_device_id;
    const std::string dmlDeviceName = AdapterName(dmlDeviceId);
    const char* buildBackend = "dml";

    file << std::fixed << std::setprecision(6);
    for (const ProviderResult& result : results)
    {
        const double avgRun = (result.runs > 0 && result.totalSeconds > 0.0)
            ? result.totalSeconds / static_cast<double>(result.runs)
            : 0.0;
        const double totalFrames = static_cast<double>(std::max(result.runs, 0)) * static_cast<double>(std::max(result.effectiveBatch, 1));
        const double avgFrame = (totalFrames > 0.0 && result.totalSeconds > 0.0)
            ? result.totalSeconds / totalFrames
            : 0.0;
        const double fps = (result.totalSeconds > 0.0) ? totalFrames / result.totalSeconds : 0.0;

        file
            << CsvEscape(timestamp) << ","
            << CsvEscape(commitId) << ","
            << (gitDirty ? "true" : "false") << ","
            << buildBackend << ","
            << CsvEscape(modelFamily) << ","
            << CsvEscape(modelPath.string()) << ","
            << CsvEscape(modelSelection) << ","
            << CsvEscape(requestedProviders) << ","
            << CsvEscape(availableProviderText) << ","
            << options.resolution << ","
            << (options.postprocess ? "true" : "false") << ","
            << (options.disableCpuFallback ? "true" : "false") << ","
            << dmlDeviceId << ","
            << CsvEscape(dmlDeviceName) << ","
            << result.provider << ","
            << CsvEscape(result.providerModel) << ","
            << result.status << ","
            << result.runs << ","
            << result.warmupRuns << ","
            << result.requestedBatch << ","
            << result.effectiveBatch << ","
            << result.inputW << ","
            << result.inputH << ","
            << result.loadSeconds << ","
            << result.warmupSeconds << ","
            << result.totalSeconds << ","
            << result.preprocessSeconds << ","
            << result.inferenceSeconds << ","
            << result.postprocessSeconds << ","
            << avgRun << ","
            << avgFrame << ","
            << fps << ","
            << result.lastDetections << ","
            << CsvEscape(result.error)
            << "\n";
    }

    if (writtenPath)
        *writtenPath = csvPath;
    return true;
}
} // namespace

bool IsProviderBenchmarkRequested(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--benchmark-providers" ||
            StartsWith(arg, "--benchmark-providers=") ||
            arg == "--bench-providers" ||
            StartsWith(arg, "--bench-providers=") ||
            arg == "--benchmark-help" ||
            arg == "--bench-help" ||
            arg == "--bench-list-devices")
        {
            return true;
        }
    }
    return false;
}

int RunProviderBenchmarkCli(int argc, char** argv)
{
    SetEnvironmentVariableA("ORT_LOG_SEVERITY_LEVEL", "4");
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_FATAL);

    CliOptions options = ParseCli(argc, argv);
    if (options.help)
    {
        PrintHelp();
        return 0;
    }

    if (options.listDevices && !options.runBenchmark)
    {
        PrintDxgiAdapters();
        return 0;
    }

    if (!config.loadConfig())
    {
        std::cerr << "[Benchmark] Failed to load config.ini." << std::endl;
        return 2;
    }

    if (options.listDevices)
    {
        PrintDxgiAdapters();
        return 0;
    }

    if (options.resolution <= 0)
        options.resolution = config.detection_resolution > 0 ? config.detection_resolution : 320;
    options.resolution = std::clamp(options.resolution, 32, 4096);
    options.runs = std::max(1, options.runs);
    options.warmupRuns = std::max(0, options.warmupRuns);
    options.batch = std::max(1, options.batch);
    if (options.dmlDeviceId < 0)
        options.dmlDeviceId = config.dml_device_id;

    Ort::Env env(ORT_LOGGING_LEVEL_FATAL, "provider_benchmark");
    std::vector<std::string> availableProviders;
    try
    {
        availableProviders = Ort::GetAvailableProviders();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Benchmark] Failed to query ONNX Runtime providers: " << CsvEscape(e.what()) << std::endl;
    }

    std::string modelSelection;
    std::filesystem::path modelPath = SelectModelPath(env, options, availableProviders, &modelSelection);
    if (modelPath.empty() || !std::filesystem::exists(modelPath))
    {
        std::cerr << "[Benchmark] ONNX model was not found. Pass --bench-model <path> or put an .onnx model in models." << std::endl;
        return 2;
    }
    if (ToLower(modelPath.extension().string()) != ".onnx")
    {
        std::cerr << "[Benchmark] Provider benchmark requires an .onnx model: " << modelPath.string() << std::endl;
        return 2;
    }

    cv::Mat frame = LoadBenchmarkFrame(options);
    if (frame.empty())
    {
        std::cerr << "[Benchmark] Failed to prepare benchmark input frame." << std::endl;
        return 2;
    }

    std::vector<ProviderResult> results;
    results.reserve(options.providers.size());
    for (ProviderKind provider : options.providers)
    {
        results.push_back(RunProviderBenchmark(env, provider, options, modelPath, frame, availableProviders));
    }

    PrintSummary(options, modelPath, modelSelection, availableProviders, results);
    if (options.saveResults)
    {
        std::filesystem::path csvPath;
        if (AppendBenchmarkCsv(options, modelPath, modelSelection, availableProviders, results, &csvPath))
        {
            std::cout << "results_csv=" << csvPath.string() << "\n";
            std::cout << "results_csv_base=repo_root\n";
        }
    }
    return 0;
}
} // namespace benchmarks
#endif
