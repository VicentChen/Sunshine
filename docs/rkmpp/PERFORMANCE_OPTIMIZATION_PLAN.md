# RKMPP 低延迟与抖动优化实施计划

## 1. 目的

本文对既有优化建议做源码复核，并给出可由后续 Agent 分阶段执行的计划。目标不是追求单个最好看的平均值，而是在 ROCK 5B+ 的 HDMI RX → RKMPP → Sunshine → Moonlight 链路上：

- 降低稳定串流时的主机处理延迟 P95/P99；
- 消除启动、信号恢复和短时阻塞后继续编码旧帧造成的高延迟；
- 保持 H.264/HEVC 码流正确、Moonlight 按需 IDR 恢复可靠、资源占用有界；
- 用同一套可复现基线决定是否保留每项优化，避免把未经测量的复杂度合入正式路径。

本文不把 Moonlight 的网络、解码和显示统计简单相加为端到端延迟。Moonlight 的 `Average network latency` 是 client → host → client 的 RTT，`Average decoding time` 是帧解码并可供渲染的平均时间；客户端帧队列、渲染、VSync、合成器和显示扫描仍是独立部分。1080p60 的帧周期是 16.67 ms，因此当前约 5 ms 的硬件 HEVC 解码时间是合理结果，不是本计划的首要目标。同一局域网内 12 ms RTT 偏高，但应先结合 variance、网络丢帧和 jitter 丢帧判断；公网 12 ms 则已经很好。

## 2. 复核依据与当前事实

### 2.1 当前实现

- `src/platform/linux/rkmpp.cpp` 每帧调用一次 `mpp_buffer_import()`，并在该帧结束时 `mpp_buffer_put()`。
- 每帧创建一个 8 MiB MPP 输出 buffer，并把 `KEY_OUTPUT_PACKET` 附到输入 `MppFrame`；返回包必须与所提供的 packet 是同一个对象。
- `encoded_packet_t` 将 `MppPacket` 和 backing `MppBuffer` 一直持有到网络消费者销毁该视频包，应用层没有先复制到 `std::vector`。
- 编码配置当前明确设置 `split:mode=0`、`split:out=0`，收到 partition packet 会报错。
- RKMPP encoder 未设置 `PARALLEL_ENCODING`，捕获、转换和编码走 `captureThreadSync()`。
- 已有 profile 覆盖 `encode_put_frame`、等待完整输出、编码后队列、封包/发送以及 V4L2 EOF 到网络线程的完整分段，因此无需再新建基础埋点。
- `validate_encoder_config()` 当前拒绝 `gop=0`；若要测试无限 GOP，必须同时修改配置语义和测试。
- 直通 `rkmpp_encode_session_t` 创建 encoder 时只传递 codec、布局和尺寸，因而使用 `encoder_config_t` 的默认 60/1 FPS、12 Mbps、GOP 60；RGA 路径才完整使用客户端帧率和码率。该差异必须在任何性能 A/B 前修正。

### 2.2 当前主机日志所反映的基线

`runtime-home/config/sunshine/sunshine.log` 中 2026-08-28 的 1080p60 HEVC 直通样本不是受控验收，但足以用于安排优先级：

| 指标 | 稳态观察值 | 结论 |
| --- | --- | --- |
| MPP submit P50 | 约 4.1–5.0 ms | 编码时间主要花在 `encode_put_frame()`，符合 MPP 输入同步消费语义 |
| MPP output wait P50 | 约 0.1–0.45 ms | 等完整输出通常不是主要瓶颈 |
| MPP encode P50 | 约 4.4–5.1 ms | 已明显低于 16.67 ms 帧周期 |
| MPP encode P95 | 多数约 4.9–5.9 ms | 优先优化尾延迟，而不是重写整个编码架构 |
| HOST-PACKET P50 | 约 5.0–6.5 ms | 当前主机链路总体正常 |
| Encoded queue P50 | 约 0.03–0.06 ms | 稳态网络消费者没有明显积压 |
| 恢复期 RX driver age | P95 曾约 848 ms、最大约 902 ms | 信号恢复后确实可能编码积压旧帧，应优先修复 freshness |

正式结论必须以第 4 节的受控基线重测为准。

### 2.3 原建议逐项结论

