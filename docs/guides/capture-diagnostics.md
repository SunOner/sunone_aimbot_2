# Capture Diagnostics

Capture diagnostics example:

```text
[CaptureDiag] backend=TRT method=duplication_api capture_fps=60 use_cuda=true show_window=true prefer_gpu=false need_cpu_copy=true ...
```

| Field | Meaning |
|---|---|
| `backend` | Active inference backend, such as `TRT` or `DML`. |
| `method` | Capture method, usually `duplication_api`. |
| `requested_method` | Configured method. May be `duplication_api` while `method=winrt` after an unsupported-DDA fallback. |
| `use_cuda` | Whether CUDA capture is allowed by config/build. |
| `show_window` | Whether preview/debug window is open. |
| `prefer_gpu` | Whether the capture path is trying to stay GPU-side. |
| `need_cpu_copy` | Whether some active feature needs CPU-readable pixels. |
| `gpu_attempts` | Number of GPU capture attempts. If this is `0`, the GPU path is not being attempted. |
| `gpu_ok` | Successful GPU capture frames. |
| `gpu_timeout`, `gpu_not_ready`, `gpu_lost` | GPU capture failure categories. |
| `cuda_map_failed`, `cuda_array_failed`, `cuda_copy_failed` | CUDA interop/copy failure categories. |
| `cpu_path_frames` | Frames captured through the CPU path. |
| `trt_cpu_submitted` | Frames sent to TensorRT from CPU-prepared input. |
| `trt_gpu_submitted` | Frames submitted to TensorRT from GPU path. |

This pattern shows TensorRT receiving CPU-prepared frames:

```text
prefer_gpu=false need_cpu_copy=true gpu_attempts=0 cpu_path_frames=6000 trt_cpu_submitted=6000
```

Preview and frame-saving features can require this path. See [backends](backends.md), [overlay](overlay.md) and [data collection](data-collection.md).

## DDA startup fails with `0x887A0004`

`0x887A0004` is `DXGI_ERROR_UNSUPPORTED`: Desktop Duplication cannot capture the selected output in the current desktop mode or graphics configuration. Microsoft also documents this failure on hybrid graphics systems when DDA runs against the discrete GPU. It does not, by itself, establish which GPU owns a laptop's internal screen. See Microsoft's [DuplicateOutput return values](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-duplicateoutput) and [hybrid graphics explanation](https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/error-when-dda-capable-app-is-against-gpu) (the latter describes a Windows 8.1 scenario).

The DDA implementation creates its D3D11 device on the adapter that enumerated the selected DXGI output. A failed `DuplicateOutput` now logs the full HRESULT, monitor device name and adapter description so this can be checked against Windows **Settings → System → Display → Advanced display**.

When DDA initialization returns `DXGI_ERROR_UNSUPPORTED`, capture automatically tries WinRT for that same monitor, using its device name rather than assuming both APIs enumerate monitors in the same order. The requested method and inference backend remain configured as before: TensorRT/CUDA can continue processing CPU-readable WinRT frames. CUDA texture interop is not used for a WinRT capture. This path may have a different performance cost.

After the fallback is selected, retries and capture restarts keep using WinRT for that target instead of repeatedly creating an unsupported DDA session. Changing the capture method or monitor, or restarting the application, allows DDA to be tried again. Other DDA initialization errors retain their existing retry behavior. If WinRT initialization also fails, its error is logged and capture remains unavailable while it retries.

For older builds, select `winrt` in the Capture Source node and select the intended monitor. Legacy configurations use `capture_method = winrt` and `capture_target = monitor`. Changing `capture_use_cuda` only controls CUDA capture interop; it cannot fix a failure to create a DDA session.

For a user report, collect the application version, CUDA or DML build, complete startup log with HRESULT, whether the screen is internal or external, and an Advanced display screenshot showing the selected screen's GPU. A successful model initialization followed by this error points to capture initialization; the model startup message does not establish that frames are being captured.
