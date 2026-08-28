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

## 4. RKMPP 编码器 Web UI 集成

**需求背景**：
将特定于 Rockchip MPP (RKMPP) 的配置通过专门的编码器选项卡暴露给 Web UI，方便用户可视化操作。

**探索与实现方案**：
*   **前端**：创建一个新的 Vue 组件 `src_assets/common/assets/web/configs/tabs/encoders/RkmppEncoder.vue`。在 `ContainerEncoders.vue` 中注册该新选项卡，使其与 NVENC、VAAPI 等并列显示。
*   **后端**：确保任何新的 RKMPP 特定变量（例如速率控制模式、QP 限制、预设 profile）都能在 `src/config.cpp` 中被正确解析并暴露给 REST API，以此来取代或增强目前在 Advanced 选项卡中的开关（如 `rkmpp_profile`）。

## 5. 音频设备下拉菜单 UI

**需求背景**：
改善选择音频接收器 (sink) 和源 (source) 的用户体验，用交互式的下拉列表代替当前的自由文本输入框。

**探索与实现方案**：
*   **后端**：在 `src/confighttp.cpp` 中创建一个新的 API 端点（例如 `/api/audio-devices`），该端点能够查询系统音频服务器（PulseAudio, PipeWire, 或 ALSA）并返回可用设备名称和描述的 JSON 列表。
*   **前端**：修改 `src_assets/common/assets/web/configs/tabs/AudioVideo.vue`。将其中的 `audio_sink` 和 `audio_source` 的 `<input type="text">` 替换为 `<select>` 下拉框（或自动补全组合框），该下拉框会从新的 API 端点异步填充数据。这样用户就无需手动运行 `pactl list short sources` 这样的命令并复制粘贴设备字符串了。

## 6. RKMPP 输出 buffer 有界池（原性能优化阶段 4）

**已测量依据**：1080p60 直通 DMA-BUF 的 12 个连续 5 秒稳定窗口中，`MPP output buffer acquire` 的 P50/P95 约为 0.63/1.11 ms，`MPP output packet init` 的 P50/P95 约为 0.46/0.71 ms；input import cache 已稳定命中。因此输出 buffer acquire 是可量化的候选成本，但尚未证明它会改善端到端尾延迟。

**后续实现方案**：

*   仅在重新排期后实现固定容量的 output buffer pool；slot 由 `encoded_packet_t` 持有，直到网络消费者完成该帧才归还。
*   pool 满时采用显式、有界的 backpressure 或丢帧策略，禁止按帧动态扩容。
*   初版保持已验证的 8 MiB 单 slot 容量，并在高码率、高分辨率和 IDR 场景下确认不存在截断。

**快速验收条件**：

*   先记录 `mpp_buffer_get()`、`mpp_packet_init_with_buffer()`、释放路径和同时在途 packet 数。
*   仅运行一次短窗口 A/B；只有 `Host to send` P95 或 P99 有明确改善且无码流、内存或网络回退时才保留。
*   若实施，60 秒内 MPP 输出分配次数必须固定且不随帧数增长；关闭、重连和网络消费者暂停时不得死锁、泄漏或复用仍在发送的 slot。
