# Build From Source

Use the batch wrappers to configure dependencies, Visual Studio, Ninja and CMake.

## 1. Choose a Backend

| Backend | Use when | Model format |
|---|---|---|
| DML | You want the easiest Windows GPU path or non-NVIDIA support. | `.onnx` |
| CUDA + TensorRT | You have a supported NVIDIA GPU and want the fastest backend. | `.engine`, or `.onnx` for first-time engine generation |

## 2. Requirements

Required for both builds:

- Windows 10 or Windows 11 x64.
- Visual Studio or Visual Studio Build Tools with the Desktop development with C++ workload.
- Windows SDK with C++/WinRT headers.
- CMake.
- PowerShell.
- Internet access for first-time dependency setup, unless everything is already prepared locally.

Install Visual Studio, GPU drivers and NVIDIA SDKs separately.

### DML Requirements

- A GPU and driver that support DirectML.
- NuGet packages restored by the scripts:
  - `Microsoft.ML.OnnxRuntime.DirectML`
  - `Microsoft.AI.DirectML`
- The wrapper prepares DML OpenCV automatically.

### CUDA Requirements

- Supported NVIDIA GPU. GTX 10xx/Pascal and older are not supported by the current TensorRT path.
- CUDA Toolkit 13.1 or newer at runtime. The current dependency resolver prefers CUDA 13.2 + TensorRT 10.16 when available, with CUDA 13.1 + TensorRT 10.14 as a fallback profile.
- TensorRT 10 Windows binary SDK and OpenCV 4.13.0 with CUDA; see [dependency paths](#6-dependency-layout).

cuDNN is optional. Inference uses TensorRT; OpenCV DNN CUDA/cuDNN is disabled by default.

## 3. First-Time Build

From the repository root, run the interactive launcher:

```powershell
.\BUILDER.bat
```

Choose DML or CUDA and allow downloads unless dependencies are already installed.

Direct backend wrappers are also available:

```powershell
.\build_dml.bat
.\build_cuda.bat
```

Non-interactive examples:

```powershell
.\build_dml.bat -NonInteractive -OpenCvAlreadyBuilt $false -DownloadOrUpdateNeeded $true
.\build_cuda.bat -NonInteractive -OpenCvAlreadyBuilt $false -DownloadOrUpdateNeeded $true -OpenBrowserForDownloads
```

Useful CUDA options:

```powershell
.\build_cuda.bat -CudaArchBin 8.6
.\build_cuda.bat -CudaArchBin all
.\build_cuda.bat -SkipOpenCvBuild -OpenCvAlreadyBuilt $true
```

For a multi-architecture release with fewer OpenCV modules and lower build parallelism:

```powershell
.\build_cuda.bat -CudaArchBin all -OpenCvAlreadyBuilt $false -DownloadOrUpdateNeeded $true -OpenCvBuildList cudev,core,imgproc,imgcodecs,videoio,highgui,dnn,cudaarithm,cudaimgproc,cudawarping -OpenCvMaxCpuCount 2 -OpenCvCleanBuild
```

Use `-OpenCvCleanBuild` when changing the OpenCV module list.

## 4. Fast Local Rebuild

After a full wrapper build:

```powershell
.\build_no-options.bat -Backend DML
.\build_no-options.bat -Backend CUDA
```

This builds the existing app target without dependency setup or CMake reconfiguration. Rerun the full backend wrapper after dependency or build-system changes.

## 5. What the Wrappers Prepare

Full wrappers import `VsDevCmd.bat`, restore backend dependencies, prepare OpenCV and build `ai.exe` with Ninja Multi-Config. Dependency diagnostics are written to `build\dependency-resolution.json`.

## 6. Dependency Layout

| Dependency | Expected location |
|---|---|
| SimpleIni | `sunone_aimbot_2\modules\SimpleIni.h` |
| serial | `sunone_aimbot_2\modules\serial\` |
| TensorRT | `sunone_aimbot_2\modules\TensorRT-*\` |
| DML OpenCV | `sunone_aimbot_2\modules\opencv\build\dml\` |
| CUDA OpenCV | `sunone_aimbot_2\modules\opencv\build\cuda\install\` |
| DML NuGet packages | `packages\Microsoft.ML.OnnxRuntime.DirectML.*` and `packages\Microsoft.AI.DirectML.*` |

## 7. Output and Runtime Files

Default outputs:

```text
build\dml\Release\ai.exe
build\cuda\Release\ai.exe
```

Place detector models in `models` and depth models in `depth_models` beside `ai.exe`. Runtime paths resolve from that folder. `screenshots` is created when an image is saved.

When `AIMBOT_COPY_RUNTIME_DLLS` is enabled, CMake copies available runtime DLLs
next to `ai.exe`, including:

- OpenCV runtime DLL.
- `ghub_mouse.dll`, when present.
- `rzctl.dll`, when present.
- ONNX Runtime and DirectML DLLs for DML builds.
- TensorRT and optional cuDNN DLLs for CUDA builds.

## 8. Advanced CMake Overrides

Prefer wrapper arguments over manual CMake commands. Extra CMake cache variables
can be passed through the batch wrappers when needed:

```powershell
.\build_dml.bat -DAIMBOT_OPENCV_DML_ROOT=C:/path/to/opencv/dml
.\build_cuda.bat -DAIMBOT_TENSORRT_ROOT=C:/path/to/TensorRT-10.x -DAIMBOT_CUDNN_ROOT=C:/path/to/cudnn
```

Useful cache variables:

| Variable | Meaning |
|---|---|
| `AIMBOT_USE_CUDA` | `ON` for CUDA + TensorRT, `OFF` for DML. Set by wrappers. |
| `AIMBOT_COPY_RUNTIME_DLLS` | Copies runtime DLLs beside `ai.exe`. |
| `AIMBOT_OPENCV_DML_ROOT` | DML OpenCV build root. |
| `AIMBOT_OPENCV_CUDA_ROOT` | CUDA OpenCV install root. |
| `AIMBOT_TENSORRT_ROOT` | TensorRT SDK root. |
| `AIMBOT_CUDNN_ROOT` | Optional cuDNN root. |
| `AIMBOT_RZCTL_DLL` | Source path for `rzctl.dll`. |
| `AIMBOT_CPPWINRT_INCLUDE_DIR` | C++/WinRT include directory, if auto-detection fails. |

## 9. Exporting AI Models

Convert PyTorch `.pt` YOLO models to ONNX with Ultralytics:

```bash
pip install ultralytics -U

# TensorRT/CUDA source ONNX. The app can build a matching .engine from this.
yolo export model=your_model.pt format=onnx dynamic=true simplify=true

# DML source ONNX.
yolo export model=your_model.pt format=onnx simplify=true
```

Select the exported ONNX in the Inference node. CUDA builds generate a missing `.engine` beside the ONNX. Depth models belong in `depth_models` and use a separate Depth Inference node.

## 10. Validation

After source or build-system changes, build both supported backends through their wrappers when dependencies are available. Run tests from the repository root:

```powershell
ctest --test-dir build/dml -C Release --output-on-failure
ctest --test-dir build/cuda -C Release --output-on-failure
```

Run `ai.exe` from the backend's output folder for a smoke test. See [provider benchmarks](guides/recipes.md#provider-benchmark) and [editor preview/benchmarks](guides/node-graphs.md#preview-and-performance-checks) for isolated checks.

## 11. Troubleshooting

| Problem | What to check |
|---|---|
| `Build tree not found` from `build_no-options` | Run the full wrapper for that backend first. |
| CMake rejects the generator | Use the wrappers. The automated path expects Ninja or Ninja Multi-Config. |
| DML OpenCV missing | Run `.\build_dml.bat` with downloads enabled, or set `AIMBOT_OPENCV_DML_ROOT`. |
| CUDA dependency missing | Install CUDA Toolkit and extract the TensorRT Windows binary SDK under `sunone_aimbot_2\modules`, then rerun `.\build_cuda.bat`. |
| TensorRT archive extracted but not detected | Make sure you downloaded the Windows binary SDK archive, not TensorRT source code. The layout must contain `include\NvInfer.h`, `lib\nvinfer_10.lib`, and `bin\nvinfer_10.dll`. |
| OpenCV CUDA build fails | Check CUDA, MSVC, OpenCV 4.13.0, contrib sources, and CUDA architecture. Retry through `.\build_cuda.bat` so the helper uses the same settings. |
| Missing `rzctl.dll` | Keep `sunone_aimbot_2\rzctl.dll` in the repo or copy it beside `ai.exe`. |
| DML runs but detects nothing | Confirm the selected model is `.onnx`, lower `confidence_threshold`, and verify class IDs. |
| CUDA engine fails to load | Delete the stale `.engine`, select the `.onnx`, and let the CUDA backend rebuild the engine for the current TensorRT/CUDA stack. |
