#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "winrt_capture.h"
#include "sunone_aimbot_2.h"
#include "other_tools.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace
{
uint64_t ElapsedMicros(std::chrono::steady_clock::time_point start,
                       std::chrono::steady_clock::time_point end)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}
}

winrt::com_ptr<IGraphicsCaptureItemInterop> GetInteropFactory()
{
    static winrt::com_ptr<IGraphicsCaptureItemInterop> s_factory = [] {
        auto factory = winrt::get_activation_factory<
            GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        return factory.as<IGraphicsCaptureItemInterop>();
    }();
    return s_factory;
}

HWND WinRTScreenCapture::FindWindowByTitleSubstring(const std::string& title_substr)
{
    return FindCaptureWindowByTitle(title_substr);
}

WinRTScreenCapture::WinRTScreenCapture(int desiredWidth, int desiredHeight, const Options& options)
    : desiredRegionWidth(desiredWidth)
    , desiredRegionHeight(desiredHeight)
    , regionWidth(desiredWidth)
    , regionHeight(desiredHeight)
{
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    winrt::check_hresult(
        D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            0,
            createDeviceFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            d3dDevice.put(),
            nullptr,
            d3dContext.put()
        )
    );

    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    winrt::check_hresult(d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));
    winrt::com_ptr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(dxgiDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice1.put()))))
    {
        dxgiDevice1->SetMaximumFrameLatency(1);
    }

    device = CreateDirect3DDevice(dxgiDevice.get());
    if (!device)
    {
        throw std::runtime_error("[WinRTCapture] Failed to create IDirect3DDevice.");
    }

    if (options.target == "window")
    {
        if (options.windowTitle.empty())
        {
            throw std::runtime_error("[WinRTCapture] capture_target=window but capture_window_title is empty.");
        }
        HWND hwnd = FindWindowByTitleSubstring(options.windowTitle);
        if (!hwnd)
        {
            throw std::runtime_error("[WinRTCapture] Target window not found by title substring: " + options.windowTitle);
        }
        captureItem = CreateCaptureItemForWindow(hwnd);
    }
    else
    {
        HMONITOR hMonitor = GetMonitorHandleByIndex(options.monitorIndex);
        if(!options.monitorDevice.empty())
        {
            struct Search{const std::string* device;HMONITOR found=nullptr;}search{&options.monitorDevice};
            EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR handle,HDC,LPRECT,LPARAM data)->BOOL{auto& search=*reinterpret_cast<Search*>(data);MONITORINFOEXW info{};info.cbSize=sizeof(info);if(GetMonitorInfoW(handle,&info)&&WideToUtf8(info.szDevice)==*search.device){search.found=handle;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&search));
            hMonitor=search.found;
        }
        if (!hMonitor)
        {
            throw std::runtime_error("[WinRTCapture] Invalid monitor index in config.");
        }
        captureItem = CreateCaptureItemForMonitor(hMonitor);
    }
    if (!captureItem)
    {
        throw std::runtime_error("[WinRTCapture] CreateCaptureItemForMonitor failed.");
    }

    screenWidth = captureItem.Size().Width;
    screenHeight = captureItem.Size().Height;
    SetSourceDimensions(screenWidth, screenHeight);
    desiredRegionWidth = std::max(1, desiredRegionWidth);
    desiredRegionHeight = std::max(1, desiredRegionHeight);
    regionWidth = std::clamp(desiredRegionWidth, 1, std::max(1, screenWidth));
    regionHeight = std::clamp(desiredRegionHeight, 1, std::max(1, screenHeight));

    regionX = (screenWidth - regionWidth) / 2;
    regionY = (screenHeight - regionHeight) / 2;

    framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        device,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        3,
        captureItem.Size()
    );

    session = framePool.CreateCaptureSession(captureItem);
    if (auto session5 = session.try_as<IGraphicsCaptureSession5>())
    {
        session5.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{ 0 });
    }

    if (!options.captureBorders)
    {
        session.IsBorderRequired(false);
    }

    if (!options.captureCursor)
    {
        session.IsCursorCaptureEnabled(false);
    }

    if (!createStagingTextureCPU())
    {
        throw std::runtime_error("[WinRTCapture] createStagingTextureCPU() failed.");
    }
    

    session.StartCapture();
}

WinRTScreenCapture::~WinRTScreenCapture()
{
    if (session)
        session.Close();
    if (framePool)
        framePool.Close();

    stagingTextureCPU = nullptr;
    sharedTexture = nullptr;
    d3dContext = nullptr;
    d3dDevice = nullptr;
}

bool WinRTScreenCapture::createStagingTextureCPU()
{
    stagingTextureCPU = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = regionWidth;
    desc.Height = regionHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, stagingTextureCPU.put());
    if (FAILED(hr))
    {
        std::cerr << "[WinRTCapture] CreateTexture2D(staging) failed hr=" << std::hex << hr << std::endl;
        return false;
    }
    return true;
}

