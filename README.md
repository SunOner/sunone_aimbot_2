<div align="center">

# Sunone Aimbot 2 (C++)

[![C++](https://img.shields.io/badge/C%2B%2B-17-blue)](https://github.com/SunOner/sunone_aimbot_2)
[![GitHub stars](https://img.shields.io/github/stars/SunOner/sunone_aimbot_2?color=ffb500)](https://github.com/SunOner/sunone_aimbot_2)
[![CUDA 13.1](https://img.shields.io/badge/CUDA-13.1-76B900?logo=nvidia&logoColor=white)](https://developer.nvidia.com/cuda-downloads)
[![Discord server](https://badgen.net/discord/online-members/37WVp6sNEh)](https://discord.gg/37WVp6sNEh)

  <p>
    <a href="https://github.com/SunOner/sunone_aimbot_2/releases" target="_blank">
      <img width="75%" src="https://github.com/SunOner/sunone_aimbot/blob/main/media/one.gif">
    </a>
  </p>
</div>

---

# Ready-to-Use Builds (Recommended)

**You do NOT need to compile anything if you just want to use the aimbot!**
Precompiled `.exe` builds are provided for both CUDA (NVIDIA only) and DirectML (all GPUs).

* **Download**
	* Pre-built binaries can be downloaded from the [Discord server](https://discord.gg/37WVp6sNEh) in the **pre-releases** channel.

---


### DirectML (DML) Build — Universal (All GPUs)

* **Works on:**

	* Any modern GPU (NVIDIA, AMD, Intel, including integrated graphics)
	* Windows 10/11 (x64)
	* No need for CUDA or special drivers
* **Recommended for:**

	* GTX 10xx/9xx/7xx series (old NVIDIA)
	* Any AMD Radeon or Intel Iris/Xe GPU
	* Laptops and office PCs with integrated graphics

### CUDA + TensorRT Build — High Performance (NVIDIA Only)

* **Works on:**

	* NVIDIA GPUs **GTX 1660, RTX 2000/3000/4000/5000**
	* **Requires:** latest NVIDIA graphics driver and CUDA 13.1
	* Windows 10/11 (x64)
* **Not supported:** GTX 10xx/Pascal and older (TensorRT limitation)
* **Includes CUDA+TensorRT runtime files in the precompiled build**
* TensorRT-10.14.1.48 is only needed separately when building from source.

Before using the CUDA build, update your NVIDIA graphics driver to the latest available version, then install [CUDA 13.1](https://developer.nvidia.com/cuda-13-1-0-download-archive). Do this before troubleshooting missing CUDA DLLs or launch-close issues.

**Both versions are ready-to-use: just download, unpack, run `ai.exe`.**

---

## How to Run (For Precompiled Builds)

1. **Download and unpack your chosen version (see links above).**
2. For CUDA build, first update your NVIDIA graphics driver to the latest version, then install [CUDA 13.1](https://developer.nvidia.com/cuda-13-1-0-download-archive) if not already installed.
3. For DML build, no extra software is needed.
4. **Run `ai.exe`.**
   On first launch, the model will be exported (may take up to 5 minutes).
5. Place your `.onnx` model in the `models` folder and select it in the overlay (HOME key).
6. All settings are available in the overlay.
   Use the HOME key to open/close overlay.

### Controls

* **Right Mouse Button:** Aim at the detected target
* **F2:** Exit
* **F3:** Pause aiming
* **F4:** Reload config
* **Home:** Open/close overlay and settings

---

# Build From Source (Advanced Users)

* [docs/build.md](docs/build.md)

---

## 📋 Documentation

* C++ config reference (`config.ini`):
  [docs/config.md](docs/config.md)
* Setup, FAQ, troubleshooting:
  [docs/guides.md](docs/guides.md)
* Source of truth in code:
  [sunone_aimbot_2/config/config.cpp](sunone_aimbot_2/config/config.cpp),
  [sunone_aimbot_2/config/config.h](sunone_aimbot_2/config/config.h)

---

## 📚 References & Useful Links

* [TensorRT Documentation](https://docs.nvidia.com/deeplearning/tensorrt/)
* [OpenCV Documentation](https://docs.opencv.org/4.x/d1/dfb/intro.html)
* [ImGui](https://github.com/ocornut/imgui)
* [CppWinRT](https://github.com/microsoft/cppwinrt)
* [GLFW](https://www.glfw.org/)
* [WindMouse](https://ben.land/post/2021/04/25/windmouse-human-mouse-movement/)
* [KMBOX](https://www.kmbox.top/)
* [MAKCU](https://makcu.com)
* [depth-anything-tensorrt](https://github.com/spacewalk01/depth-anything-tensorrt)

---

## 📄 Licenses

### OpenCV

* **License:** [Apache License 2.0](https://opencv.org/license.html)

### ImGui

* **License:** [MIT License](https://github.com/ocornut/imgui/blob/master/LICENSE)

---
## ❤️ Support the Project & Get Better AI Models

This project is actively developed thanks to the people who support it on [Boosty](https://boosty.to/sunone) and [Patreon](https://www.patreon.com/c/sunone).
**By supporting the project, you get access to improved and better-trained AI models!**

---

**Need help or want to contribute? Join our [Discord server](https://discord.gg/37WVp6sNEh) or open an issue on GitHub!**

## Star History

<a href="https://www.star-history.com/?repos=SunOner%2Fsunone_aimbot_2&amp;type=date&amp;legend=top-left">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=SunOner/sunone_aimbot_2&amp;type=date&amp;theme=dark&amp;legend=top-left" />
    <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=SunOner/sunone_aimbot_2&amp;type=date&amp;legend=top-left" />
    <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=SunOner/sunone_aimbot_2&amp;type=date&amp;legend=top-left" />
  </picture>
</a>
