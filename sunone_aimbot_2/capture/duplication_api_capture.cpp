#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <iostream>

#ifdef USE_CUDA
#include <cuda_d3d11_interop.h>
#include <cuda_runtime_api.h>
#endif

#include "duplication_api_capture.h"
#include "sunone_aimbot_2.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

template <typename T>
inline void SafeRelease(T** ppInterface)
{
    if (*ppInterface)
    {
        (*ppInterface)->Release();
        *ppInterface = nullptr;
    }
}

struct FrameContext
{
    ID3D11Texture2D* texture = nullptr;
    bool hasAcquiredFrame = false;
    uint32_t accumulatedFrames = 0;
    bool hasLastPresentTime = false;
    bool hasLastMouseUpdateTime = false;
    bool pointerVisible = false;
    bool rectsCoalesced = false;
    uint32_t totalMetadataBufferSize = 0;
    uint32_t pointerShapeBufferSize = 0;
};

class DDAManager
{
public:
    DDAManager()
        : m_device(nullptr)
        , m_context(nullptr)
        , m_duplication(nullptr)
        , m_output1(nullptr)
        , m_frameAcquired(false)
    {
        ZeroMemory(&m_duplDesc, sizeof(m_duplDesc));
    }

    ~DDAManager()
    {
        Release();
    }

