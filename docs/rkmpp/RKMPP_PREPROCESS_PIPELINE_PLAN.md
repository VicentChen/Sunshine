# RKMPP RGA/UI 预处理流水线计划

## 状态

- 状态：代码改造与模块测试完成；实机性能矩阵待验收
- 实施基线：`54b78fe`
- 已完成：prepared-frame 合约、每会话 preprocess worker、raw/prepared latest-only 覆盖、sticky IDR、双 RGA target、Vulkan UI 所有权迁移、generation/layout/route 失效、跨线程 profile 与压力计数
- 自动验证：`scripts/build-rkmpp.sh` 构建成功；`test_sunshine_rkmpp` 157/157 通过
- 保留本文：完成 4K60、source-change、多会话和 30 分钟 soak 实机矩阵后，按完成标准将最终数据并入长期文档并删除本文
- 基线提交：`54b78fe`（RKMPP 捕获与编码已并行）
- 范围：Linux RK3588 HDMI RX、RGA、Vulkan UI、RKMPP 编码会话及性能统计
- 目标：把会话级 RGA/UI 预处理从 MPP 编码线程拆出，使下一帧预处理可以与上一帧 MPP 编码重叠，同时保持采集线程不阻塞、直通路径零拷贝和 latest-frame 低延迟语义
- 非目标：通过重复帧让 VPU 人为满载、修改非 RKMPP 编码器、修改 HDMI EDID 策略、改变 Xbox Remote Play 协议或网络发送路径

## 已确认基线

2026-09-04/05 的 4K60 HEVC、Vulkan UI 可见实测已经确认：

- 启用 `PARALLEL_ENCODING` 后，捕获和编码不再位于同一线程；
- 连续 20 个五秒窗口共编码 5,970 帧，平均 59.70 fps，`freshness_drops=0`；
- Moonlight 收帧、解码和渲染均为 59.93 fps，网络丢帧与 jitter 丢帧均为 0；
- `Capture queue`：P50 0.290 ms、P95 9.964 ms、P99 13.056 ms；
- `RGA`：P50 3.324 ms、P95 3.533 ms、P99 3.641 ms；
- `MPP encode`：P50 9.253 ms、P95 10.261 ms、P99 10.510 ms；
- `MPP total`：P50 9.702 ms、P95 10.916 ms、P99 11.267 ms；
- `Host to send`：P50 16.497 ms、P95 27.188 ms、P99 30.613 ms；
- RKMPP output pool 峰值 lease 为 2/6，等待次数为 0。

当前 RGA 使用 `IM_SYNC`。UI 可见且 HDMI 输入为 NV12 时，同一个编码线程依次执行：

```text
取出 capture image
  -> 全帧 RGA 转换/复制
  -> Vulkan UI 必要时更新缓存面板
  -> Vulkan 直接覆盖私有 BGR target 的 UI ROI
  -> MPP encode_put_frame()/encode_get_packet()
  -> 发布码流
```

RGA/UI 与 MPP 对同一帧必然串行；当上一帧 MPP 或线程调度产生尾部抖动时，下一帧只能在 capture event 中等待。该等待是本计划要消除的“可避免饥饿”。

MPP 单帧约占 9.25 ms，而 60 Hz 帧周期为 16.67 ms，因此仅靠流水线不能让 VPU 在 4K60 下接近 100% 占用。自然帧间空档不是故障，本计划不得通过重复编码旧帧填满它。

## 架构决策

### 不把 RGA/UI 直接放进共享 `captureThread`

`captureThread` 必须继续只负责：

- V4L2 dequeue 和 HDMI source-change/reinit；
- 建立帧元数据、generation 和 profile 起点；
- 将同一个只读 capture frame 扇出到各 streaming session；
- 回收不再被任何会话持有的 capture image。

不得在 `captureThread` 中执行 RGA、Vulkan render、UI 状态轮询、目标缓冲等待或 MPP 调用，原因如下：

1. `captureThread` 是所有异步会话共享的 critical-priority 线程，而输出分辨率、UI 和 RGA target 都是会话级资源。
2. 同步 RGA 在 4K 下通常占用 3 ms 以上，直接执行会暂停 DQBUF 和 source-change 恢复。
3. 一个慢客户端、目标池耗尽或 Vulkan/RGA 异常不得阻塞其他客户端和 HDMI capture queue。
4. 同一个 capture DMA-BUF 被扇出给多个会话时，任何会话都不得原地修改它；只有确认该帧只发布给一个会话时，BGR888 direct route 才可由该会话的 preprocess worker 原地覆盖。