| 建议 | 复核结论 | 调整后的处理方式 |
| --- | --- | --- |
| 增加 `put/get` 分段时间点 | 有道理，但已经完成 | 直接作为所有 A/B 的测量基础 |
| `base:low_delay=1`、`rc:max_reenc_times=0` | 有依据，值得 A/B | 目标机 MPP 1.3.9 的库包含这两个配置键；先保持 split 关闭，分别隔离 low-delay 与禁止重编码的收益 |
| 缓存输入 DMA-BUF import | 有道理 | 必须使用“捕获 generation + buffer index”或同等稳定身份，不能只用可能被系统复用的 fd 数值 |
| 复用 8 MiB 输出 buffer | 方向合理，但优先级被原建议高估 | 当前只能确认每帧 acquire/release，不能据此断言每帧发生 8 MiB 物理分配；先测耗时，再决定是否实现有界池 |
| GOP 60 改 300/0 | 有道理，属于码流/网络 A/B | `gop=0` 当前被校验拒绝；只有 IDR 请求和丢包恢复测试通过后才能采用 |
| Annex-B 只在 `output_intra()` 后扫描 | 正确且低风险 | 利用短路求值，补单元测试或可观测调用计数 |
| backlog 时只保留最新帧 | 有道理，且日志已有明确证据 | 作为恢复和 stall 场景的 P1 项，增加显式 stale-drop 计数 |
| slice low-delay 边编码边发送 | MPP 能力真实，但原建议低估了 Sunshine 改造范围 | 先做可行性阶段；当前发送端要先知道整帧长度、`lastPayloadLen` 和 FEC block 数，不能仅把 partition 当普通 packet 推入队列 |
| MppTask pipeline | 仅为条件性研究项 | MPP 文档同时指出高级 task API 更复杂且可能效率更低；当前 `KEY_OUTPUT_PACKET` 已返回调用方提供的对象，不能假设改成 MppTask 必然更快或才是零拷贝 |

## 3. 总体验收规则

除非某步骤另有规定，每项候选改动必须满足以下共同条件：

1. 基线和候选使用同一 HDMI 源、分辨率、帧率、codec、码率、FEC、网络路径、Moonlight 设备和客户端设置。
2. 1080p60 HEVC 为主验收；H.264 必须做兼容性验收。直通和 RGA fallback 分开统计，不混合样本。
3. 每个方案至少执行 3 次、每次 10 分钟；忽略连接后的前 10 秒 warm-up，比较三次运行各自统计值的中位数。
4. 保存 Sunshine 的每个 5 秒 profile window、Moonlight 完整统计、MPP 版本、commit、实际 V4L2 fourcc/stride、协商码率和 GOP。
5. 性能候选只有在 `HOST-PACKET` P95 降低至少 0.5 ms 或 10%（取较容易达到者），并且 P99、网络丢帧、jitter 丢帧和解码时间无显著回退时才默认启用。低于阈值的改动可保留为实验开关，也可删除，不以“理论上更快”作为合入理由。
6. 所有码流都能由 `ffmpeg`/`ffprobe` 连续解析和解码；访问单元数、帧数、IDR 标记和参数集符合预期，Moonlight 无花屏、绿块、长期黑屏或 decoder reset。
7. 改动方法必须有测试，目标覆盖 changed code 的 100%；新增/修改 C++ API、结构体和成员均按仓库要求补全 Doxygen，并通过 `.clang-format`。
8. 30 分钟稳态串流期间 RSS、DMA-BUF/MPP buffer 数和 `/proc/<pid>/fd` 不单调增长；退出、重连和异常恢复不能死锁。

## 4. 分阶段实施

### 阶段 0：冻结可比较基线并修正配置一致性（P0）

要做什么：

- 将直通和 RGA 路径统一到一个经过测试的 `make_encoder_config()`，确保实际传给 MPP 的 FPS、分数帧率、bitrate 和 GOP 来源一致。
- 在 encoder 创建日志或测试可见状态中记录最终 codec、尺寸、FPS、bitrate、GOP、low-delay、max re-encode 和 split 设置。
- 用现有 `rkmpp_profile` 完成 HEVC/H.264 直通基线；另做一次尺寸不匹配的 RGA 基线。
- 同时记录 Moonlight 的 network RTT/variance、network drop、jitter drop、decode、frame queue 和 render 指标。