    HRESULT Initialize(
        int monitorIndex,
        int /*captureWidth*/,
        int /*captureHeight*/,
        int& outScreenWidth,
        int& outScreenHeight,
        ID3D11Device** outDevice,
        ID3D11DeviceContext** outContext,const std::string& monitorDevice)
    {
        HRESULT hr = S_OK;

        IDXGIFactory1* factory = nullptr;
        hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
        if (FAILED(hr))
        {
            std::cerr << "[DDA] CreateDXGIFactory1 failed hr=" << std::hex << hr << std::endl;
            return hr;
        }

        IDXGIAdapter1* adapter = nullptr;
        IDXGIOutput* output = nullptr;
        const int targetMonitorIndex = std::max(0, monitorIndex);
        std::wstring targetDevice;
        if(!monitorDevice.empty()){const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,monitorDevice.data(),static_cast<int>(monitorDevice.size()),nullptr,0);targetDevice.resize(std::max(0,count));if(count)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,monitorDevice.data(),static_cast<int>(monitorDevice.size()),targetDevice.data(),count);else{SafeRelease(&factory);return E_INVALIDARG;}}

        int currentMonitorIndex = 0;
        bool foundOutput = false;
        for (UINT adapterIdx = 0; ; ++adapterIdx)
        {
            IDXGIAdapter1* candidateAdapter = nullptr;
            hr = factory->EnumAdapters1(adapterIdx, &candidateAdapter);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(hr))
            {
                std::cerr << "[DDA] EnumAdapters1 failed hr=" << std::hex << hr << std::endl;
                SafeRelease(&factory);
                return hr;
            }

            for (UINT outputIdx = 0; ; ++outputIdx)
            {
                IDXGIOutput* candidateOutput = nullptr;
                hr = candidateAdapter->EnumOutputs(outputIdx, &candidateOutput);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(hr))
                {
                    std::cerr << "[DDA] EnumOutputs failed hr=" << std::hex << hr << std::endl;
                    SafeRelease(&candidateAdapter);
                    SafeRelease(&factory);
                    return hr;
                }

                DXGI_OUTPUT_DESC candidate{};const bool matchingDevice=!targetDevice.empty()&&SUCCEEDED(candidateOutput->GetDesc(&candidate))&&targetDevice==candidate.DeviceName;
                if (targetDevice.empty()?currentMonitorIndex==targetMonitorIndex:matchingDevice)
                {
                    adapter = candidateAdapter;
                    output = candidateOutput;
                    foundOutput = true;
                    break;
                }

                ++currentMonitorIndex;
                candidateOutput->Release();
            }

            if (foundOutput)
                break;

            candidateAdapter->Release();
        }

        if (!foundOutput || !adapter || !output)
        {
            std::cerr << "[DDA] No monitor with index " << targetMonitorIndex << std::endl;
            SafeRelease(&adapter);
            SafeRelease(&output);
            SafeRelease(&factory);
            return DXGI_ERROR_NOT_FOUND;
        }

        {
            D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
            UINT createDeviceFlags = 0;

            hr = D3D11CreateDevice(
                adapter,
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                createDeviceFlags,
                featureLevels,
                1,
                D3D11_SDK_VERSION,
                &m_device,
                nullptr,
                &m_context
            );
            if (FAILED(hr))
            {
                std::cerr << "[DDA] D3D11CreateDevice failed hr=" << std::hex << hr << std::endl;
                SafeRelease(&output);
                SafeRelease(&adapter);
                SafeRelease(&factory);
                return hr;
            }

            IDXGIDevice1* dxgiDevice = nullptr;
            if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice)))
            {
                dxgiDevice->SetMaximumFrameLatency(1);
                dxgiDevice->Release();
            }
        }

        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&m_output1);
        if (FAILED(hr))
        {
            std::cerr << "[DDA] QueryInterface(IDXGIOutput1) failed hr=" << std::hex << hr << std::endl;
            SafeRelease(&m_context);
            SafeRelease(&m_device);
            SafeRelease(&output);
            SafeRelease(&adapter);
            SafeRelease(&factory);
            return hr;
        }

        hr = m_output1->DuplicateOutput(m_device, &m_duplication);
        if (FAILED(hr))
        {
            std::cerr << "[DDA] DuplicateOutput failed hr=" << std::hex << hr << std::endl;
            SafeRelease(&m_output1);
            SafeRelease(&m_context);
            SafeRelease(&m_device);
            SafeRelease(&output);
            SafeRelease(&adapter);
            SafeRelease(&factory);
            return hr;
        }

        m_duplication->GetDesc(&m_duplDesc);

        DXGI_OUTPUT_DESC oDesc{};
        output->GetDesc(&oDesc);
        outScreenWidth = oDesc.DesktopCoordinates.right - oDesc.DesktopCoordinates.left;
        outScreenHeight = oDesc.DesktopCoordinates.bottom - oDesc.DesktopCoordinates.top;

        SafeRelease(&output);
        SafeRelease(&adapter);
        SafeRelease(&factory);

        if (outDevice)  *outDevice = m_device;
        if (outContext) *outContext = m_context;

        return hr;
    }

    HRESULT AcquireFrame(FrameContext& frameCtx, UINT timeout = 100)
    {
        frameCtx.texture = nullptr;
        frameCtx.hasAcquiredFrame = false;
        if (!m_duplication) return E_FAIL;

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        IDXGIResource* resource = nullptr;

        HRESULT hr = m_duplication->AcquireNextFrame(timeout, &frameInfo, &resource);
        if (FAILED(hr)) return hr;

        frameCtx.hasAcquiredFrame = true;
        frameCtx.accumulatedFrames = frameInfo.AccumulatedFrames;
        frameCtx.hasLastPresentTime = frameInfo.LastPresentTime.QuadPart != 0;
        frameCtx.hasLastMouseUpdateTime = frameInfo.LastMouseUpdateTime.QuadPart != 0;
        frameCtx.pointerVisible = frameInfo.PointerPosition.Visible != FALSE;
        frameCtx.rectsCoalesced = frameInfo.RectsCoalesced != FALSE;
        frameCtx.totalMetadataBufferSize = frameInfo.TotalMetadataBufferSize;
        frameCtx.pointerShapeBufferSize = frameInfo.PointerShapeBufferSize;
        m_frameAcquired = true;

        if (resource)
        {
            hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frameCtx.texture);
            resource->Release();
        }
        return hr;
    }

    void ReleaseFrame()
    {
        if (!m_duplication || !m_frameAcquired)
            return;

        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }

    void Release()
    {
        if (m_duplication)
        {
            ReleaseFrame();
            m_duplication->Release();
            m_duplication = nullptr;
        }
        SafeRelease(&m_output1);
        SafeRelease(&m_context);
        SafeRelease(&m_device);
    }

public:
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;
    IDXGIOutputDuplication* m_duplication;
    IDXGIOutput1* m_output1;
    DXGI_OUTDUPL_DESC m_duplDesc;
    bool m_frameAcquired;
};