### 采用三段式流水线

```text
共享 captureThread
  V4L2 DQBUF + metadata + fan-out
            |
            v  raw latest-only event（每会话）
会话级 preprocess worker
  route 判断 + RGA + Vulkan UI + prepared target
            |
            v  prepared latest-only event（容量 1）
会话级 encode worker
  RKMPP submit/get packet + packet queue
```

线程所有权必须固定：

- capture thread 独占 V4L2 capture/recovery 状态；
- preprocess thread 独占该会话的 RGA backend、Vulkan renderer、UI surface、RGA target pool 和 import cache；
- encode thread 独占 MPP context、输入 import cache、码流 output pool 和编码控制；
- 网络线程继续持有码流 output lease，行为不变。

## 不可破坏的不变量

### 捕获与低延迟

1. capture thread 永远不得等待 RGA target、prepared queue 或 MPP。
2. raw queue 和 prepared queue 都只保留最新一帧，容量不得随负载动态增长。
3. 被替换的帧必须立即释放其 holder，不得延迟 V4L2 buffer requeue。
4. 任何正常负载下，从 capture 到 prepared 最多保留一个等待编码的帧。
5. HDMI source-change 后旧 generation 不得进入新 MPP session。

### 数据所有权

1. capture DMA-BUF 默认只读；仅当 capture thread 确认当前帧只发布给一个会话、输入为 BGR888 且尺寸匹配时，允许该会话的 preprocess worker 用 Vulkan 原地覆盖 UI ROI。
2. UI 隐藏且输入 layout/尺寸匹配时，prepared frame 直接持有 capture frame lease，保持零拷贝。
3. 需要缩放、格式转换或 UI 覆盖时，只能写入该会话私有的 CMA target。
4. `IM_SYNC` 返回后可以释放 RGA source lease；target lease 必须保持到 MPP 完成该帧提交。
5. MPP 返回后必须立即归还 target；码流 output lease 与输入 target lease 相互独立。
6. BGR UI 直写 capture 的授权必须随每帧发布，不能从会话数或历史状态推断；未取得独占标记时必须使用私有 BGR target。

### 编码顺序与控制事件

1. 每个 prepared input 只能向 RKMPP 提交一次，继续遵守 `SINGLE_USE_INPUT`。
2. IDR 请求采用 sticky latch；如果携带请求的 raw/prepared frame 被覆盖，请求必须转移到下一张实际提交帧。
3. route、layout 或 generation 变化必须在下一张已准备帧上请求 IDR，并在 encode thread 清空旧 MPP input cache。
4. 无信号时只按现有低频策略产生 placeholder，不重复提交已消费 target。
5. EOS、shutdown 和 reinit 必须可中断所有 buffer wait，不能依赖超时完成退出。

### 多会话隔离

1. 每个 streaming session 拥有独立 preprocess worker、输出尺寸、UI session 和 target pool。
2. 一个会话丢帧、关闭 UI、切换 route 或销毁时不得改变其他会话的 prepared frame。
3. capture frame 可由多个会话只读持有；每个会话独立决定直通或转换。
4. 目标分辨率不同的两个会话不得共享可写 RGA target。

## 数据模型

### Raw frame

沿用 `std::shared_ptr<platf::img_t>` 作为 capture fan-out 对象，其中 RKMPP 路径必须保留：

- capture frame holder；
- capture format 和 generation；
- V4L2 timestamp、dequeue 时间和 sequence；
- connection/UI 状态快照；
- sticky 前的 per-frame IDR 标志；
- `frame_profile_t`。

raw event 仍为 latest-only。替换旧值时，应把旧值尚未兑现的 IDR 合并进会话级 sticky latch，并累计 `raw_replaced`。

### Prepared frame

新增 RKMPP 专用 `prepared_frame_t`，至少包含：

- 最终 `input_layout_t`；
- DMA-BUF fd 和 allocation size；
- PTS、generation 和稳定 cache key；
- 持有 direct capture frame 或私有 RGA target 的 type-erased holder；
- route：`direct`、`rga` 或 `placeholder`；
- 是否需要 IDR；
- 完整 profile 所有权。

encode thread 不得再访问 `hdmirx_img_t *`，也不得调用 RGA/Vulkan。它只消费 `prepared_frame_t` 并构造 `platf::rkmpp::input_frame_t`。

