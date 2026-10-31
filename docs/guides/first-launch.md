# First Launch Checklist

1. Choose DML for DirectML/ONNX, or CUDA for a supported NVIDIA GPU with CUDA/TensorRT installed.
2. Put detector models in `models` beside `ai.exe`. DML uses `.onnx`; CUDA loads `.engine` or generates it from ONNX.
3. Start the app to create `config.ini` and the default graphs.
4. Open the menu with Home and edit node settings; changes save automatically.
5. Select the `input_method` for your device/runtime and check its connection.

See [backends](backends.md), [node graphs](node-graphs.md), [configuration](../config.md) and [building](../build.md).