DuplicationAPIScreenCapture::DuplicationAPIScreenCapture(int desiredWidth, int desiredHeight, int monitorIndex,const std::string& monitorDevice)
    : d3dDevice(nullptr)
    , d3dContext(nullptr)
    , deskDupl(nullptr)
    , output1(nullptr)
    , sharedTexture(nullptr)
    , stagingTextureCPU(nullptr)
    , screenWidth(0)
    , screenHeight(0)
    , regionWidth(desiredWidth)
    , regionHeight(desiredHeight)
{
    m_ddaManager = std::make_unique<DDAManager>();

    HRESULT hr = m_ddaManager->Initialize(
        monitorIndex,
        regionWidth,
        regionHeight,
        screenWidth,
        screenHeight,
        &d3dDevice,
        &d3dContext,monitorDevice
    );
    if (FAILED(hr))
    {
        std::cerr << "[DDA] DDAManager Initialize failed hr=0x" << std::hex << hr << std::endl;
        return;
    }

    regionWidth = std::clamp(regionWidth, 1, std::max(1, screenWidth));
    regionHeight = std::clamp(regionHeight, 1, std::max(1, screenHeight));
    SetSourceDimensions(screenWidth, screenHeight);

    initialized_ = createStagingTextureCPU();
    if (!initialized_)
        return;
#ifdef USE_CUDA
    createCudaInteropTexture();
#endif
}

DuplicationAPIScreenCapture::~DuplicationAPIScreenCapture()
{
#ifdef USE_CUDA
    releaseCudaInteropTexture();
#endif
    if (m_ddaManager)
    {
        m_ddaManager->Release();
        m_ddaManager.reset();
    }
    SafeRelease(&stagingTextureCPU);
    SafeRelease(&sharedTexture);

    d3dDevice = nullptr;
    d3dContext = nullptr;
    deskDupl = nullptr;
    output1 = nullptr;
}

cv::Mat DuplicationAPIScreenCapture::GetNextFrameCpu()
{
    if (!m_ddaManager || !m_ddaManager->m_duplication || !stagingTextureCPU)
        return cv::Mat();

    FrameContext frameCtx;
    HRESULT hr = m_ddaManager->AcquireFrame(frameCtx, 0);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        return cv::Mat();
    }
    else if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_INVALID_CALL)
    {
        capture_method_changed.store(true);
        return cv::Mat();
    }
    else if (FAILED(hr))
    {
        std::cerr << "[DuplicationAPIScreenCapture] AcquireNextFrame (CPU) failed hr=0x"
            << std::hex << hr << std::endl;
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        return cv::Mat();
    }

    if (!frameCtx.texture)
    {
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        return cv::Mat();
    }

    const int copyWidth = std::min(regionWidth, std::max(1, screenWidth));
    const int copyHeight = std::min(regionHeight, std::max(1, screenHeight));
    const int left = std::max(0, (screenWidth - copyWidth) / 2);
    const int top = std::max(0, (screenHeight - copyHeight) / 2);

    D3D11_BOX sourceRegion;
    sourceRegion.left = left;
    sourceRegion.top = top;
    sourceRegion.front = 0;
    sourceRegion.right = sourceRegion.left + copyWidth;
    sourceRegion.bottom = sourceRegion.top + copyHeight;
    sourceRegion.back = 1;

    d3dContext->CopySubresourceRegion(
        stagingTextureCPU,
        0,
        0, 0, 0,
        frameCtx.texture,
        0,
        &sourceRegion
    );

    m_ddaManager->ReleaseFrame();
    frameCtx.texture->Release();

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hrMap = d3dContext->Map(stagingTextureCPU, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hrMap))
    {
        std::cerr << "[DDA] Map stagingTextureCPU failed hr=" << std::hex << hrMap << std::endl;
        if (hrMap == DXGI_ERROR_DEVICE_REMOVED || hrMap == DXGI_ERROR_DEVICE_RESET)
            capture_method_changed.store(true);
        return cv::Mat();
    }

    cv::Mat cpuFrame(regionHeight, regionWidth, CV_8UC4);
    for (int y = 0; y < regionHeight; y++)
    {
        unsigned char* dstRow = cpuFrame.ptr<unsigned char>(y);
        unsigned char* srcRow = (unsigned char*)mapped.pData + y * mapped.RowPitch;
        memcpy(dstRow, srcRow, regionWidth * 4);
    }

    d3dContext->Unmap(stagingTextureCPU, 0);
    return cpuFrame;
}