### Target pool

每个需要 RGA 的会话最多保留两个同格式 target。UI 隐藏的转换路径使用 NV12；UI 可见时使用 BGR888，以便 RGA 完整转换后由 Vulkan 直接覆盖：

- target A 可由 MPP 同步处理；
- target B 可同时由 preprocess worker 写入下一帧。

4K NV12 单 target 当前为 12,441,600 bytes，双 target 约占 23.7 MiB；4K BGR888 单 target 约 24.9 MiB，双 target 约 47.5 MiB。可见性切换需要改变格式时，必须先等旧 lease 全部归还再重建固定的两个 target。不得扩展到三个或动态增长。

prepared event 容量为 1。如果两个 target 分别被 MPP 和尚未消费的 prepared frame 持有，而新 raw frame 到达，preprocess worker 应先丢弃旧 prepared frame 并回收其 target，再处理最新 raw frame；不得等待 encode thread 后继续处理一张已经过时的 raw frame。

## 调度流程

### 直通帧

```text
capture 发布 raw frame
  -> preprocess 检查尺寸/layout/generation
  -> UI 隐藏且不需要转换，或独占的匹配 BGR888 需要可见 UI
  -> BGR888 可见时由 Vulkan 直接覆盖 capture ROI
  -> 构造 direct prepared frame，转移 capture lease
  -> 发布 prepared event
  -> encode thread 提交 MPP
  -> MPP 返回后释放 capture lease
```

该路径不执行 RGA，不分配 target。新增的一次 event handoff 必须控制在可测噪声范围内。

### RGA/UI 帧

```text
capture 发布 raw frame
  -> preprocess 获取/回收一个私有 target
  -> 全帧转换或复制到 target
  -> 必要时更新 Vulkan UI 缓存
  -> UI 可见时，target 为 BGR888，Vulkan 直接覆盖其 UI ROI
  -> IM_SYNC 完成，释放 capture lease
  -> 发布 target prepared frame
  -> 与此同时 preprocess 可使用另一个 target处理后续 raw frame
  -> encode thread 提交 MPP
  -> MPP 返回后归还 target
```

UI 隐藏后，下一张匹配输入立即恢复 direct route。单会话的匹配 BGR888 可见帧也保持 direct route；NV12、尺寸不匹配或共享 BGR888 可见帧使用私有 BGR target，绝不通过 RGA 把 Vulkan 结果覆盖回 NV12。

### Backpressure

按以下优先级处理压力：

1. 保证 capture thread 不阻塞；
2. 保证 MPP 已取得的 target 不被复用；
3. 丢弃尚未编码的最旧 prepared frame；
4. raw event 只保留最新输入；
5. 将丢弃计数和未兑现 IDR 转移到下一帧；
6. 仅当两个 target 都处于真实硬件 in-flight 状态时，preprocess worker 才进行可中断等待。

不得建立 FIFO backlog，也不得为了提高统计帧数编码已经过时的画面。

### Source change 与退出

```text
capture 检测 source-change
  -> raise reinit barrier，停止发布旧 generation
  -> preprocess 停止取 raw，清空 raw/prepared event
  -> encode 完成或中断当前同步 MPP 调用
  -> 释放 direct frame和全部 target holder
  -> 销毁 preprocess/UI/RGA session
  -> captureThread 确认旧 display/frame lease 已释放
  -> 重建 V4L2 capture queue
  -> 新 generation 建立新 preprocess/encode session
```

析构顺序必须保证 Vulkan renderer 在其 import target 和 RGA allocator 之前销毁。初始化、使用和析构 Vulkan/RGA 会话应发生在同一个 preprocess thread。

## 代码改造阶段

### 阶段 1：建立 prepared-frame 合约，不改变线程模型

- 在 RKMPP 私有实现中新增 `prepared_frame_t`、route 枚举和 holder 规则；
- 将当前 `rkmpp_encode_session_t::convert()` 拆为“准备输入”和“MPP 提交”两个明确步骤；
- encode 侧只接收 prepared frame，不再保存裸 `hdmirx_img_t *`；
- 暂时仍在 encode thread 同步调用 prepare，确保行为和性能基线不变；
- 为 direct、RGA、placeholder、IDR 和 generation transition 增加单元测试。

阶段验收：输出码流、UI、profile 和 4K60 指标相对 `54b78fe` 无显著回归。

