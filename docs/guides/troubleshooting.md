# Troubleshooting Order

1. Confirm the build and model match: DML uses ONNX; CUDA uses TensorRT engines or generates them from ONNX.
2. Check the capture source shows the expected content.
3. Check graph outputs are enabled and receive the required data.
4. Confirm `input_method` matches an available device/runtime and its logs show a connection.
5. Inspect [capture counters](capture-diagnostics.md) for CPU/GPU path changes.
6. After code changes, rebuild through the matching [wrapper](build-workflow.md) and run a smoke test.

## Detection Works, But Aim Does Not Move

Check the input method's console log and test movement outside the game. WIN32 synthetic events may work on the desktop but be ignored by a game. See [input setup and checks](input-methods.md).

Other checks: [backends](backends.md), [UDP capture](udp-capture.md), [overlay](overlay.md).
