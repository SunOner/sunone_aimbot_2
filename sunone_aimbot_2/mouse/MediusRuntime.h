#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

class MediusRuntime
{
public:
    struct Status
    {
        bool runtimeLoaded = false;
        bool deviceConnected = false;
        bool updateChecked = false;
        bool latestKnown = false;
        bool latestSupported = false;
        std::string runtimeVersion;
        uint32_t runtimeAbi = 0;
        std::string latestVersion;
        uint32_t latestAbi = 0;
        std::string selectedPort = "AUTO";
        uint32_t controlBaud = 4'000'000;
        std::string message;
    };

    static MediusRuntime& instance()
    {
        static MediusRuntime value;
        return value;
    }

    static std::string supportedAbiLabel()
    {
        std::ostringstream out;
        for (size_t i = 0; i < supportedAbis().size(); ++i)
        {
            if (i)
                out << ", ";
            out << supportedAbis()[i];
        }
        return out.str();
    }

    static std::string supportedVersionLabel()
    {
        return "Any Medius C API release reporting ABI " + supportedAbiLabel();
    }

    static constexpr uint32_t controlBaudRate() noexcept
    {
        // Medius upstream currently fixes the control link at 4 Mbaud.
        return 4'000'000;
    }

    static constexpr bool customBaudSupported() noexcept
    {
        return false;
    }

    Status status() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return status_;
    }

    std::string preferredPort() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return preferredPort_;
    }

    bool setPreferredPort(const std::string& requested)
    {
        const std::string normalized = normalizePort(requested);
        if (normalized.empty())
            return false;

        std::lock_guard<std::mutex> lock(mutex_);
        preferredPort_ = normalized;
        status_.selectedPort = preferredPort_;
        saveSettingsUnlocked();
        status_.message = preferredPort_ == "AUTO"
            ? "Medius port set to automatic discovery."
            : "Medius port set to " + preferredPort_ + ".";
        return true;
    }

    // Kept as an API for the UI/config layer. The current upstream C API has no baud parameter,
    // so values other than 4,000,000 are rejected instead of pretending they were applied.
    bool setControlBaudRate(uint32_t baud)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (baud != controlBaudRate())
        {
            status_.message = "Current Medius C API fixes the control baud rate at 4000000.";
            return false;
        }
        status_.controlBaud = controlBaudRate();
        saveSettingsUnlocked();
        return true;
    }

    void requestForceUpdateCheck()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        checkedThisRun_ = false;
        status_.updateChecked = false;
        status_.latestKnown = false;
        status_.latestSupported = false;
        status_.latestVersion.clear();
        status_.latestAbi = 0;
        status_.message = "Medius update check requested; it will run on reconnect.";
    }

    bool connectMouse()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnectMouseUnlocked();

        // Check upstream first. A failed network check never destroys a working runtime.
        checkAndUpdateUnlocked();

        if (!loadInstalledUnlocked())
        {
            status_.message = "No supported Medius runtime is installed.";
            logStatusUnlocked();
            return false;
        }

        device_ = nullptr;
        int32_t openStatus = -1;
        if (preferredPort_ == "AUTO")
        {
            if (findMouseBox_)
                openStatus = findMouseBox_(&device_);
        }
        else
        {
            if (deviceOpen_)
                openStatus = deviceOpen_(preferredPort_.c_str(), &device_);
        }

        if (openStatus != 0 || !device_)
        {
            status_.deviceConnected = false;
            if (preferredPort_ == "AUTO")
                status_.message = "Medius runtime loaded, but no mouse box was found automatically" + lastErrorSuffixUnlocked() + ".";
            else
                status_.message = "Medius runtime loaded, but " + preferredPort_ + " could not be opened" + lastErrorSuffixUnlocked() + ".";
            logStatusUnlocked();
            return false;
        }

        status_.deviceConnected = true;
        status_.selectedPort = preferredPort_;
        status_.controlBaud = controlBaudRate();
        status_.message = preferredPort_ == "AUTO"
            ? "Medius connected using automatic mouse-box discovery."
            : "Medius connected on " + preferredPort_ + ".";
        logStatusUnlocked();
        return true;
    }

    void disconnectMouse()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnectMouseUnlocked();
    }

    bool move(int dx, int dy)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!device_ || !moveRelNow_)
            return false;
        dx = std::clamp(dx, -32768, 32767);
        dy = std::clamp(dy, -32768, 32767);
        return moveRelNow_(device_, static_cast<int16_t>(dx), static_cast<int16_t>(dy)) == 0;
    }

    bool leftDown()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return device_ && usageButton_ && press_ && press_(device_, usageButton_(0)) == 0;
    }

    bool leftUp()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return device_ && usageButton_ && softRelease_ && softRelease_(device_, usageButton_(0)) == 0;
    }

