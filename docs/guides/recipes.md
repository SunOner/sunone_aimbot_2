# Common Recipes

## Lowest Setup Burden

Config examples seed new graphs; edit node inputs in an existing workspace.

```ini
backend = DML
ai_model = your_model.onnx
input_method = WIN32
circle_fov_enabled = true
```

## CUDA/TensorRT Performance Test

```ini
backend = TRT
ai_model = your_model.engine
capture_method = duplication_api
capture_use_cuda = true
show_window = false
collect_data_while_playing = false
```

Then check `[CaptureDiag]` output.

## Provider Benchmark

Run from the backend's output folder; capture and input devices stay inactive:

```powershell
.\ai.exe --benchmark-providers
.\ai.exe --benchmark-providers cpu,dml-gpu --bench-runs 200 --bench-warmup 20
.\ai.exe --benchmark-providers dml-gpu,dml-cpu --bench-model models\your_model.onnx
.\ai.exe --benchmark-providers cuda --bench-cuda-model models\your_model.engine
```

DML supports `cpu`, `dml-gpu` and `dml-cpu`; CUDA supports `cuda` through TensorRT. Timings are reported in seconds. Results append to `benchmark_results\provider_benchmark.csv` or `provider_benchmark_cuda.csv`. Use `--bench-no-save` for console output only, or `--bench-results <path>` for a custom CSV.

## Razer Control Test

```ini
input_method = RAZER
```

Place `rzctl.dll` beside `ai.exe` or in `sunone_aimbot_2`.

## Teensy Control Test

```ini
input_method = TEENSY41_HID
teensy_hid_serial = AUTO
teensy_hid_vid_filter = AUTO
teensy_hid_pid_filter = AUTO
```

Start broad with `AUTO`, then narrow filters after the device is confirmed.

Related docs:

- [Capture diagnostics](capture-diagnostics.md)
- [Input methods](input-methods.md)
- [Build workflow](build-workflow.md)
