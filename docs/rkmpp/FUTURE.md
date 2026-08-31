# RKMPP 待完成工作

本文只记录尚未完成或尚未通过实机验收的工作。当前已经实现的能力、测量结论和已知限制统一记录在 [FEATURES.md](FEATURES.md)，用户配置记录在 [README.md](README.md)。

## HDMI RX 音频实机验收

`audio_source` 的代码、构建和 Source 选择测试已经完成，后续仍需：

1. 使用稳定的 PipeWire `node.name` 短时采集真实 HDMI PCM，确认无 XRUN、持续静音或零长度读取。
2. 在 Moonlight 中以 1080p60 串流至少五分钟，验证声音方向、连续性、一次客户端重连和 HDMI 音频源断开/恢复。
3. 使用同源闪光/短促音测量启动 lip-sync 和五分钟漂移，再决定是否需要时间戳传播、缓冲调整或自适应重采样。
4. 只有 PipeWire 路径被实测证明不满足延迟或稳定性要求时，才评估直接 ALSA 后端。

直接 ALSA 方案必须覆盖格式与声道转换、48 kHz 重采样、XRUN/ENODEV 恢复、可中断读取、设备独占、多会话和时钟漂移，不能只封装 `snd_pcm_open()` 与 `snd_pcm_readi()`。

## 音频设备选择 UI

- 在配置 API 中枚举可用 PulseAudio/PipeWire sink 和 source，并返回稳定名称与说明。
- 将音频配置的自由文本输入升级为可保留手工值的下拉或自动补全控件。
- 枚举失败不得改变现有配置，也不得阻塞配置页面。

## RKMPP 输出 buffer 有界池

1080p60 直通测量中，MPP output buffer acquire P50/P95 约为 0.63/1.11 ms，packet init P50/P95 约为 0.46/0.71 ms。该成本可测量，但尚未证明有界池能改善端到端尾延迟。

- 固定容量 slot 必须由 `encoded_packet_t` 持有到网络消费者完成，禁止复用仍在发送的 buffer。
- pool 满时采用显式、有界的 backpressure 或 freshness 优先丢帧，禁止动态无限扩容。
- 初版保持已经验证的 8 MiB slot，并覆盖高码率、4K 和 IDR 最大帧。
- 只有 Host to send P95 或 P99 明确改善，且无码流、内存、重连或网络回退时才保留。

## GOP 与按需 IDR

- 对比 GOP 60、300，以及明确定义后的 `gop=0 + Moonlight 按需 IDR`。
- 验证 H.264 SPS/PPS、HEVC VPS/SPS/PPS、真实 IDR 请求和受控丢包恢复。
- 仅当更长 GOP 降低网络尾延迟且不降低恢复可靠性时，才改变默认值。

## Slice low-delay 可行性

先在独立 RKMPP smoke 中验证 MPP 1.3.9 的 partition 行为、首 partition 相对完整访问单元的提前量，以及 H.264/HEVC 连续解码正确性。只有提前量 P50 至少 1 ms，并且能在首包发送前正确确定 GameStream 短帧头、总帧长度和 FEC 元数据时，才允许进入网络主路径。

## MppTask 异步 pipeline

仅当完成其他优化后，MPP submit/encode 仍是 Host to packet P95 的主要部分时评估。原型必须使用固定最大在途深度，并为每帧保持 V4L2/RGA holder、profile、PTS、IDR 状态和 output slot，覆盖满载、网络暂停、MPP timeout、source change 与 teardown。未达到至少 1 ms 的潜在收益时继续保留 simple API。

## RKMPP 与 Xbox 后续实机验证

- 对单 RGA 目标和四个 HDMI RX capture buffer 的 4K 路径执行短时与长时间实机验证，确认无 CMA 分配失败、queue starvation、FD/handle 增长或重连断流。
- 完成 Moonlight → Sunshine → Xbox 的按键、摇杆、扳机、普通/扳机振动、重连和故障矩阵。
- 定位 Xbox 会话建立及退出时 ROCK 5B+ 风扇短时升速的触发源；记录 CPU、线程、Remote Play 网络吞吐和系统调频，不把正常硬件调频误判为 Sunshine 忙等。

## NXBT 后续实机验证

- 使用 `scripts/nxbt-hardware-validation.sh` 完成 30 分钟服务、受限 socket 和连接稳定性检查。
- 在真实 Moonlight → Sunshine → Switch 链路逐项检查面键、方向键、双摇杆、L/R、ZL/ZR、PLUS/MINUS、HOME 和 CAPTURE。
- 分别验证 Moonlight 断开、Sunshine 停止、Bridge 重启、Switch 休眠/唤醒及蓝牙短时中断，确认发送中立状态、无粘键且可重新连接。
- 保存脱敏日志；不得记录蓝牙地址、账号信息或其他设备标识。

## Vulkan UI

通用 Vulkan UI、手柄组合键导航和 HDMI RX DMA-BUF 原位 RGA 合成仍是独立进行中的工作，详细门槛见 [VULKAN_UI_HDMI_RX_DMA_PLAN.md](VULKAN_UI_HDMI_RX_DMA_PLAN.md)。