验收标准：

- 30、60 和 60000/1001 FPS，以及至少两个非默认码率，在直通和 RGA 配置构造测试中得到完全相同的 MPP 配置语义。
- 现场日志中的最终配置与 Moonlight 请求一致，不再静默落到 60 FPS/12 Mbps 默认值。
- 每个 10 分钟运行的 `captured_frames` 与实际帧率一致，profile 无 missing/invalid/dropped sample。
- 生成基线表，至少包含 MPP submit/output wait/encode、RX driver age、HOST-PACKET、HOST-SEND 的 P50/P95/P99/max。

### 阶段 1：低延迟配置 A/B（P1）

要做什么：

- 给 `encoder_config_t` 增加内部可测试的 low-delay 和 max-reencode 语义，默认先保持现状。
- 在 `split:mode=0`、`split:out=0` 不变时依次测试：
  - A：当前默认；
  - B：仅 `rc:max_reenc_times=0`；
  - C：`base:low_delay=1` 且 `rc:max_reenc_times=0`。
- 不在同一次实验中引入 GOP、buffer cache 或 slice 变化。
- 记录 MPP 配置失败、每帧大小、IDR 大小、实际码率、MPP submit/encode P95/P99 和 Moonlight 解码错误。

验收标准：

- 目标机 MPP 1.3.9 对 H.264、HEVC 的配置均明确成功；若某 codec/硬件不支持，应在创建阶段给出可读错误或回退，不能运行中静默改变语义。
- 每组完成第 3 节的三轮测试和一轮 30 分钟稳定性测试。
- 若 C 达到全局性能采纳阈值且画质、实际码率、最大帧大小无不可接受回退，则将 C 设为 RKMPP 默认；否则保留当前默认并记录结果。
- `rc:max_reenc_times=0` 是否单独保留由 B 对 A 的结果决定，不把 C 的收益错误归因给单一配置键。

### 阶段 2：帧 freshness 与输入 lease（P1）

要做什么：

- 为 HDMI RX 实现“阻塞等到至少一帧，然后非阻塞 drain 到最新完整帧”的 API；每个被丢弃的旧 buffer 立即 QBUF，只将最新帧交给编码器。
- 增加 stale-drop 计数和被选择帧的 sequence/age 统计，区分主动 freshness drop、驱动丢帧和网络丢帧。
- 依据 MPP 文档所述 `encode_put_frame()` 返回时硬件已完成输入图像使用，评估在其返回后立即释放 input holder，而不是等完整输出 packet。该改动必须单独 A/B。
- 不允许 drain 循环阻塞，也不能在 steady-state 每帧无条件制造丢帧。

验收标准：

- 稳态 1080p60 时主动 stale-drop 为 0，或只在已证明存在 backlog 时增加；输出 sequence 单调递增。
- 注入 100 ms 和 500 ms 编码线程 stall 各 20 次后，恢复后的首个编码帧年龄不超过两个帧周期（60 FPS 时 33.4 ms），且不会继续逐帧追赶旧队列。
- HDMI 拔插/重锁 20 次后，不再出现数百毫秒级 RX driver age；若驱动在恢复时只能提供旧时间戳，必须明确记录并触发丢弃，不能把它发送给客户端。
- 提前释放 holder 的实验中，H.264/HEVC 各至少 10,000 帧无图像损坏、V4L2 QBUF 错误、MPP 错误或 FD 增长；若无法证明安装版本的输入消费边界，则保持当前保守生命周期。

### 阶段 3：消除重复的 input import（P2）

要做什么：

- 为生产者 buffer 增加稳定 cache key，至少包含捕获/分配 generation 和 buffer index；fd 仅作为待导入资源，不作为跨重开唯一身份。
- 在 encoder 生命周期内缓存每个 V4L2 buffer 和每个固定 RGA target 的 `MppBuffer`。
- source change、encoder reconfigure、RGA pool 重建和 session teardown 时显式失效对应 cache。
- 保持逐帧 holder：缓存 MPP handle 不等于允许正在编码的 V4L2 buffer 提前 QBUF。

验收标准：

