# 待完成工作与功能规划 (FUTURE)

本文档记录了基于 Rockchip MPP (rkmpp) 与 HDMI RX 的 Sunshine 串流方案中，后续需要探索和实现的各项功能与性能优化点。

## 1. 性能 Profile：端到端延迟拆解

**需求背景**：
为了优化整体串流体验，我们需要精确了解视频帧从进入 HDMI RX 到最终在串流终端（如 Moonlight）屏幕上显示，各个环节分别消耗了多少时间。

**探索与实现方案**：
*   **时间戳追踪 (Timestamping)**：
    *   整个流水线应统一采用高精度的单调时间 (`CLOCK_MONOTONIC`) 进行对齐。
    *   **HDMI RX 捕获阶段**：从 V4L2 缓冲区 (`v4l2_buffer`) 中提取内核驱动打上的时间戳，并与我们应用层的时钟基准对齐，记录为帧捕获起点时间。
    *   **RKMPP 编码阶段**：在调用 `mpi->encode_put_frame` 送入未压缩帧前记录时间，在 `mpi->encode_get_packet` 成功获取 H.264/HEVC 编码包后记录时间，两者之差即为硬件编码耗时。
    *   **传输与终端显示阶段**：网络传输耗时和客户端解码/渲染耗时通常由 Sunshine 与 Moonlight 之间的控制协议（通过计算 RTP 测算或客户端反馈）进行处理，我们需要确保前端时间戳准确传入 Sunshine 核心管道。
*   **数据集成**：
    *   研究 Sunshine 的核心代码，找到 Sunshine 追踪内部帧耗时的统计结构体（如 `plat_frame_stats` 等类似机制）。将我们测算出的 HDMI 捕获耗时和 MPP 编码耗时，正确赋值或累加到这些结构中，以便后端协议将其发送给客户端。

## 2. 性能 Profile：HUD/Overlay 显示

**需求背景**：
将上述各个环节的耗时直观地显示在用户的串流终端上，参考现有 Sunshine/Moonlight 组合为用户提供详细性能统计信息的做法。

**探索与实现方案**：
*   **方案 A：复用 Moonlight 原生性能叠加层 (推荐)**
    *   **原理**：Moonlight 客户端本身带有一个性能监控 OSD（通常通过快捷键 Ctrl+Alt+Shift+S 或手柄组合键呼出）。它显示的数据完全来源于 Sunshine 发送的统计元数据。
    *   **实现思路**：将我们测算出的“HDMI RX 捕获时间”映射为 Sunshine 发送统计中的 "Capture" (捕获) 耗时，将“RKMPP 编码时间”映射为 "Encode" (编码) 耗时。这样可以直接利用 Moonlight 现有的 UI 进行展示，不需要消耗 RK3588 的额外算力去渲染文字，用户体验最无缝。
*   **方案 B：视频流画面硬编码 (Burn-in OSD)**
    *   **原理**：在将获取到的视频帧送入 RKMPP 编码前，直接在视频图像（如 NV12/YUV 数据）上绘制文本。
    *   **实现思路**：可以利用 Rockchip 的 2D 图形加速器 (RGA) 的 OSD 功能进行高效叠加，或者在 CPU 中利用 Freetype 等字体库将信息渲染成 bitmap 并在 CPU 端合成。
    *   **缺点**：增加了额外的图像处理耗时和内存带宽开销；文字经过视频编码器压缩后边缘可能出现伪影。仅建议作为调试后备方案。

## 3. 音频处理：HDMI RX 声音流转

**需求背景**：
当前方案仅处理了视频流，未能捕获和串流从 HDMI RX 输入的音频数据。完整的串流体验必须包含音频。

**探索与实现方案**：
*   **硬件接口确认**：
    *   首先需要在 Rock 5B Plus 上确认 HDMI RX 输入音频暴露在系统中的节点。通常会被识别为一个 ALSA 捕获设备（例如 `hw:X,Y` 形式，对应 I2S 或 SPDIF RX 接口）。可以通过 `arecord -l` 命令进行确认。
*   **方案 A：通过 PipeWire / PulseAudio 路由 (推荐)**
    *   **原理**：Sunshine 在 Linux 上默认具有成熟的 PulseAudio 或 PipeWire 音频捕获后端。
    *   **实现思路**：在系统中配置音频服务，使用类似 `module-alsa-source` 的模块将 HDMI RX 的 ALSA 捕获节点加载为系统的虚拟麦克风或源 (Source)。然后配置 Sunshine 去读取这个源。
    *   **优势**：PipeWire/PulseAudio 可以自动处理 HDMI 音频与 Sunshine 要求的音频格式（通常是 48kHz, 16-bit, Stereo）之间的重采样 (Resampling) 与格式转换工作，开发工作量极小。
*   **方案 B：直接开发 ALSA 捕获后端**
    *   **原理**：如果在 RK3588 平台上运行完整的音频服务开销过大或者引入了不可接受的延迟，可以考虑绕过它们。
    *   **实现思路**：在 Sunshine (或当前插件中) 扩展音频捕获逻辑，直接打开 HDMI RX 对应的 ALSA PCM 设备（`snd_pcm_open` 等接口），循环读取音频数据。
    *   **难点**：如果源头（连接到 HDMI RX 的设备，如游戏主机）输出的是 44.1kHz 甚至多声道音频，而 Sunshine 协议强制需要 48kHz 立体声，则需要在我们的代码中自行引入 `libswresample` 等库进行软件重采样，增加了代码复杂度。
