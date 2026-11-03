#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <Windows.h>

#include <string>
#include <iostream>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include <dwmapi.h>


#include "other_tools.h"
#include "config.h"
#include "sunone_aimbot_2.h"

#pragma comment(lib, "dwmapi.lib")


namespace OtherTools
{
std::string TrimAscii(std::string s)
{
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};

    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end == std::string::npos ? std::string::npos : (end - start + 1));
}

std::string ToLowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle)
{
    const std::string lowerHaystack = ToLowerAscii(haystack);
    const std::string lowerNeedle = ToLowerAscii(needle);
    return lowerHaystack.find(lowerNeedle) != std::string::npos;
}

bool HasExtensionCaseInsensitive(const std::string& path, const char* ext)
{
    if (!ext || !*ext)
        return false;

    const std::string current = ToLowerAscii(std::filesystem::path(path).extension().string());
    const std::string expected = ToLowerAscii(ext);
    return current == expected;
}
}

static std::vector<std::string> GetModelFilesByExtInDir(const std::string& dir, const std::vector<std::string>& exts)
{
    std::vector<std::string> files;

    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec)
    {
        return files;
    }

    for (const auto& entry : it)
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        std::string ext = OtherTools::ToLowerAscii(entry.path().extension().string());

        const bool match = std::any_of(exts.begin(), exts.end(),
            [&ext](const std::string& e) { return ext == e; });
        if (match)
        {
            files.push_back(entry.path().filename().string());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

static std::vector<std::string> GetModelFilesByExt(const std::vector<std::string>& exts)
{
    return GetModelFilesByExtInDir("models/", exts);
}

std::string WideToUtf8(const std::wstring& ws)
{
    if (ws.empty())
        return {};

    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
        nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};

    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
        out.data(), len, nullptr, nullptr);
    return out;
}

static std::string GetWindowProcessName(HWND hwnd)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0)
        return {};

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return {};

    wchar_t imagePath[MAX_PATH]{};
    DWORD imagePathLen = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, imagePath, &imagePathLen);
    CloseHandle(process);

    if (!ok || imagePathLen == 0)
        return {};

    std::filesystem::path path(std::wstring(imagePath, imagePathLen));
    return WideToUtf8(path.filename().wstring());
}

static std::string MakeCaptureWindowDisplayName(HWND hwnd, const std::string& title, bool minimized)
{
    std::string display = title;

    const std::string processName = GetWindowProcessName(hwnd);
    if (!processName.empty())
        display += " - " + processName;

    if (minimized)
        display += " [minimized]";

    char hwndText[32]{};
    std::snprintf(hwndText, sizeof(hwndText), "%p", reinterpret_cast<void*>(hwnd));
    display += " [" + std::string(hwndText) + "]";

    return display;
}

std::vector<CaptureWindowInfo> EnumerateCaptureWindows()
{
    std::vector<CaptureWindowInfo> windows;

    ::EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* out = reinterpret_cast<std::vector<CaptureWindowInfo>*>(lParam);
        if (!out || !::IsWindowVisible(hwnd))
            return TRUE;

        BOOL isCloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked))) && isCloaked)
            return TRUE;

        const int titleLength = ::GetWindowTextLengthW(hwnd);
        if (titleLength <= 0)
            return TRUE;

        std::vector<wchar_t> titleBuffer(static_cast<size_t>(titleLength) + 1);
        const int copied = ::GetWindowTextW(hwnd, titleBuffer.data(), static_cast<int>(titleBuffer.size()));
        if (copied <= 0)
            return TRUE;

        const std::string title = OtherTools::TrimAscii(WideToUtf8(std::wstring(titleBuffer.data(), copied)));
        if (title.empty())
            return TRUE;

        CaptureWindowInfo info;
        info.hwnd = hwnd;
        info.title = title;
        info.minimized = ::IsIconic(hwnd) != FALSE;
        info.displayName = MakeCaptureWindowDisplayName(hwnd, title, info.minimized);
        out->push_back(std::move(info));

        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));

    return windows;
}

HWND FindCaptureWindowByTitle(const std::string& title)
{
    const std::string needle = OtherTools::TrimAscii(title);
    if (needle.empty())
        return nullptr;

    const std::vector<CaptureWindowInfo> windows = EnumerateCaptureWindows();

    for (const auto& window : windows)
        if (window.title == needle)
            return window.hwnd;

    for (const auto& window : windows)
        if (window.title.find(needle) != std::string::npos)
            return window.hwnd;

    for (const auto& window : windows)
        if (OtherTools::ContainsCaseInsensitive(window.title, needle))
            return window.hwnd;

    return nullptr;
}

bool fileExists(const std::string& path)
{
    std::filesystem::path fsPath;
    try
    {
        fsPath = std::filesystem::u8path(path);
    }
    catch (...)
    {
        fsPath = std::filesystem::path(path);
    }

    std::error_code ec;
    return std::filesystem::exists(fsPath, ec) && !ec;
}