- 稳态 `mpp_buffer_import()` 次数不超过当前 generation 的唯一 buffer 数；不再随编码帧数线性增长。
- fd 数值在 source recovery 后被复用时不会命中旧 generation；单元测试覆盖 fd 相同但 generation 不同的场景。
- 50 次 source change/reconfigure、H.264/HEVC 各 30 分钟后无 stale mapping、图像损坏、FD/handle 增长或 teardown use-after-free。
- 只有达到第 3 节性能采纳阈值，或能证明 CPU/内核调用与尾延迟显著下降时，才保留这项复杂度。

### 阶段 4：输出 buffer acquire 成本与有界池（条件性 P2）

要做什么：

- 先单独测量 `mpp_buffer_get()`、`mpp_packet_init_with_buffer()` 和释放路径的 P50/P95/P99、调用次数及同时在途 packet 数。
- 只有 acquire/release 对尾延迟有可测贡献时，才实现固定容量 output pool。
- pool slot 由 `encoded_packet_t` 持有，必须等网络消费者完成该帧后归还；pool 满时采用明确的有界 backpressure/丢帧策略，不能动态无限扩容。
- 第一版保持已验证的 8 MiB 容量，另行验证高码率/高分辨率 IDR 是否需要更大上限，不能因缩小 buffer 引入截断。

验收标准：

- 未实现池之前先提交测量报告；若 acquire P95 小于 0.1 ms 且与 HOST-PACKET/MPP encode 尾延迟无相关性，本阶段结束为“不实施”。
- 若实施，预分配数量覆盖实测最大在途 packet 数并留一个 slot；30 分钟内运行时 MPP 输出分配次数固定，不随帧数增长。
- 人工暂停网络消费者时内存保持上限、编码线程不会永久死锁，恢复或关闭能够释放所有 slot。
- 最大 IDR、正常 P 帧、H.264/HEVC、FEC 开关组合均无截断或复用中的数据覆盖。

### 阶段 5：GOP 与按需 IDR A/B（P2）

要做什么：

- 测试 GOP 60、300，以及 `gop=0 + Moonlight 按需 IDR`。
- 为 `gop=0` 明确定义有效配置并修改校验测试；不能用特殊值绕过配置验证。
- 记录周期 IDR 的 packet 大小、FEC block 数、`PACKET-SEND` P95/P99、网络 RTT/variance 和 jitter drop。
- 用真实控制事件反复请求 IDR，并用受控丢包或客户端恢复流程测试参考链恢复。

验收标准：

- 每种 GOP 下访问单元和预期 IDR 数相符；每个 IDR 前都有 H.264 SPS/PPS 或 HEVC VPS/SPS/PPS。
- 20 次显式 IDR 请求全部在两帧内产生真实 IDR，Moonlight 20 次全部恢复且无长期黑屏。
- 20 次受控丢包测试中客户端均能请求并恢复；记录请求到首个可显示恢复帧的 P50/P95/max。
- 仅当较长/无限 GOP 显著降低周期性 IDR 引起的 PACKET-SEND 或网络尾延迟，且恢复可靠性不下降时才改变默认值。

### 阶段 6：Annex-B 扫描短路（P2，小改动）

要做什么：

- 把 IDR 判断改为先检查 `encoded.output_intra()`，只有为真时才调用 `annexb_first_vcl_is_idr()`。
- 保留 bitstream 扫描作为 MPP metadata 的真实性校验，不能只相信 `KEY_OUTPUT_INTRA`。

验收标准：

- H.264/HEVC 的 IDR、普通 P 帧、缺参数集和错误 metadata 组合均有测试。
- 普通 P 帧路径不调用 Annex-B 扫描，最终 `packet->is_idr()` 结果与修改前一致。
- 性能收益无需达到全局阈值；该改动以语义等价和减少无用工作为验收依据。

### 阶段 7：slice low-delay 可行性门（P3，不直接产品化）

要做什么：

- 先在独立 RKMPP smoke 中测试 `split:mode=BY_CTU/BY_BYTE`、`split:arg`、`split:out=LOWDELAY`，记录首 partition、EOI、完整访问单元和普通整帧模式的时间差。
- 验证目标 MPP 1.3.9 在 RK3588 的 H.264/HEVC 实际行为，不仅依据头文件存在枚举。
- 在修改网络层前写清 GameStream 约束：`video_short_frame_header_t::lastPayloadLen`、总帧长度、FEC block 数、SOF/EOF、frame index 和加密/FEC 如何在首 slice 发送前确定。
- 若必须等 EOI 后再聚合/复制所有 partition，记录其净收益；这种实现不能宣称“边编码边发送”。

