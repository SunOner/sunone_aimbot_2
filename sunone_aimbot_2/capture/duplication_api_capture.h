#ifndef DUPLICATION_API_CAPTURE_H
#define DUPLICATION_API_CAPTURE_H

#include <d3d11.h>
#include <dxgi1_2.h>
#include <opencv2/opencv.hpp>
#include <memory>
#include <cstdint>
#include <string>

#ifdef USE_CUDA
#include <cuda_runtime_api.h>
#include <opencv2/core/cuda.hpp>
struct cudaGraphicsResource;

struct DdaCaptureFrameInfo
{
    uint32_t accumulatedFrames = 0;
    bool hasLastPresentTime = false;
    bool hasLastMouseUpdateTime = false;
    bool pointerVisible = false;
    bool rectsCoalesced = false;
    uint32_t totalMetadataBufferSize = 0;
    uint32_t pointerShapeBufferSize = 0;
};

enum class GpuCaptureStatus
{
    Captured,
    NotReady,
    Timeout,
    NoPresent,
    DeviceLost,
    AcquireFailed,
    MissingTexture,
    CudaMapFailed,
    CudaArrayFailed,
    CudaCopyFailed
};
#endif

#include "capture.h"

class DDAManager;

class DuplicationAPIScreenCapture : public IScreenCapture
{
public:
    DuplicationAPIScreenCapture(int desiredWidth, int desiredHeight, int monitorIndex,const std::string& monitorDevice={});
    ~DuplicationAPIScreenCapture();

    cv::Mat GetNextFrameCpu() override;
    bool isInitialized() const { return initialized_; }
    HRESULT initializationResult() const { return initializationResult_; }
    const std::string& monitorDevice() const { return monitorDevice_; }
#ifdef USE_CUDA
    bool GetNextFrameGpu(
        cv::cuda::GpuMat& gpuFrameBgra,
        GpuCaptureStatus* status = nullptr,
        uint32_t* accumulatedFrames = nullptr,
        DdaCaptureFrameInfo* frameInfo = nullptr);
#endif

private:
    std::unique_ptr<DDAManager> m_ddaManager;

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    IDXGIOutputDuplication* deskDupl = nullptr;
    IDXGIOutput1* output1 = nullptr;

    ID3D11Texture2D* sharedTexture = nullptr;

    ID3D11Texture2D* stagingTextureCPU = nullptr;
#ifdef USE_CUDA
    ID3D11Texture2D* interopTextureGPU = nullptr;
    cudaGraphicsResource* cudaInteropResource = nullptr;
    bool cudaInteropReady = false;
#endif

    int screenWidth = 0;
    int screenHeight = 0;
    int regionWidth = 0;
    int regionHeight = 0;
    bool initialized_ = false;
    HRESULT initializationResult_ = E_FAIL;
    std::string monitorDevice_;

    bool createStagingTextureCPU();
#ifdef USE_CUDA
    bool createCudaInteropTexture();
    void releaseCudaInteropTexture();
#endif
};

#endif // DUPLICATION_API_CAPTURE_H
