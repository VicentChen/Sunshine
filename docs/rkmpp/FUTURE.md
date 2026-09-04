# RKMPP 待完成工作

本文只记录尚未完成或尚未通过实机验收的工作。当前已经实现的能力、测量结论和已知限制统一记录在 [FEATURES.md](FEATURES.md)，用户配置记录在 [README.md](README.md)。

## HDMI RX 音频实机验收

`audio_source` 的代码、构建和 Source 选择测试已经完成，后续仍需：

2026-09-04 现有日志只能证明配置的 HDMI Source 已打开且 Opus 成功初始化；尚无足够长的会话、音频样本或人工听感记录支持实机验收结项。

1. 使用稳定的 PipeWire `node.name` 短时采集真实 HDMI PCM，确认无 XRUN、持续静音或零长度读取。
2. 在 Moonlight 中以 1080p60 串流至少五分钟，验证声音方向、连续性、一次客户端重连和 HDMI 音频源断开/恢复。
3. 使用同源闪光/短促音测量启动 lip-sync 和五分钟漂移，再决定是否需要时间戳传播、缓冲调整或自适应重采样。
4. 只有 PipeWire 路径被实测证明不满足延迟或稳定性要求时，才评估直接 ALSA 后端。

直接 ALSA 方案必须覆盖格式与声道转换、48 kHz 重采样、XRUN/ENODEV 恢复、可中断读取、设备独占、多会话和时钟漂移，不能只封装 `snd_pcm_open()` 与 `snd_pcm_readi()`。

## GOP 与按需 IDR

- 对比 GOP 60、300，以及明确定义后的 `gop=0 + Moonlight 按需 IDR`。
- 验证 H.264 SPS/PPS、HEVC VPS/SPS/PPS、真实 IDR 请求和受控丢包恢复。
- 仅当更长 GOP 降低网络尾延迟且不降低恢复可靠性时，才改变默认值。

## Slice low-delay 可行性

先在独立 RKMPP smoke 中验证 MPP 1.3.9 的 partition 行为、首 partition 相对完整访问单元的提前量，以及 H.264/HEVC 连续解码正确性。只有提前量 P50 至少 1 ms，并且能在首包发送前正确确定 GameStream 短帧头、总帧长度和 FEC 元数据时，才允许进入网络主路径。

## MppTask 异步 pipeline

仅当完成其他优化后，MPP submit/encode 仍是 Host to packet P95 的主要部分时评估。原型必须使用固定最大在途深度，并为每帧保持 V4L2/RGA holder、profile、PTS、IDR 状态和 output slot，覆盖满载、网络暂停、MPP timeout、source change 与 teardown。未达到至少 1 ms 的潜在收益时继续保留 simple API。

## HDMI RX 帧收集时间观测

当前真实帧 Timeline 以驱动随 V4L2 buffer 返回的 `CLOCK_MONOTONIC` EOF 时间戳为零点，因此 `Host to send` 只覆盖 EOF 到最后一次发送，不包含 HDMI 从 SOF 到整帧 DMA 完成的收集时间。后续补充以下独立口径，不替换现有主机处理指标：

2026-09-04 代码核验确认尚未完成：`VIDIOC_QUERY_DV_TIMINGS` 目前只用于读取和恢复 HDMI 输入时序，尚未计算 frame period/estimated SOF，Profiler 也没有 `RX SCAN-IN` 或 `Capture-inclusive host` 指标。

- 基于 `VIDIOC_QUERY_DV_TIMINGS` 的 pixel clock、水平/垂直总时序计算名义 frame period，并可结合相邻 EOF 间隔的有界中位数校验；得到 `estimated SOF = EOF - frame period`。
- Timeline 新增明确标记为估算的 `RX SCAN-IN (estimated)`，范围为 estimated SOF 到 EOF；估算 span 应使用与实测 span 可区分的视觉样式。
- 新增 `Capture-inclusive host`（estimated SOF 到 final send）指标，同时保留现有 `RX driver age`、`Protocol host` 和 `Host to send` 的 EOF 起点语义，禁止重复计入 EOF 到 DQBUF。
- timing 缺失、无效、source change、隔行输入或刷新率发生变化时将估算标记为 missing，不得使用 Moonlight 请求帧率或固定 60 Hz 伪造输入收集时间。
- 若估算指标证明确有诊断价值，再评估在 `rk_hdmirx` 的帧开始/line-flag 与 DMA-idle 中断分别记录 SOF/EOF，并通过正式 V4L2 metadata 或受控接口暴露；在实测 SOF 可用前，不得把估算值标记为驱动实测。
- 1080p60、4K60 与 59.94 Hz 实机验证应确认 SOF→EOF 接近对应输入 frame period，并报告估算误差、抖动及对 Timeline/percentile 的影响。
