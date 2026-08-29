# HDMI RX 到 Moonlight 延迟观测实施计划

## 1. 目标与边界

目标是在不修改 Moonlight 客户端的前提下，得到 HDMI 输入经过 Sunshine 主机链路各阶段的耗时，并把主机侧统计直接烧录到视频中，使所有 Moonlight 客户端都能看到。

最终画面由两组数据组成：

- Sunshine HUD：HDMI RX、采集队列、RGA、RKMPP 编码、编码后队列、分包及发送提交的 P50/P95/P99。
- Moonlight 原生统计：网络延迟与客户端解码延迟。

两者可用于定位主要瓶颈，但不能直接等同于严格的 HDMI 信号到屏幕发光时间。当前 V4L2 驱动时间戳是 `CLOCK_MONOTONIC + END_OF_FRAME`，所以最早的可观测边界是“HDMI RX 帧结束”，不包含一帧从首行输入到末行输入的时间；客户端合成、显示排队和屏幕扫描也需要外部测量。

所有功能默认关闭：

```text
rkmpp_profile = disabled
rkmpp_profile_overlay = disabled
```

启用 `rkmpp_profile_overlay` 时自动启用 `rkmpp_profile`。

## 2. 指标定义

| HUD/日志名称 | 起点 | 终点 | 含义 |
| --- | --- | --- | --- |
| RX EOF-DQ | V4L2 `END_OF_FRAME` | `VIDIOC_DQBUF` 返回 | 驱动/采集排队年龄，不是完整 HDMI 输入时间 |
| CAP QUEUE | DQBUF 返回 | 编码线程取出图像 | Sunshine 采集队列等待 |
| RGA | RGA 调用前 | RGA 调用后 | 格式转换、缩放或占位帧填充；直通帧单独计为 bypass |
| MPP ENCODE | `encode_put_frame` 前 | 完整 `MppPacket` 可用 | RKMPP 主机可见编码耗时 |
| ENC QUEUE | 完整 `MppPacket` 可用 | 网络线程开始处理 | 编码后队列等待 |
| PACKET-SEND | 网络线程开始处理 | 最后一次发送批次返回 | 分包、FEC 和内核发送提交 |
| HOST-PACKET | V4L2 EOF | 网络线程开始处理 | 与 GameStream `frame_processing_latency` 同边界 |
| HOST-SEND | V4L2 EOF | 最后一次发送批次返回 | Sunshine 主机侧最完整可观测时间 |

窗口固定为 5 秒，每项最多保留 512 个样本，超过容量只增加丢弃计数，串流热路径不进行无界内存增长。

## 3. 分步实施与验收

### 步骤 0：记录基线与运行边界（已完成）

要做什么：

- 记录分支、HEAD、内核、HDMI RX 节点、正在运行的 Sunshine 二进制和配置。
- 保留现有服务，不在开发构建阶段重启。
- 约束现场测试为短时测试，不执行 50 次重连、两小时串流或强制 4K 验收。

验收标准：

- 基线可追溯，开发构建由 `./scripts/build-rkmpp.sh` 生成，不指定构建目录。
- 未覆盖或清理用户的无关文件。

### 步骤 1：确认 V4L2 时间戳语义（已完成）

要做什么：

- 解码 V4L2 timestamp type/source flags。
- 验证 Linux `CLOCK_MONOTONIC` 与 C++ `steady_clock` 可直接比较。
- 用真实 `/dev/video0` 连续采样确认时间戳源。

验收标准：

- 单元测试覆盖 clock/source 解码和时钟对齐。
- 实机结果为 `ts-monotonic, ts-src-eof`，60 Hz 帧间隔约 16.66 ms。
- HUD 明确显示 `RX EOF-DQ`，不误称为完整 HDMI capture latency。

### 步骤 2：建立固定大小的逐帧数据模型（已完成）

要做什么：

- 为真实帧、占位帧和重复帧建立类型标记。
- 贯穿保存采集序号、V4L2 flags、RGA 使用状态和各阶段单调时钟点。
- 使用 `std::optional` 表达缺失阶段，禁止用 0 冒充有效耗时。

验收标准：

- profile 结构不拥有动态缓冲区。
- 占位帧、重复帧、RGA bypass 不混入不适用的百分位。

### 步骤 3：埋点 HDMI RX、队列、RGA 和 RKMPP（已完成）

要做什么：

- DQBUF 后立即记录 dequeue 时间。
- 编码线程取帧时记录 capture queue exit。
- RGA 调用前后记录时间；直通路径标记 bypass。
- RKMPP submit 前后及完整输出包返回时记录时间。

验收标准：

- 直接 DMA-BUF 和 RGA fallback 两条路径都能把同一个 profile 传到编码包。
- 生命周期仍由原有 holder 保证，profile 不改变 DMA-BUF 所有权。

### 步骤 4：对齐 GameStream 主机延迟字段（已完成）

要做什么：

