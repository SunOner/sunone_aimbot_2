#ifndef OTHER_TOOLS_H
#define OTHER_TOOLS_H

#include <string>
#include <vector>
#include <Windows.h>

struct CaptureWindowInfo
{
    HWND hwnd = nullptr;
    std::string title;
    std::string displayName;
    bool minimized = false;
};

namespace OtherTools
{
inline int MaxInt(int a, int b) noexcept
{
    return (a > b) ? a : b;
}

inline float MaxFloat(float a, float b) noexcept
{
    return (a > b) ? a : b;
}

std::string TrimAscii(std::string s);
std::string ToLowerAscii(std::string s);
bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle);
bool HasExtensionCaseInsensitive(const std::string& path, const char* ext);
}

bool fileExists(const std::string& path);



std::string get_ghub_version();
HMONITOR GetMonitorHandleByIndex(int monitorIndex);
double GetMonitorRefreshRateByIndex(int monitorIndex);
void SetRandomConsoleTitle();
std::vector<CaptureWindowInfo> EnumerateCaptureWindows();
HWND FindCaptureWindowByTitle(const std::string& title);

std::vector<std::string> getAvailableModels();

void welcome_message();
std::string WideToUtf8(const std::wstring& ws);
#endif // OTHER_TOOLS_H