### 阶段 2：引入会话级 preprocess worker

- 为每个 RKMPP streaming session 建立 raw event、prepared event 和 worker 生命周期；
- captureThread 仍按现有方式向 raw event 扇出；
- encode loop 改为等待 prepared event；
- raw/prepared 均采用 latest-only replacement；
- 实现 sticky IDR、shutdown、reinit 和错误传播；
- 保留 `SINGLE_USE_INPUT` 防止超时重复提交。

阶段验收：无信号、延迟唤醒和稳定 4K60 均无 `encode called without a converted frame`、死锁或重建循环。

### 阶段 3：双 RGA target 与覆盖策略

- 将当前单 target 池固定扩为两个；
- target holder 返回时唤醒 preprocess worker；
- prepared event 被替换时立即归还旧 target；
- 无 target 时优先回收旧 prepared，再进行可中断等待；
- 加入池峰值、等待和 stale replacement 计数。

阶段验收：Timeline 能在调度抖动或注入负载下观察到 frame N 的 MPP 与 frame N+1 的 RGA 重叠；target 不会同时被 RGA 和 MPP 写读复用。

### 阶段 4：迁移 Vulkan UI 所有权

- 将 UI snapshot 轮询、Vulkan render cache 和每帧 ROI compose 移到 preprocess worker；
- global controller 继续通过自身 mutex 接收输入和发布 snapshot；
- 独占匹配 BGR888 可见帧由 Vulkan 直接覆盖；NV12、尺寸不匹配或共享输入的可见 UI 使用私有 BGR target；
- generation 变化在可见性判断前失效全部旧 import；
- UI 初始化或运行失败必须解绑 backend，不能留下不可见 modal interception。

阶段验收：UI 开关、页面导航、大小切换和 Profile Timeline 在连续 route/source-change 下均正确；多客户端不出现叠加污染。

### 阶段 5：重初始化与故障收敛

- 为 capture、preprocess、encode 建立单向 stop/barrier 顺序；
- 覆盖 source-change storm、会话取消、Moonlight 重连和 Sunshine 退出；
- RGA/Vulkan 可恢复错误只影响当前会话；
- 转换必需但 RGA 失败时明确结束/重建当前编码会话，不得阻塞 capture thread；
- 删除旧 combined convert/encode 状态和不再需要的裸指针、单 target 注释。

阶段验收：旧 generation DMA-BUF 全部释放后才重建 capture queue，fd/handle/target 数量长期稳定。

### 阶段 6：性能统计和文档收口

- 将现有 `Capture queue` 明确为 raw preprocess queue，或新增不歧义的阶段名；
- 新增 `Prepared queue`，区分预处理完成到 MPP 开始的等待；
- 保留 `RGA`、`UI render`、`UI compose`、`MPP encode` 和 `Host to send`；
- 新增 `raw_replaced`、`prepared_replaced`、`target_waits` 和 sticky IDR transfer 计数；
- Timeline 按真实线程 lane 展示跨帧 RGA/MPP 重叠；
- 实施完成后将长期行为写入 `docs/rkmpp/FEATURES.md`，用户可配置项写入 `docs/rkmpp/README.md`。

## 测试计划

### 单元测试

- prepared direct frame正确持有并释放 capture lease；
- prepared RGA frame只持有 target，`IM_SYNC` 后释放 source；
- prepared replacement 立即归还旧 target；
- 两个 target中一个由 MPP 持有时，另一个仍可完成 RGA；
- 两个 target都不可用时等待可由 shutdown/reinit 中断；
- raw replacement 和 prepared replacement 都不会丢失 IDR；
- route/layout/generation 变化在下一张提交帧请求 IDR并清 cache；
- placeholder 不会被重复提交；
- direct/RGA 多次切换不泄漏 holder；
- 两个不同输出配置的会话不共享可写 target或 UI surface；
- profile 跨线程时间点缺失、倒序和被丢弃帧时保持有效。

### 模块测试

只构建并运行 `test_sunshine_rkmpp`，不得运行上游 `test_sunshine`。覆盖：

- preprocess worker 生命周期和 event replacement；
- synthetic capture producer + blocking fake RGA/MPP 的确定性重叠；
- reinit barrier 与 generation invalidation；
- RGA/UI failure isolation；
- direct、RGA、placeholder 和 source-change 路由。

测试中的 fake stage 必须能用 latch 精确控制 RGA 和 MPP 完成顺序，证明重叠与 holder 生命周期，不能依赖 sleep 猜测时序。