cv::Mat WinRTScreenCapture::GetNextFrameCpu()
{
    if (!framePool || !stagingTextureCPU)
        return cv::Mat();

    captureWinrtPollAttemptsTotal.fetch_add(1, std::memory_order_relaxed);

    Direct3D11CaptureFrame lastFrame = framePool.TryGetNextFrame();
    if (!lastFrame)
    {
        captureWinrtEmptyPollsTotal.fetch_add(1, std::memory_order_relaxed);
        return cv::Mat();
    }
    captureWinrtFramesDrainedTotal.fetch_add(1, std::memory_order_relaxed);

    const auto contentSize = lastFrame.ContentSize();
    if (contentSize.Width > 0 && contentSize.Height > 0 &&
        (contentSize.Width != screenWidth || contentSize.Height != screenHeight))
    {
        screenWidth = contentSize.Width;
        screenHeight = contentSize.Height;
        SetSourceDimensions(screenWidth, screenHeight);

        regionWidth = std::clamp(desiredRegionWidth, 1, std::max(1, screenWidth));
        regionHeight = std::clamp(desiredRegionHeight, 1, std::max(1, screenHeight));
        regionX = (screenWidth - regionWidth) / 2;
        regionY = (screenHeight - regionHeight) / 2;

        framePool.Recreate(
            device,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            3,
            contentSize
        );

        if (!createStagingTextureCPU())
            return cv::Mat();
    }

    auto frameSurface = lastFrame.Surface();
    auto frameTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frameSurface);
    if (!frameTexture)
        return cv::Mat();

    D3D11_BOX sourceRegion;
    sourceRegion.left = regionX;
    sourceRegion.top = regionY;
    sourceRegion.front = 0;
    sourceRegion.right = regionX + regionWidth;
    sourceRegion.bottom = regionY + regionHeight;
    sourceRegion.back = 1;

    const auto readbackStart = std::chrono::steady_clock::now();
    d3dContext->CopySubresourceRegion(
        stagingTextureCPU.get(),
        0,
        0, 0, 0,
        frameTexture.get(),
        0,
        &sourceRegion
    );

    D3D11_MAPPED_SUBRESOURCE mapped;
    const auto mapStart = std::chrono::steady_clock::now();
    HRESULT hrMap = d3dContext->Map(stagingTextureCPU.get(), 0, D3D11_MAP_READ, 0, &mapped);
    const auto mapEnd = std::chrono::steady_clock::now();
    captureWinrtMapMicrosTotal.fetch_add(ElapsedMicros(mapStart, mapEnd), std::memory_order_relaxed);
    if (FAILED(hrMap))
    {
        std::cerr << "[WinRTCapture] Map stagingTextureCPU failed hr=" << std::hex << hrMap << std::endl;
        if (hrMap == DXGI_ERROR_DEVICE_REMOVED || hrMap == DXGI_ERROR_DEVICE_RESET)
            capture_method_changed.store(true);
        return cv::Mat();
    }

    cv::Mat cpuFrame(regionHeight, regionWidth, CV_8UC4);
    const auto pixelCopyStart = std::chrono::steady_clock::now();
    for (int y = 0; y < regionHeight; y++)
    {
        unsigned char* dstRow = cpuFrame.ptr<unsigned char>(y);
        unsigned char* srcRow = (unsigned char*)mapped.pData + y * mapped.RowPitch;
        memcpy(dstRow, srcRow, regionWidth * 4);
    }
    const auto pixelCopyEnd = std::chrono::steady_clock::now();
    d3dContext->Unmap(stagingTextureCPU.get(), 0);
    const auto readbackEnd = std::chrono::steady_clock::now();

    captureWinrtPixelCopyMicrosTotal.fetch_add(
        ElapsedMicros(pixelCopyStart, pixelCopyEnd),
        std::memory_order_relaxed);
    captureWinrtReadbackMicrosTotal.fetch_add(
        ElapsedMicros(readbackStart, readbackEnd),
        std::memory_order_relaxed);
    captureWinrtFramesReturnedTotal.fetch_add(1, std::memory_order_relaxed);

    return cpuFrame;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem
WinRTScreenCapture::CreateCaptureItemForMonitor(HMONITOR hMonitor)
{
    auto interopFactory = GetInteropFactory();
    GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interopFactory->CreateForMonitor(
        hMonitor,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)
    );
    if (FAILED(hr))
    {
        throw std::runtime_error("[WinRTCapture] CreateForMonitor failed, HR=" + std::to_string(hr));
    }
    return item;
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem
WinRTScreenCapture::CreateCaptureItemForWindow(HWND hWnd)
{
    auto interopFactory = GetInteropFactory();
    GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interopFactory->CreateForWindow(
        hWnd,
        winrt::guid_of<GraphicsCaptureItem>(),
        winrt::put_abi(item)
    );
    if (FAILED(hr))
    {
        throw std::runtime_error("[WinRTCapture] CreateForWindow failed, HR=" + std::to_string(hr));
    }
    return item;
}

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
WinRTScreenCapture::CreateDirect3DDevice(IDXGIDevice* dxgiDevice)
{
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(
        CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, inspectable.put())
    );
    return inspectable.as<IDirect3DDevice>();
}

template<typename T>
winrt::com_ptr<T> WinRTScreenCapture::GetDXGIInterfaceFromObject(
    winrt::Windows::Foundation::IInspectable const& object)
{
    auto dxgiInterfaceAccess = object.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<T> result = nullptr;
    winrt::check_hresult(
        dxgiInterfaceAccess->GetInterface(winrt::guid_of<T>(), result.put_void())
    );
    return result;
}