#ifdef USE_CUDA
static void SetGpuCaptureStatus(GpuCaptureStatus* status, GpuCaptureStatus value)
{
    if (status)
        *status = value;
}

static void SetDdaCaptureFrameInfo(DdaCaptureFrameInfo* outInfo, const FrameContext& frameCtx)
{
    if (!outInfo)
        return;

    outInfo->accumulatedFrames = frameCtx.accumulatedFrames;
    outInfo->hasLastPresentTime = frameCtx.hasLastPresentTime;
    outInfo->hasLastMouseUpdateTime = frameCtx.hasLastMouseUpdateTime;
    outInfo->pointerVisible = frameCtx.pointerVisible;
    outInfo->rectsCoalesced = frameCtx.rectsCoalesced;
    outInfo->totalMetadataBufferSize = frameCtx.totalMetadataBufferSize;
    outInfo->pointerShapeBufferSize = frameCtx.pointerShapeBufferSize;
}

static void ResetDdaCaptureFrameInfo(DdaCaptureFrameInfo* outInfo)
{
    if (outInfo)
        *outInfo = DdaCaptureFrameInfo{};
}

bool DuplicationAPIScreenCapture::GetNextFrameGpu(
    cv::cuda::GpuMat& gpuFrameBgra,
    GpuCaptureStatus* status,
    uint32_t* accumulatedFrames,
    DdaCaptureFrameInfo* frameInfo)
{
    if (!m_ddaManager || !m_ddaManager->m_duplication || !interopTextureGPU || !cudaInteropResource || !cudaInteropReady)
    {
        SetGpuCaptureStatus(status, GpuCaptureStatus::NotReady);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }

    FrameContext frameCtx;
    HRESULT hr = m_ddaManager->AcquireFrame(frameCtx, 0);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        SetGpuCaptureStatus(status, GpuCaptureStatus::Timeout);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }
    else if (hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_INVALID_CALL)
    {
        capture_method_changed.store(true);
        SetGpuCaptureStatus(status, GpuCaptureStatus::DeviceLost);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }
    else if (FAILED(hr))
    {
        std::cerr << "[DuplicationAPIScreenCapture] AcquireNextFrame (GPU) failed hr=0x"
            << std::hex << hr << std::endl;
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        SetGpuCaptureStatus(status, GpuCaptureStatus::AcquireFailed);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        ResetDdaCaptureFrameInfo(frameInfo);
        return false;
    }

    if (!frameCtx.texture)
    {
        if (frameCtx.hasAcquiredFrame)
            m_ddaManager->ReleaseFrame();
        SetGpuCaptureStatus(status, GpuCaptureStatus::MissingTexture);
        if (accumulatedFrames)
            *accumulatedFrames = 0;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    const int copyWidth = std::min(regionWidth, std::max(1, screenWidth));
    const int copyHeight = std::min(regionHeight, std::max(1, screenHeight));
    const int left = std::max(0, (screenWidth - copyWidth) / 2);
    const int top = std::max(0, (screenHeight - copyHeight) / 2);

    D3D11_BOX sourceRegion;
    sourceRegion.left = left;
    sourceRegion.top = top;
    sourceRegion.front = 0;
    sourceRegion.right = sourceRegion.left + copyWidth;
    sourceRegion.bottom = sourceRegion.top + copyHeight;
    sourceRegion.back = 1;

    d3dContext->CopySubresourceRegion(
        interopTextureGPU,
        0,
        0, 0, 0,
        frameCtx.texture,
        0,
        &sourceRegion
    );

    m_ddaManager->ReleaseFrame();
    frameCtx.texture->Release();

    cudaError_t cuErr = cudaGraphicsMapResources(1, &cudaInteropResource, 0);
    if (cuErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsMapResources failed: " << cudaGetErrorString(cuErr) << std::endl;
        cudaInteropReady = false;
        SetGpuCaptureStatus(status, GpuCaptureStatus::CudaMapFailed);
        if (accumulatedFrames)
            *accumulatedFrames = frameCtx.accumulatedFrames;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    cudaArray_t mappedArray = nullptr;
    cuErr = cudaGraphicsSubResourceGetMappedArray(&mappedArray, cudaInteropResource, 0, 0);
    if (cuErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsSubResourceGetMappedArray failed: " << cudaGetErrorString(cuErr) << std::endl;
        cudaGraphicsUnmapResources(1, &cudaInteropResource, 0);
        cudaInteropReady = false;
        SetGpuCaptureStatus(status, GpuCaptureStatus::CudaArrayFailed);
        if (accumulatedFrames)
            *accumulatedFrames = frameCtx.accumulatedFrames;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    gpuFrameBgra.create(regionHeight, regionWidth, CV_8UC4);

    cuErr = cudaMemcpy2DFromArray(
        gpuFrameBgra.ptr<unsigned char>(),
        gpuFrameBgra.step,
        mappedArray,
        0, 0,
        static_cast<size_t>(regionWidth) * 4,
        static_cast<size_t>(regionHeight),
        cudaMemcpyDeviceToDevice
    );

    cudaError_t unmapErr = cudaGraphicsUnmapResources(1, &cudaInteropResource, 0);
    if (unmapErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsUnmapResources failed: " << cudaGetErrorString(unmapErr) << std::endl;
        cudaInteropReady = false;
    }

    if (cuErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaMemcpy2DFromArray failed: " << cudaGetErrorString(cuErr) << std::endl;
        cudaInteropReady = false;
        SetGpuCaptureStatus(status, GpuCaptureStatus::CudaCopyFailed);
        if (accumulatedFrames)
            *accumulatedFrames = frameCtx.accumulatedFrames;
        SetDdaCaptureFrameInfo(frameInfo, frameCtx);
        return false;
    }

    SetGpuCaptureStatus(status, GpuCaptureStatus::Captured);
    if (accumulatedFrames)
        *accumulatedFrames = frameCtx.accumulatedFrames;
    SetDdaCaptureFrameInfo(frameInfo, frameCtx);
    return true;
}

bool DuplicationAPIScreenCapture::createCudaInteropTexture()
{
    if (!d3dDevice)
        return false;

    releaseCudaInteropTexture();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = regionWidth;
    desc.Height = regionHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &interopTextureGPU);
    if (FAILED(hr))
    {
        std::cerr << "[DDA] CreateTexture2D(interop) failed hr=" << std::hex << hr << std::endl;
        return false;
    }

    cudaError_t cuErr = cudaGraphicsD3D11RegisterResource(
        &cudaInteropResource,
        interopTextureGPU,
        cudaGraphicsRegisterFlagsNone
    );

    if (cuErr != cudaSuccess)
    {
        std::cerr << "[DDA] cudaGraphicsD3D11RegisterResource failed: "
            << cudaGetErrorString(cuErr) << std::endl;
        SafeRelease(&interopTextureGPU);
        cudaInteropResource = nullptr;
        cudaInteropReady = false;
        return false;
    }

    cudaInteropReady = true;
    return true;
}

void DuplicationAPIScreenCapture::releaseCudaInteropTexture()
{
    if (cudaInteropResource)
    {
        cudaError_t cuErr = cudaGraphicsUnregisterResource(cudaInteropResource);
        if (cuErr != cudaSuccess)
        {
            std::cerr << "[DDA] cudaGraphicsUnregisterResource failed: "
                << cudaGetErrorString(cuErr) << std::endl;
        }
        cudaInteropResource = nullptr;
    }

    SafeRelease(&interopTextureGPU);
    cudaInteropReady = false;
}
#endif

bool DuplicationAPIScreenCapture::createStagingTextureCPU()
{
    if (!d3dDevice) return false;

    SafeRelease(&stagingTextureCPU);

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
    desc.MiscFlags = 0;

    HRESULT hr = d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTextureCPU);
    if (FAILED(hr))
    {
        std::cerr << "[DDA] CreateTexture2D(staging) failed hr=" << std::hex << hr << std::endl;
        return false;
    }
    return true;
}