### 实机矩阵

1. 4K60 HEVC，输入和 Moonlight 均为 3840x2160，UI 隐藏，验证 direct bypass。
2. 4K60 HEVC，UI 持续可见，验证双 target RGA/UI 流水线。
3. 4K 输入、1080p Moonlight，UI 隐藏和可见各运行一次。
4. 无信号连接，Xbox 延迟唤醒到 4K，再关闭输入回到 placeholder。
5. 4K/1080p source-change 循环至少 20 次。
6. 两个 Moonlight 客户端使用相同和不同分辨率并发连接。
7. UI 连续开关、切页和切换尺寸，同时制造 source-change。
8. 持续 4K60 至少 30 分钟，记录内存、fd、target、output pool 和错误计数。
9. 注入 CPU 调度压力，确认 capture thread仍及时 DQBUF，preprocess 丢旧帧而不积压。

每次性能测试必须固定 codec、bitrate、UI 可见性、输入画面、Moonlight 显示模式和采样时长，并同时记录服务端窗口与 Moonlight statistics，不能把 UI 隐藏 direct 路径和 UI 可见 RGA 路径混为一组。

## 性能验收标准

以提交 `54b78fe` 的同条件 4K60 UI 可见数据为 A/B 基线：

1. 连续十分钟无 freshness drop、RGA/MPP 错误、session rebuild loop 或 output pool wait。
2. Moonlight incoming/decoding/rendering frame rate 均不低于 59.8 fps，网络与 jitter 丢帧均为 0。
3. `Host to send` P50 不得比 16.497 ms 回退超过 1 ms。
4. `Host to send` P95 目标不高于 24 ms，或在严格同条件 A/B 中至少改善 2 ms；若没有改善，必须用 Timeline 解释等待来源。
5. `Prepared queue` P50 应接近 0，P95 不得形成持续一帧以上 backlog。
6. 注入调度抖动时，Timeline 必须显示相邻帧 RGA/MPP 重叠，并且 capture queue 尾部不因 RGA 迁移而增加。
7. UI 隐藏且输入匹配时，至少 99.9% captured frame 走 direct bypass，新增 event hop 的 P50 开销不超过 0.2 ms。
8. target pool 峰值不超过 2/2；稳定 4K60 不等待 target，压力下允许 replacement 但不允许动态扩池。
9. 多会话和 source-change 矩阵结束后，DMA-BUF、RGA/Vulkan import、fd 和线程数量回到基线。

以上指标用于减少可避免的流水线饥饿，不以 VPU 100% 占用作为完成条件。若 60 Hz 输入下 MPP 在下一帧到达前完成，剩余空档属于输入节拍上限。

## 回滚边界

各阶段必须保持可独立回滚：

- 阶段 1 只引入数据合约，不启用新线程；
- 阶段 2 可以退回同线程 prepare；
- 阶段 3 可以退回单 target，但不得留下阻塞 capture 的代码；
- 阶段 4 出错时允许当前会话禁用 UI，视频必须继续；
- 任意阶段失败都不得关闭已验证的 `PARALLEL_ENCODING`，除非证明它本身造成独立回归。

不允许用无界队列、额外重复帧、提高 minimum FPS 或增加 captureThread 工作来掩盖流水线错误。

## 完成标准

全部条件满足后才可将本计划标记完成：

1. capture、preprocess、encode 三段线程边界及所有权符合本文不变量。
2. RGA/UI 不再由 encode thread 执行，也未进入共享 captureThread。
3. 双 target latest-only 策略、sticky IDR 和 reinit barrier 具有确定性测试。
4. direct 路径保持零拷贝；独占匹配 BGR888 的 UI 由 Vulkan 原地覆盖，所有 NV12 或共享可见输入都写入会话私有 BGR target。
5. 单会话、多会话、无信号、延迟唤醒和 source-change 实机矩阵通过。
6. 同条件 4K60 A/B 达到性能验收标准，且没有以内存或画面时效换取统计改善。
7. `scripts/build-rkmpp.sh` 构建成功，`test_sunshine_rkmpp` 全部通过；不运行上游 `test_sunshine`。
8. 所有新增或修改的 C++ API、类型、字段和状态具有完整 Doxygen 文档并通过格式检查。
9. 最终行为合并进 `FEATURES.md`/`README.md` 后删除本计划文件，不保留已完成计划归档。