- 网络线程取包时记录统一的 `packetize_begin`。
- `frame_processing_latency` 与内部 `HOST-PACKET` 使用同一个时间边界。
- 按协议量化为 0.1 ms，并对 `uint16_t` 饱和。

验收标准：

- 单元测试覆盖 0.1 ms 舍入和饱和值。
- Moonlight 原生统计继续显示主机处理延迟，不需要客户端修改。

### 步骤 5：实现有界窗口和百分位（已完成）

要做什么：

- 每 5 秒计算 count/missing/invalid/min/P50/P95/P99/max。
- 每项使用固定 512 样本数组，窗口结束后复用。
- 单独输出 captured、placeholder、repeated、RGA bypass 和 dropped samples。

验收标准：

- 测试覆盖所有阶段计算、nearest-rank 百分位、缺失/逆序时间点和样本分类。
- 窗口满后只计数丢弃，不扩容。

### 步骤 6：日志与配置入口（已完成）

要做什么：

- 增加 `rkmpp_profile` 和 `rkmpp_profile_overlay`。
- 每个窗口在 info 日志输出统计。
- 补齐 Web UI、英文 locale 和配置文档。

验收标准：

- 两项默认关闭，overlay 自动打开 profile。
- `ConfigConsistencyTest` 全部通过。

### 步骤 7：验证 RKMPP 硬件 OSD ABI（已完成）

要做什么：

- 使用 MPP 推荐的 `MppFrame` metadata `KEY_OSD_DATA`，不使用已废弃的 `MPP_ENC_SET_OSD_DATA_CFG`。
- 实现单区域、16 像素对齐、8-bit palette-index bitmap。
- 在真实 HDMI RX 帧上完成短时 H.264/H.265 编码并解码检查。

验收标准：

- H.264 和 H.265 均以 10 帧、1 个正式 session 加 1 帧 warmup 编码通过。
- 访问单元数量、IDR/参数集和 FD 生命周期检查通过。
- 解码 PNG 中能看到硬件烧录内容。

### 步骤 8：实现客户端无关的文字 HUD（已完成）

要做什么：

- 使用固定 5x7 字体和 640x160 palette bitmap。
- 首个窗口前显示 collecting 状态，之后显示八项 P50/P95/P99 和异常计数。
- 仅在新窗口发布时重绘和复制 OSD buffer，不逐帧格式化文字。

验收标准：

- bitmap 大小固定，OSD 坐标与尺寸满足 16 像素对齐。
- 合成快照的文字硬件编码、解码后可读。
- Moonlight 无论平台和变体都能看到，因为文字已成为编码视频的一部分。

### 步骤 9：OSD 不支持时的 RGA 回退（条件不成立，无需启用）

要做什么：

- 仅当 RKMPP OSD 在目标 SoC/codec 上失败时，才在现有 RGA 转换路径混合 HUD。

验收标准：

- 当前 RK3588 H.264 硬件 OSD 已通过，因此保持 RGA 回退未启用，避免增加一次不必要的图像处理。
- 后续若某 codec 实机失败，再用同一 bitmap 增加 RGA blend，并分别统计其成本。

### 步骤 10：Moonlight 现场短时验收（待执行）

要做什么：

- 用新构建和测试配置启动独立 Sunshine 实例或安排短暂停机替换现有实例。
- 开启 `rkmpp_profile_overlay`，从 Moonlight 连接 5 分钟。
- 同时保存 Sunshine 日志、Moonlight 原生网络/解码统计和 HUD 画面。
- 分别检查 H.264 和实际常用 codec；分辨率以 1080p60 为必测，其他模式按可用性补测。

验收标准：

- HUD 首先显示 collecting，5 秒后稳定更新。
- HUD `HOST-PACKET` 与 Moonlight host processing latency 在 0.1 ms 量化误差范围内一致。
- 5 分钟内没有编码错误、FD 增长、明显帧率下降或 HUD 闪烁。
- 关闭两个开关后日志与画面不再出现 profile 内容。

### 步骤 11：严格端到端外部校准（待执行）

要做什么：

- 在 HDMI 源画面中产生可重复的 LED/高对比度变化。
- 用高速相机同时拍摄源显示与 Moonlight 显示，或使用光电二极管/逻辑分析仪。
- 将外部 photon-to-photon 结果与同一时段的 `HOST-SEND + Moonlight network + decode` 对齐。

验收标准：

- 至少 100 次事件，报告 P50/P95/P99。
- 明确给出未被软件时间戳覆盖的残差：HDMI 首行到 EOF、客户端渲染/显示队列和屏幕扫描。
- 外部结果可解释且可重复，才可称为“HDMI 信号输入到 Moonlight 屏幕显示”的严格总延迟。

## 4. 当前退出条件

代码级与硬件 OSD 级工作完成后，不自动重启正在运行的 Sunshine。步骤 10 需要一个 Moonlight 客户端连接窗口；步骤 11 需要外部拍摄或传感器设备。两项均以短时、可回滚方式执行。
