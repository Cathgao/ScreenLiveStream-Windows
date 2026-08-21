# ScreenLiveStream Windows (QuestCast VR for Windows)

> 在局域网内，实现 Windows 电脑与 Android 手机 / Meta Quest 头显之间的超低延迟硬件加速投屏（支持 Windows 发送端与接收端合一）。
>



## 🔗 相关项目

- **Android / Meta Quest 客户端**：[ScreenLiveStream](https://github.com/Cathgao/ScreenLiveStream) (QuestCast VR) — 基于 Kotlin + Jetpack Compose 与 MediaCodec 开发的 Android / Meta Quest 投屏与接收客户端，支持硬件编解码、局域网自动发现及本地录制。
---



## 🌟 核心特性与架构

- **发送端与接收端双向支持 (Sender & Receiver)**：
  - **发送端 (Sender)**：将 Windows 屏幕/游戏/特定窗口 + 系统音频，超低延迟投送到 Android 手机、平板或 Quest。
  - **接收端 (Receiver)**：接收 Android / Quest 发送的 H.264/H.265 投屏画面并以 Direct3D 11 硬件加速窗口播放。
- **Windows.Graphics.Capture (WGC) 现代采集**：
  - 支持单窗口 / 全屏显示器精准抓取。
  - 显存零拷贝（Zero-Copy），Direct3D 11 纹理直接送入硬件编码器。
  - 自动消除黄色边框（Windows 11 22H2+），支持光标捕获开关。
- **WASAPI Loopback 系统音频采集与低延迟播放**：
  - 无侵入式抓取系统 48kHz 双声道 PCM 声音，带静音保活机制。
- **全链路硬件加速编解码 (Windows Media Foundation Hardware MFT)**：
  - 支持 NVIDIA (NVENC/NVDEC)、AMD (AMF)、Intel (QSV) 硬件加速。
  - 支持 **H.265 / HEVC** (默认) 与 **H.264 / AVC**，超低延迟无 B 帧预设。
  - AAC 硬件/低延迟音频编解码。
- **极速渲染 (Direct3D 11 FLIP SwapChain)**：
  - 采用 `DXGI_SWAP_EFFECT_FLIP_DISCARD` 与 `Present(0, 0)`，杜绝垂直同步画面积压。
- **100% 协议互通**：
  - 与 Android 端 `ScreenLiveStream` 自定义分包协议（22 字节 Header、UDP 9998 局域网自动发现、UDP / TCP 传输、RTT Probe 测速）无缝兼容。

---

## 🛠️ 构建与编译

### 环境要求
- **操作系统**：Windows 10 (1803+) 或 Windows 11
- **编译器**：Visual Studio 2022 / 2026 (MSVC C++20 支持)
- **SDK**：Windows 10/11 SDK (`10.0.26100.0` 或更高)
- **CMake**：3.20 或更高版本

### 命令行编译

```powershell
# 1. 进入项目根目录
cd r:\ScreenLiveStream-Windows

# 2. 生成 Visual Studio 解决方案或 Ninja 构建文件
cmake -B build -G "Visual Studio 17 2022" -A x64

# 3. 编译 Release 版本
cmake --build build --config Release
```

编译生成的产物位于 `build/Release/ScreenLiveStreamWindows.exe`。

---

## 📖 使用指南

### 1. 作为发送端 (将电脑画面投到手机/Quest)
1. 打开 `ScreenLiveStreamWindows.exe`，选择顶部 **电脑发送端 (Sender)**。
2. 在 **采集目标** 下拉框中选择想要投屏的显示器或特定应用窗口。
3. 从 **局域网设备** 列表中选择你的 Android/Quest 接收端（或手动填写 IP 和端口 `8888`）。
4. 选择编码器（推荐 H.265）、码率、帧率与协议（推荐 UDP）。
5. 点击 **🚀 启动画面投屏** 开始推流。

### 2. 作为接收端 (在电脑上接收手机/Quest 投屏)
1. 选择顶部 **电脑接收端 (Receiver)**。
2. 确认监听端口（默认 `8888`）与传输协议（UDP 或 TCP）。
3. 点击 **📺 启动接收端**。
4. 打开手机/Quest 上的 `ScreenLiveStream`，在发送端搜索列表中即可看到 `Windows-PC`，点击启动投屏即可在电脑独立窗口中享受低延迟高画质播放。