static std::vector<std::string> getEngineFiles()
{
    return GetModelFilesByExt({ ".engine" });
}

static std::vector<std::string> getOnnxFiles()
{
    return GetModelFilesByExt({ ".onnx" });
}

std::string get_ghub_version()
{
    std::string line;
    std::ifstream in("C:\\Program Files\\LGHUB\\version");
    if (in.is_open())
    {
        while (std::getline(in, line)) { }
    }
    in.close();

    if (!line.empty())
    {
        return line;
    }

	return "unknown";
}

HMONITOR GetMonitorHandleByIndex(int monitorIndex)
{
    monitorIndex = std::max(0, monitorIndex);

    struct MonitorSearch
    {
        int targetIndex;
        int currentIndex;
        HMONITOR targetMonitor;
    };

    MonitorSearch search = { monitorIndex, 0, nullptr };

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) -> BOOL
        {
            MonitorSearch* search = reinterpret_cast<MonitorSearch*>(lParam);
            if (search->currentIndex == search->targetIndex)
            {
                search->targetMonitor = hMonitor;
                return FALSE;
            }
            search->currentIndex++;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));

    return search.targetMonitor;
}

namespace
{
double RefreshRateFromRational(const DISPLAYCONFIG_RATIONAL& refreshRate)
{
    if (refreshRate.Numerator == 0 || refreshRate.Denominator == 0)
        return 0.0;

    return static_cast<double>(refreshRate.Numerator) /
        static_cast<double>(refreshRate.Denominator);
}

double GetDisplayConfigRefreshRateForDevice(const wchar_t* gdiDeviceName)
{
    if (!gdiDeviceName || gdiDeviceName[0] == L'\0')
        return 0.0;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (result != ERROR_SUCCESS || pathCount == 0 || modeCount == 0)
        return 0.0;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    result = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS,
        &pathCount,
        paths.data(),
        &modeCount,
        modes.data(),
        nullptr);
    if (result != ERROR_SUCCESS)
        return 0.0;

    paths.resize(pathCount);
    for (const auto& path : paths)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
            continue;

        if (wcscmp(sourceName.viewGdiDeviceName, gdiDeviceName) == 0)
        {
            const double refreshHz = RefreshRateFromRational(path.targetInfo.refreshRate);
            if (refreshHz > 0.0)
                return refreshHz;
        }
    }

    return 0.0;
}
}

double GetMonitorRefreshRateByIndex(int monitorIndex)
{
    HMONITOR monitor = GetMonitorHandleByIndex(monitorIndex);
    if (!monitor)
        return 0.0;

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return 0.0;

    double refreshHz = GetDisplayConfigRefreshRateForDevice(monitorInfo.szDevice);
    if (refreshHz > 0.0)
        return refreshHz;

    DEVMODEW devMode{};
    devMode.dmSize = sizeof(devMode);
    if (EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode) &&
        devMode.dmDisplayFrequency > 1)
    {
        return static_cast<double>(devMode.dmDisplayFrequency);
    }

    return 0.0;
}

std::vector<std::string> getAvailableModels()
{
    std::vector<std::string> availableModels;

    std::vector<std::string> engineFiles = getEngineFiles();
    std::vector<std::string> onnxFiles = getOnnxFiles();

    if (config.backend == "TRT")
    {
        std::set<std::string> engineModels;
        for (const auto& file : engineFiles)
        {
            engineModels.insert(std::filesystem::path(file).stem().string());
        }

        for (const auto& file : engineFiles)
        {
            availableModels.push_back(file);
        }

        for (const auto& file : onnxFiles)
        {
            std::string modelName = std::filesystem::path(file).stem().string();
            if (engineModels.find(modelName) == engineModels.end())
            {
                availableModels.push_back(file);
            }
        }
    }
    else
    {
        for (const auto& file : onnxFiles)
        {
            availableModels.push_back(file);
        }
    }

    return availableModels;
}

void SetRandomConsoleTitle()
{
    static constexpr std::array<const wchar_t*, 10> kTitles = {
        L"Microsoft Edge",
        L"Google Chrome",
        L"Notepad",
        L"Windows Terminal",
        L"PowerShell",
        L"Visual Studio Code",
        L"Task Manager",
        L"File Explorer",
        L"Calculator",
        L"Command Prompt",
    };

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, kTitles.size() - 1);

    ::SetConsoleTitleW(kTitles[dist(gen)]);
}

void welcome_message()
{
    std::cout <<
    "\n\nAimbot is started!\n" <<
    config.joinStrings(config.button_targeting) << " -> Targeting\n" <<
    config.joinStrings(config.button_exit) << " -> Exit\n" <<
    config.joinStrings(config.button_pause) << " -> Pause Aiming\n" <<
    config.joinStrings(config.button_reload_config) << " -> Reload settings and graphs\n" <<
    config.joinStrings(config.button_open_overlay) << " -> Overlay (OPTIONS)" <<
    std::endl;
}
