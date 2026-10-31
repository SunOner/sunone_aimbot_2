# Backend Selection and Checks

## Choosing DML vs CUDA

| Build | Requirements | Models |
| --- | --- | --- |
| DML | Windows GPU with DirectML support | `.onnx` |
| CUDA + TensorRT | Supported NVIDIA GPU, CUDA Toolkit and TensorRT | `.engine`; `.onnx` for engine generation |

See [build requirements](../build.md#2-requirements).

## DML Runs But Does Not Detect Anything

Check the Inference node's ONNX model, confidence threshold and device index, the class filters, and the capture preview. First-launch config example:

```ini
backend = DML
ai_model = your_model.onnx
confidence_threshold = 0.10
class_player = 0
class_head = 1
```

## CUDA Runs But GPU Usage Spikes

Preview, data collection and screenshots require CPU-readable frames and can change the capture path. Compare [capture diagnostics](capture-diagnostics.md) with these features enabled and disabled.

See [benchmark commands](recipes.md#provider-benchmark) for isolated provider timings.