可行性验收标准：

- H.264/HEVC 各至少 10,000 帧能按 SOI/EOI 重组为与整帧模式等价的 Annex-B 访问单元，并可完整解码。
- 首 partition 相对 EOI 的提前量 P50 至少 1 ms，否则停止网络层改造。
- 形成一份不破坏现有整帧发送器的协议设计；若无法在发送首包前正确确定短帧头和 FEC 元数据，本阶段结论应为“不产品化”。

产品化验收标准（只有可行性通过后适用）：

- Moonlight 无需修改即可连续播放，SOF/EOF、FEC、加密和 IDR 恢复全部通过。
- “V4L2 EOF → 首个 UDP send”P95 相比整帧模式降低至少 1 ms，同时“V4L2 EOF → 最后 UDP send”和网络丢帧不回退。
- partition 生命周期、同帧 metadata 和错误中止均有测试；任何半帧错误都触发下一帧 IDR 恢复，不发送无法完成的访问单元。

### 阶段 8：MppTask/异步 pipeline 条件门（P3）

仅在阶段 1–7 后仍满足以下条件时启动：MPP submit/encode 仍是 HOST-PACKET P95 的主要部分，且目标是进一步让捕获与硬件编码重叠。

要做什么：

- 先做 standalone prototype，对比 simple API + `KEY_OUTPUT_PACKET` 与 poll/dequeue/enqueue MppTask 的 submit、首输出、完整输出、CPU 和 buffer 生命周期。
- 为每个 in-flight frame 保留 V4L2/RGA holder、profile、frame index、PTS、IDR 状态和 output slot，直到 MPP 明确归还输入所有权。
- 设定固定最大 in-flight 深度；满时优先保持 freshness，不允许排队延迟无界增长。
- 不因为 MppTask 文档提到 zero-copy 就假定当前路径有 copy；当前实现返回调用方提供的同一个 output packet，这一事实必须用 profile/copy trace 再比较。

验收标准：

- standalone prototype 达到 `HOST-PACKET` 潜在收益至少 1 ms，或显著降低 P99，才允许进入 Sunshine 主路径。
- 10,000 帧压力测试中 frame index/PTS/profile 不错配，所有 input holder 和 output slot 恰好释放一次。
- in-flight 满载、网络线程暂停、MPP timeout、source change 和 teardown 均不会死锁、重复 QBUF 或使用已释放 DMA-BUF。
- 接入后通过第 3 节全部验收；未达阈值则保留 simple API。

## 5. 推荐执行顺序与停止条件

```text
阶段 0：配置一致性 + 正式基线
  ↓
阶段 1：low_delay / max_reenc 独立 A/B
  ↓
阶段 2：恢复期只取最新帧 + 输入 lease A/B
  ↓
阶段 3：input import cache
  ↓
阶段 4：先测 output acquire，再决定是否做 pool
  ↓
阶段 5：GOP / 按需 IDR
  ↓
阶段 6：Annex-B 短路小优化
  ↓
阶段 7：slice feasibility（通过门槛才产品化）
  ↓
阶段 8：MppTask（仍有明确瓶颈才开始）
```

每个阶段应单独提交测量结果，禁止把 low-delay、GOP、buffer cache 和 pipeline 一次性叠加后只比较最终平均值。若阶段 1–6 已使稳态 `HOST-PACKET` P95 稳定低于 6 ms、P99 低于 8 ms，恢复后帧年龄低于两个帧周期，且网络 RTT 仍约 12 ms，则停止侵入式 RKMPP 改造，后续收益应优先从局域网、客户端 frame queue/render/VSync 和外部 photon-to-photon 测量中寻找。

## 6. 参考资料

- Rockchip MPP Developer Guide：<https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_EN.md>
- Rockchip MPP encoder sample：<https://github.com/rockchip-linux/mpp/blob/develop/test/mpi_enc_test.c>
- Moonlight performance statistics definitions：<https://github.com/moonlight-stream/moonlight-docs/wiki/Frequently-Asked-Questions>
