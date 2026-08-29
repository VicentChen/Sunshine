# HDMI RX 音频串流实施计划

## 1. 目标与边界

目标是把 ROCK 5B+ HDMI RX 的音频输入接入 Sunshine 现有的浮点 PCM → Opus → Moonlight 管线，默认行为保持不变，并为后续音视频同步优化建立可验证基线。

本计划不修改 HDMI EDID、不重启当前 Sunshine、不安装或持久化 PipeWire 系统配置。硬件采集、服务切换和 Moonlight 现场测试分别授权后执行。

## 2. 当前实机基线（已完成）

- ALSA capture：`hw:3,0`，card `rockchiphdmiin`，双声道。
- PipeWire Source：`alsa_input.platform-hdmiin-sound.HDMI__hw_rockchiphdmiin__source.8`。
- PipeWire 枚举格式：S16LE、S24_32LE、S32LE，32–192 kHz，FL/FR。
- Sunshine Linux 后端通过 libpulse 连接 `pipewire-pulse`，默认采集所选 Sink 的 monitor source。
- Sunshine Opus 输入为 48 kHz float PCM，客户端可协商 2/6/8 声道；本功能第一阶段以双声道验收。

## 3. 分步实施

### 步骤 1：显式 Source 选择（代码与构建已完成）

- 新增 Linux 配置 `audio_source`，空值保持现有 `audio_sink → monitor source` 路径。
- 非空时把 Source 名称直接传给 PulseAudio capture API，不引入直接 ALSA 后端。
- 更新配置文档、Web UI、英文 locale 和配置一致性数据。
- 为 Source 优先级和 monitor 回退增加单元测试。

验收结果：

- `audio_source` 为空时保留 Sink monitor 回退，非空时优先使用显式 Source。
- `LinuxAudioSourceSelection` 与 `ConfigConsistencyTest` 共 7 项定向测试通过。
- `test_sunshine` 和正式 `sunshine` 目标均构建成功。
- 尚未打开 HDMI 音频设备，因此真实 PCM、Moonlight 声音和 A/V 同步仍属于步骤 2–4。

### 步骤 2：PipeWire/HDMI RX 短时采集验证（待授权）

- 使用稳定的 PipeWire `node.name`，不依赖会变化的数字对象 ID。
- 先用 `pw-record` 或等价工具做短时 PCM/WAV 采样，检查真实信号、声道、采样率和静音状态。
- 用测试配置启动独立 Sunshine 或安排短暂停机，避免覆盖当前实例。

验收：连续采集无 XRUN、无零长度读、无持续静音，48 kHz 双声道转换稳定。

### 步骤 3：Moonlight 现场验收（待客户端窗口）

- 以 1080p60 和常用视频 codec 串流 5 分钟。
- 检查声音方向、连续性、Opus 错误、CPU、PipeWire quantum/延迟和一次客户端重连。
- 做一次 HDMI 音频源断开/恢复；不执行 50 次重连或两小时耐久测试。

验收：Moonlight 持续有声，无明显爆音、重复、积压或静音；关闭 `audio_source` 后恢复原有 Sink monitor 行为。

### 步骤 4：A/V 同步测量与恢复策略（待步骤 3）

- 使用同源闪光/短促音事件测量启动 lip-sync 和 5 分钟漂移。
- 记录 HDMI 视频 V4L2 单调时间、音频图时钟和网络包节奏，区分固定偏移与时钟漂移。
- 定义可接受阈值后，再决定是否需要时间戳传播、缓冲调整或自适应重采样。

### 步骤 5：直接 ALSA 后端（条件项）

仅当实测证明 PipeWire 路径的延迟或稳定性不满足要求时启动。届时必须覆盖格式/声道转换、重采样、XRUN/ENODEV 恢复、可中断读取、设备独占、多会话和时钟漂移；不能只实现 `snd_pcm_open`/`snd_pcm_readi`。

## 4. 当前退出条件

步骤 1 已完成。未获单独授权前，不打开 HDMI 音频 PCM 进行采集、不写用户级 PipeWire 配置、不替换或重启运行中的 Sunshine。