private:
    struct MediusDevice;
    struct MediusUsage
    {
        uint8_t kind;
        uint16_t id;
    };

    using AbiVersionFn = uint32_t (*)();
    using VersionStringFn = const char* (*)();
    using DeviceOpenFn = int32_t (*)(const char*, MediusDevice**);
    using FindMouseBoxFn = int32_t (*)(MediusDevice**);
    using DeviceFreeFn = void (*)(MediusDevice*);
    using MoveRelNowFn = int32_t (*)(MediusDevice*, int16_t, int16_t);
    using UsageButtonFn = MediusUsage (*)(uint8_t);
    using PressFn = int32_t (*)(MediusDevice*, MediusUsage);
    using SoftReleaseFn = int32_t (*)(MediusDevice*, MediusUsage);
    using LastErrorMessageFn = uintptr_t (*)(char*, uintptr_t);

    struct ReleaseInfo
    {
        std::string version;
        std::string url;
        std::string digest;
    };

    MediusRuntime()
    {
        loadSettingsUnlocked();
        status_.selectedPort = preferredPort_;
        status_.controlBaud = controlBaudRate();
    }

    ~MediusRuntime()
    {
        disconnectMouseUnlocked();
        unloadUnlocked();
    }

    static const std::vector<uint32_t>& supportedAbis()
    {
        static const std::vector<uint32_t> values{ 6 };
        return values;
    }

    static bool abiSupported(uint32_t abi)
    {
        const auto& values = supportedAbis();
        return std::find(values.begin(), values.end(), abi) != values.end();
    }

    static std::filesystem::path runtimeDir()
    {
        wchar_t buffer[32768]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
        std::filesystem::path base = n ? std::filesystem::path(buffer) : std::filesystem::temp_directory_path();
        return base / L"SunoneAimbot" / L"runtime" / L"medius";
    }

    static std::filesystem::path settingsPath()
    {
        return runtimeDir() / L"settings.ini";
    }

    static std::string trim(std::string value)
    {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    static std::string normalizePort(std::string value)
    {
        value = trim(std::move(value));
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });

        if (value == "AUTO")
            return value;

        const std::string win32Prefix = R"(\\.\)";
        if (value.rfind(win32Prefix, 0) == 0)
            value.erase(0, win32Prefix.size());

        if (value.size() <= 3 || value.rfind("COM", 0) != 0)
            return {};

        const std::string numberText = value.substr(3);
        if (numberText.empty() || !std::all_of(numberText.begin(), numberText.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return {};

        try
        {
            const unsigned long number = std::stoul(numberText);
            if (number == 0 || number > 4096)
                return {};
            return "COM" + std::to_string(number);
        }
        catch (...)
        {
            return {};
        }
    }

    void loadSettingsUnlocked()
    {
        preferredPort_ = "AUTO";
        std::ifstream in(settingsPath(), std::ios::binary);
        if (!in)
            return;

        std::string line;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = trim(line.substr(0, eq));
            const std::string value = trim(line.substr(eq + 1));
            if (key == "port")
            {
                const std::string normalized = normalizePort(value);
                if (!normalized.empty())
                    preferredPort_ = normalized;
            }
        }
    }

    void saveSettingsUnlocked() const
    {
        std::error_code ec;
        std::filesystem::create_directories(runtimeDir(), ec);
        std::ofstream out(settingsPath(), std::ios::binary | std::ios::trunc);
        if (!out)
            return;
        out << "port=" << preferredPort_ << "\n";
        out << "baud=" << controlBaudRate() << "\n";
    }

    static std::wstring quote(const std::filesystem::path& value)
    {
        std::wstring s = value.wstring();
        std::wstring out = L"\"";
        for (wchar_t ch : s)
        {
            if (ch == L'\"')
                out += L"`\"";
            else
                out += ch;
        }
        out += L"\"";
        return out;
    }

    static bool runPowerShell(const std::wstring& script)
    {
        const std::wstring command = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"$ErrorActionPreference='Stop'; " + script + L"\"";
        return _wsystem(command.c_str()) == 0;
    }

    static std::optional<std::string> jsonString(const std::string& json, const std::string& key)
    {
        const std::string needle = "\"" + key + "\"";
        size_t p = json.find(needle);
        if (p == std::string::npos)
            return std::nullopt;
        p = json.find(':', p + needle.size());
        if (p == std::string::npos)
            return std::nullopt;
        p = json.find('"', p + 1);
        if (p == std::string::npos)
            return std::nullopt;
        const size_t e = json.find('"', p + 1);
        if (e == std::string::npos)
            return std::nullopt;
        return json.substr(p + 1, e - p - 1);
    }

    static std::optional<ReleaseInfo> queryLatest(const std::filesystem::path& dir)
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto meta = dir / L"latest.json";
        const std::wstring script =
            L"$r=Invoke-RestMethod -Headers @{'User-Agent'='Sunone-Medius'} -Uri 'https://api.github.com/repos/K4HVH/medius/releases/latest'; "
            L"$a=$r.assets | Where-Object {$_.name -eq 'medius-capi-x86_64-pc-windows-msvc.tar.gz'} | Select-Object -First 1; "
            L"if(-not $a){exit 3}; "
            L"if(-not $a.digest){exit 5}; "
            L"[pscustomobject]@{version=$r.tag_name;url=$a.browser_download_url;digest=$a.digest} | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 " + quote(meta);
        if (!runPowerShell(script))
            return std::nullopt;

        std::ifstream in(meta, std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        ReleaseInfo out;
        auto version = jsonString(json, "version");
        auto url = jsonString(json, "url");
        auto digest = jsonString(json, "digest");
        if (!version || !url || !digest)
            return std::nullopt;
        out.version = *version;
        if (!out.version.empty() && out.version.front() == 'v')
            out.version.erase(out.version.begin());
        out.url = *url;
        out.digest = *digest;
        return out;
    }

    static std::filesystem::path findRecursive(const std::filesystem::path& root, const wchar_t* filename)
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
        {
            if (!ec && entry.is_regular_file() && _wcsicmp(entry.path().filename().c_str(), filename) == 0)
                return entry.path();
        }
        return {};
    }

    static bool probeDll(const std::filesystem::path& dll, uint32_t& abi, std::string& version)
    {
        HMODULE module = LoadLibraryW(dll.c_str());
        if (!module)
            return false;
        auto abiFn = reinterpret_cast<AbiVersionFn>(GetProcAddress(module, "medius_abi_version"));
        auto versionFn = reinterpret_cast<VersionStringFn>(GetProcAddress(module, "medius_version_string"));
        if (!abiFn || !versionFn)
        {
            FreeLibrary(module);
            return false;
        }
        abi = abiFn();
        const char* v = versionFn();
        version = v ? v : "unknown";
        FreeLibrary(module);
        return true;
    }

    bool checkAndUpdateUnlocked()
    {
        if (checkedThisRun_)
            return true;
        checkedThisRun_ = true;
        status_.updateChecked = true;

        const auto dir = runtimeDir();
        auto latest = queryLatest(dir);
        if (!latest)
        {
            status_.message = "Unable to check the latest Medius release; keeping installed runtime.";
            return false;
        }
        status_.latestVersion = latest->version;

        const auto archive = dir / L"medius-latest.tar.gz";
        const auto staging = dir / L"staging";
        std::error_code ec;
        std::filesystem::remove_all(staging, ec);
        ec.clear();
        std::filesystem::create_directories(staging, ec);

        std::wstring digest(latest->digest.begin(), latest->digest.end());
        if (digest.rfind(L"sha256:", 0) == 0)
            digest.erase(0, 7);
        std::wstring url(latest->url.begin(), latest->url.end());
        const std::wstring script =
            L"Invoke-WebRequest -UseBasicParsing -Headers @{'User-Agent'='Sunone-Medius'} -Uri '" + url + L"' -OutFile " + quote(archive) + L"; "
            L"$h=(Get-FileHash -Algorithm SHA256 " + quote(archive) + L").Hash.ToLower(); "
            L"if($h -ne '" + digest + L"'.ToLower()){exit 4}; "
            L"tar.exe -xzf " + quote(archive) + L" -C " + quote(staging);
        if (!runPowerShell(script))
        {
            status_.message = "Medius release download, SHA-256 verification, or extraction failed.";
            return false;
        }

        const auto candidate = findRecursive(staging, L"medius_capi.dll");
        uint32_t abi = 0;
        std::string version;
        if (candidate.empty() || !probeDll(candidate, abi, version))
        {
            status_.message = "Latest Medius DLL could not be probed.";
            return false;
        }

        status_.latestKnown = true;
        status_.latestAbi = abi;
        status_.latestSupported = abiSupported(abi);
        if (!status_.latestSupported)
        {
            status_.message = "Latest Medius v" + latest->version + " uses ABI " + std::to_string(abi) +
                "; this Sunone build supports ABI " + supportedAbiLabel() + ". Keeping the installed runtime.";
            return true;
        }

        const auto installed = dir / L"medius_capi.dll";
        uint32_t installedAbi = 0;
        std::string installedVersion;
        if (std::filesystem::exists(installed) && probeDll(installed, installedAbi, installedVersion) &&
            installedAbi == abi && installedVersion == version)
        {
            status_.message = "Latest Medius runtime is already installed.";
            return true;
        }

        const auto backup = dir / L"medius_capi.dll.previous";
        unloadUnlocked();
        std::filesystem::remove(backup, ec);
        ec.clear();
        if (std::filesystem::exists(installed))
        {
            std::filesystem::rename(installed, backup, ec);
            if (ec)
            {
                status_.message = "Failed to stage the previous Medius DLL for replacement.";
                return false;
            }
        }

        ec.clear();
        std::filesystem::copy_file(candidate, installed, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            if (std::filesystem::exists(backup) && !std::filesystem::exists(installed))
            {
                ec.clear();
                std::filesystem::rename(backup, installed, ec);
            }
            status_.message = "Failed to install the compatible Medius runtime; previous DLL retained.";
            return false;
        }

        uint32_t verifyAbi = 0;
        std::string verifyVersion;
        if (!probeDll(installed, verifyAbi, verifyVersion) || !abiSupported(verifyAbi))
        {
            std::filesystem::remove(installed, ec);
            ec.clear();
            if (std::filesystem::exists(backup))
                std::filesystem::rename(backup, installed, ec);
            status_.message = "Installed Medius runtime failed verification; rolled back.";
            return false;
        }

        status_.message = "Updated Medius runtime to v" + verifyVersion + " / ABI " + std::to_string(verifyAbi) + ".";
        return true;
    }

    bool loadInstalledUnlocked()
    {
        if (module_)
            return true;
        const auto dll = runtimeDir() / L"medius_capi.dll";
        if (!std::filesystem::exists(dll))
            return false;

        module_ = LoadLibraryW(dll.c_str());
        if (!module_)
            return false;
        abiVersion_ = reinterpret_cast<AbiVersionFn>(GetProcAddress(module_, "medius_abi_version"));
        versionString_ = reinterpret_cast<VersionStringFn>(GetProcAddress(module_, "medius_version_string"));
        deviceOpen_ = reinterpret_cast<DeviceOpenFn>(GetProcAddress(module_, "medius_device_open"));
        findMouseBox_ = reinterpret_cast<FindMouseBoxFn>(GetProcAddress(module_, "medius_device_find_mouse_box"));
        deviceFree_ = reinterpret_cast<DeviceFreeFn>(GetProcAddress(module_, "medius_device_free"));
        moveRelNow_ = reinterpret_cast<MoveRelNowFn>(GetProcAddress(module_, "medius_device_move_rel_now"));
        usageButton_ = reinterpret_cast<UsageButtonFn>(GetProcAddress(module_, "medius_usage_button"));
        press_ = reinterpret_cast<PressFn>(GetProcAddress(module_, "medius_device_press"));
        softRelease_ = reinterpret_cast<SoftReleaseFn>(GetProcAddress(module_, "medius_device_soft_release"));
        lastErrorMessage_ = reinterpret_cast<LastErrorMessageFn>(GetProcAddress(module_, "medius_last_error_message"));

        if (!abiVersion_ || !versionString_ || !deviceOpen_ || !findMouseBox_ || !deviceFree_ ||
            !moveRelNow_ || !usageButton_ || !press_ || !softRelease_)
        {
            unloadUnlocked();
            return false;
        }

        status_.runtimeAbi = abiVersion_();
        const char* version = versionString_();
        status_.runtimeVersion = version ? version : "unknown";
        if (!abiSupported(status_.runtimeAbi))
        {
            status_.message = "Installed Medius ABI " + std::to_string(status_.runtimeAbi) +
                " is unsupported; supported ABI: " + supportedAbiLabel() + ".";
            unloadUnlocked();
            return false;
        }
        status_.runtimeLoaded = true;
        status_.selectedPort = preferredPort_;
        status_.controlBaud = controlBaudRate();
        return true;
    }

    std::string lastErrorSuffixUnlocked() const
    {
        if (!lastErrorMessage_)
            return {};
        char buffer[512]{};
        const uintptr_t n = lastErrorMessage_(buffer, static_cast<uintptr_t>(sizeof(buffer)));
        if (n == 0 || buffer[0] == '\0')
            return {};
        return ": " + std::string(buffer);
    }

    void disconnectMouseUnlocked()
    {
        if (device_ && deviceFree_)
            deviceFree_(device_);
        device_ = nullptr;
        status_.deviceConnected = false;
    }

    void unloadUnlocked()
    {
        disconnectMouseUnlocked();
        if (module_)
            FreeLibrary(module_);
        module_ = nullptr;
        abiVersion_ = nullptr;
        versionString_ = nullptr;
        deviceOpen_ = nullptr;
        findMouseBox_ = nullptr;
        deviceFree_ = nullptr;
        moveRelNow_ = nullptr;
        usageButton_ = nullptr;
        press_ = nullptr;
        softRelease_ = nullptr;
        lastErrorMessage_ = nullptr;
        status_.runtimeLoaded = false;
        status_.runtimeVersion.clear();
        status_.runtimeAbi = 0;
    }

    void logStatusUnlocked() const
    {
        std::cout << "[Medius] " << status_.message << '\n';
        std::cout << "[Medius] Port: " << preferredPort_ << " | baud: " << controlBaudRate() << '\n';
        if (status_.runtimeLoaded)
            std::cout << "[Medius] Runtime: v" << status_.runtimeVersion << " / ABI " << status_.runtimeAbi << '\n';
        std::cout << "[Medius] Supported ABI: " << supportedAbiLabel() << '\n';
        if (status_.latestKnown)
        {
            std::cout << "[Medius] Latest: v" << status_.latestVersion << " / ABI " << status_.latestAbi
                      << " / " << (status_.latestSupported ? "supported" : "unsupported") << '\n';
        }
    }

    mutable std::mutex mutex_;
    Status status_;
    bool checkedThisRun_ = false;
    std::string preferredPort_ = "AUTO";

    HMODULE module_ = nullptr;
    MediusDevice* device_ = nullptr;
    AbiVersionFn abiVersion_ = nullptr;
    VersionStringFn versionString_ = nullptr;
    DeviceOpenFn deviceOpen_ = nullptr;
    FindMouseBoxFn findMouseBox_ = nullptr;
    DeviceFreeFn deviceFree_ = nullptr;
    MoveRelNowFn moveRelNow_ = nullptr;
    UsageButtonFn usageButton_ = nullptr;
    PressFn press_ = nullptr;
    SoftReleaseFn softRelease_ = nullptr;
    LastErrorMessageFn lastErrorMessage_ = nullptr;
};
