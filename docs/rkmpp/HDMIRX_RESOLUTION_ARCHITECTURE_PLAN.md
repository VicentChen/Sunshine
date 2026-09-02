# HDMI RX 分辨率架构重构计划

## 状态

- 状态：软件重构与自动验证已完成，等待实机验收矩阵确认
- 范围：Linux RK3588 HDMI RX、EDID、V4L2 捕获恢复、RGA/RKMPP 路由
- 目标：彻底移除 EDID 控制与视频数据传输之间的错误耦合，保证 HDMI 有输入时立即串流，并根据 Moonlight 请求从 HDMI RX 原生 EDID 中选择最接近的分辨率
- 非目标：修改 Xbox Remote Play 协议、改变非 RKMPP 平台的显示捕获、修改 `docs/rkmpp/SPEC.md`

## 2026-09-03 实施进度

已完成的软件改造：

- 删除旧 `session_negotiator_t`、640x480 重试、timing 稳定门禁、source-change EDID 权限和会话析构恢复；
- 新增进程级 `edid_controller_t`，独占 EDID 写入并持久化原生/最后应用快照；
- 重写 EDID 解析和投影，候选覆盖 Established Timing、Standard Timing、base/CTA DTD、CTA VDB 和 Y420 VDB，生产代码不再包含固定分辨率 EDID 模板；
- 捕获线程改为首个有效 dequeue 帧立即串流，尺寸匹配走直通，不匹配从第一帧走 RGA；
- encoder probe 改为显式只读用途，不写 EDID；
- encoder probe 和正式编码会话初始化均使用合成占位输入验证 RKMPP/RGA，不再因 Sunshine 启动或 Moonlight 连接瞬间没有 HDMI 帧而初始化失败；
- 删除旧协商测试，使用新的 parser/projector、控制器事件模型和 live-first 状态测试替代；
- `test_sunshine_rkmpp` 136 个测试全部通过，`sunshine` 与 `rkmpp_hdmirx_smoke` 已构建通过。
- 2026-09-03 无 HDMI 帧启动验证已确认 encoder probe 不再等待 dequeue，并成功识别 H.264/HEVC RKMPP；此前的 `failed to initialize video capture/encoding` 根因已消除。

尚未完成的是本文件“实机验收矩阵”。在真实 Xbox、RK3588 HDMI RX 和 Moonlight 的事件时间线确认前，本计划保持为活动计划，不宣称问题已经完成实机根治。

## 背景与已确认故障

当前实现把 EDID 协商、HDMI link 状态、V4L2 capture queue、占位帧和编码路径选择放进同一个会话状态机。实际测试已经出现以下反馈循环：

```text
写入目标 EDID
  -> RK3588 驱动执行 plugout/HPD 周期
  -> HDMI RX 重新检测到 640x480
  -> Sunshine 拒绝使用这个有效输入
  -> Sunshine 再次写入相同 EDID
  -> 延迟到达的 source-change 被视为新的外部链路
  -> 重置 EDID 重试额度
  -> 再次写入相同 EDID
```

2026-09-02 23:50 的实机日志中，Moonlight 会话从首次写入 EDID 开始连续约 86 秒只有绿色 placeholder。期间 HDMI RX 已多次重新检测到稳定的 `640x480` timing，但程序仍然保持 `captured=0`，并重复写入相同 EDID。Xbox 最终切换到 1080p 后，程序才偶然退出该循环。

这不是单一条件判断错误，而是以下架构问题共同造成的：

1. EDID 控制面决定捕获线程是否允许 dequeue 真实帧。
2. `source-change` 被错误地赋予重新写 EDID 的权限。
3. 每个 `hdmirx_display_t` 都拥有独立的 EDID 保存、写入和恢复生命周期。
4. 当前 EDID 生成器没有复用原生 timing，而是用四组硬编码 timing 重造 EDID。
5. 当前 EDID 解析器遗漏 established timing 和 standard timing，不能得到完整原生模式集合。
6. 写入前没有比较当前 EDID 与目标 EDID，缺少幂等性。
7. 单元测试没有模拟 `VIDIOC_S_EDID` 引发的真实 plugout/hotplug/source-change 事件链，反而把 640x480 重试和 source-change 重置重试额度当成正确行为。

## 不可破坏的不变量

后续设计和实现必须同时满足以下不变量。任何优化不得绕过这些约束。

### 视频优先

1. 只要 V4L2 成功 dequeue 一个有效 HDMI 帧，该帧必须立即进入编码路径。
2. 实际 HDMI 分辨率与 Moonlight 请求不一致不能阻止串流。
3. 实际尺寸一致时必须使用 RKMPP 直通路径，不使用 RGA 缩放。
4. 实际尺寸不一致时必须从第一帧开始使用 RGA 转换到 Moonlight 编码尺寸。
5. EDID 状态、Xbox Remote Play 状态和 timing 稳定计数不得阻塞真实 HDMI 帧。
6. 绿色 placeholder 只允许在没有任何可用 HDMI 帧时产生。有效帧必须始终覆盖 placeholder。

### EDID 单一所有权

1. 一个 HDMI RX 设备只能有一个进程级 EDID 控制器。
2. 编码器探测、capture queue 重建、source-change、Xbox wake 和 640x480 输入都不能直接写 EDID。
3. 相同目标 EDID 已经生效时，写入次数必须为零。
4. Moonlight 请求映射到新的原生模式时，正常路径最多执行一次 EDID 写入。
5. EDID 写入失败后的原始数据恢复是错误恢复事务，不属于正常重试，并且必须有明确上限。
6. 会话析构不得自动恢复 EDID并制造新的 HPD 周期。

### 原生模式真实性

1. 候选模式必须来自完整且校验有效的 HDMI RX 原生 EDID。
2. 选中的 timing 必须能追溯到原生 EDID 中的具体 Established Timing、Standard Timing、DTD、CTA SVD 或 Y420 SVD。
3. 不得使用固定的 720p、1080p、1440p、2160p 模板代替原生 timing。
4. EDID 投影器只能过滤、复制、重排和重新计算校验和，不得凭空创造原生 EDID 未声明的模式。
5. 音频、speaker allocation、HDMI VSDB、HDMI Forum VSDB 及与保留模式相关的能力块必须保持一致。

## 目标执行流程

### 进程启动

```text
打开 HDMI RX 控制接口
  -> 读取并完整校验当前 EDID
  -> 从持久化状态识别原生 EDID与 Sunshine 上次应用的受限 EDID
  -> 建立完整原生模式目录
  -> 缓存当前已应用 EDID 的内容哈希
  -> 编码器能力探测使用目标尺寸的合成占位帧，不读取或改变 EDID
```

### Moonlight 会话启动

```text
接收 Moonlight width/height/fps
  -> 从原生模式目录选择最接近分辨率
  -> 同分辨率时用请求刷新率或原生 preferred/native 顺序决胜
  -> 从原生 EDID 投影出只保留该分辨率的目标 EDID
  -> 目标与当前 EDID 相同：不写入
  -> 目标与当前 EDID 不同：执行一次写入并 readback 验证
```

EDID 控制事务与视频捕获状态相互独立。若写入前已经存在有效输入，现有帧可以立即用于建立 Moonlight 编码输出；驱动执行 HPD 周期时才进入短暂无信号处理。

### HDMI 帧处理

```text
成功 dequeue HDMI 帧
  -> 读取该帧自己的 capture format 和 generation
  -> 输入尺寸 == Moonlight 尺寸：RKMPP 直通
  -> 输入尺寸 != Moonlight 尺寸：RGA 转换后 RKMPP 编码
  -> 发送真实视频帧
```

### Source change

```text
收到 signal-lost/source-change 或 dequeue 超时
  -> 停止旧 capture queue
  -> 等待旧 generation 的 DMA-BUF lease 释放
  -> 重新查询 DV timings 与 G_FMT
  -> 重建 capture queue
  -> 第一帧立即进入直通或 RGA 路径
```

该流程不得调用 EDID 控制器。

## 设计与代码改造

### 阶段 1：移除旧协商状态机

删除 `session_negotiator_t` 及其全部重试状态：

- `safe_fallback_observations_`
- `safe_fallback_retry_attempted_`
- `awaiting_fresh_timing_`
- `notify_timing_refreshed()` 中的协商职责
- `notify_source_change()` 中的 EDID 职责
- 640x480 特殊重试策略
- timing 稳定计数对真实帧 dequeue 的门禁

从 `input_state_machine.cpp/.h` 移走全部 EDID 读写逻辑。输入状态机仅描述：

- `no_signal`
- `live`
- `reconfiguring`
- `shutdown`
- `fatal`

如果 UI 仍需要显示 EDID 事务状态，应从独立的只读状态快照取得，不得重新引入捕获门禁。

### 阶段 2：建立设备级 EDID 控制器

新增 `src/platform/linux/hdmirx_edid_controller.h/.cpp`，职责包括：

- 独占 EDID 写权限；
- 读取、校验和保存原生 EDID；
- 构建原生模式目录；
- 接收 Moonlight 目标并选择模式；
- 生成目标 EDID；
- 比较当前与目标 EDID；
- 执行最多一次的 guarded write；
- readback 并逐字节验证；
- 发布只读事务结果和选择结果；
- 记录原生 EDID、最后应用 EDID及其哈希，支持异常退出后的识别和恢复。

控制器的生命周期必须长于单个 `hdmirx_display_t`。同一目标的 Moonlight 断开重连应复用当前 EDID，不恢复、不重写。

删除通用 `video::config_t` 中用于修补探测副作用的 `configureHdmirxLink` 布尔字段。用明确的 API 区分：

- 只读 encoder probe；
- 正式 streaming target application。

### 阶段 3：重写 EDID 模式模型

扩展 EDID 解析器，使每个模式记录至少包含：

- 分辨率；
- 精确有理数刷新率；
- progressive/interlaced；
- preferred/native；
- 编码来源；
- 原始 DTD、standard timing 字节或 CTA VIC；
- 所属 block、data block 和索引；
- YCbCr 4:2:0 限制。

解析范围至少包括：

- Base Established Timings；
- Base Standard Timings；
- Base DTD；
- CTA DTD；
- CTA Video Data Block；
- CTA YCbCr 4:2:0 Video Data Block；
- CTA YCbCr 4:2:0 Capability Map。

如果原生 EDID包含当前实现不能安全解释的其他视频扩展，控制器必须明确报告并采取保守策略，不能静默丢弃或误写。

删除生产路径中的 `make_720p_edid()`、`make_1080p_edid()`、`make_1440p_edid()`、`make_2160p_edid()` 选择依赖。必要的合成 EDID只能作为单元测试 fixture 存在。

### 阶段 4：实现原生 EDID 投影器

用新的投影器替换 `restrict_edid_to_resolution()`：

1. 接收完整原生 EDID和已选择的原生 resolution group。
2. 保留该分辨率所有允许的原生刷新率，或按明确策略保留选定刷新率。
3. 原样复制对应 DTD/VIC/standard timing 编码。
4. 删除其他分辨率的 Established、Standard、DTD、CTA SVD 和 Y420 SVD。
5. 正确重建 native/preferred 标记。
6. 删除因 VDB 重排而失效的 Y420 capability map，或按新索引重建。
7. 保留仍然有效的非视频 CTA data block。
8. 更新 DTD offset、native DTD count、extension count 和所有 checksum。
9. 重新解析生成结果，确认其视频模式集合只包含预期分辨率。

写入前必须满足：

```text
目标 EDID 校验有效
AND 目标模式来源于原生 EDID
AND 重新解析结果符合预期
AND 目标 EDID != 当前已应用 EDID
```

### 阶段 5：重构 HDMI 捕获数据面

修改 `hdmirx_display_t`：

- 构造函数不再启动 session negotiation；
- `capture()` 不再先调用协商门禁决定是否 dequeue；
- 成功 dequeue 的帧直接发布；
- source-change 只设置 queue recovery；
- queue 重建后不等待固定次数的 timing sample；
- 第一帧自身即为当前 format 可用的最终证据；
- placeholder 生成只依赖当前确实无法取得帧。

订阅并区分 Rockchip 的 signal-lost 与标准 `V4L2_EVENT_SOURCE_CHANGE`，但两者都只影响 capture queue，不影响 EDID。

保留 capture generation 和 DMA-BUF lease 安全规则，确保旧 generation 不会被重新排入新 queue。

### 阶段 6：收敛 RKMPP/RGA 路由

Moonlight 编码尺寸在整个会话中保持固定，实际 HDMI 帧决定输入路由：

- 尺寸一致且 layout 可被 RKMPP 接受：直通；
- 尺寸不一致：RGA 转换到固定 NV12 target；
- source-change 后根据新帧重新选择路由；
- 路由切换必须请求 IDR 并清除旧 generation 的 MPP import cache；
- 路由切换失败时优先保留可工作的 RGA 输出，不允许回到“等待目标分辨率”的状态。

不得为了等待直通机会而扣留可以通过 RGA发送的帧。

### 阶段 7：EDID 生命周期和异常恢复

原生 EDID和最后一次 Sunshine 应用状态使用原子文件更新保存。启动恢复规则：

1. 当前设备 EDID 等于上次应用哈希：继续使用已保存的原生 EDID作为模式来源。
2. 当前设备 EDID 与上次应用哈希不同且自身有效：视为外部更新的原生 EDID，重新建立目录。
3. 当前设备 EDID 无效但存在有效原生快照：只允许执行一次恢复事务。
4. 无法证明存在可恢复原生 EDID：禁止写入，继续捕获实际输入并通过 RGA适配。

正常 Moonlight 会话结束不恢复 EDID。仅在明确的守护进程正常退出、设备重置或恢复操作中恢复原生 EDID。

## 测试计划

### 单元测试

#### EDID 解析

- 使用真实 Rockchip 340 MHz 与 600 MHz 原生 EDID作为 fixture。
- 验证 Established、Standard、Base DTD、CTA DTD、VDB 和 Y420 VDB 全部进入模式目录。
- 验证 1600x900、1440x900、1280x800、1024x768、800x600、640x480 不再丢失。
- 验证 interlaced、非法 checksum、截断 block、未知 extension 和失效 capability map。

#### 模式选择

- 精确目标优先。
- 不存在精确目标时，分辨率距离、宽高比和刷新率决胜结果确定且与输入顺序无关。
- 每个选择结果都能追溯到原生 EDID记录。
- 不允许选择解析器或投影器不能安全表达的模式。

#### EDID 投影

- 1080p 投影保留原生 `88/44/148` timing，不再出现硬编码 `48/32/200`。
- Standard Timing 被选择时保留其原始编码。
- CTA VIC、Y420 VIC、音频、speaker allocation、VSDB/HF-VSDB 保持一致。
- 生成结果重新解析后不包含其他分辨率。
- 两次生成同一目标得到完全相同字节。

#### 控制器不变量

- 当前 EDID 等于目标时写入次数为 0。
- 新目标正常路径写入次数为 1。
- 任意数量 source-change 后写入次数不增加。
- 640x480 输入不触发写入。
- Xbox wake 状态不触发写入。
- 写入成功但 readback 不一致时进入确定错误状态，不循环重试。
- 部分写入失败只执行一次原生 EDID恢复。
- 同目标连续建立和销毁会话不恢复、不重写。

#### 真实事件设备模型

替换仅记录 ioctl 次数的简单 mock，建立可编程 HDMI RX 模型：

```text
S_EDID
  -> signal lost
  -> HPD low interval
  -> hotplug
  -> timing available
  -> source-change
  -> frames available
```

该模型必须验证：即使 EDID 写入产生延迟 source-change，也不可能引发第二次 EDID 写入。

#### 捕获与编码

- 目标 1080p、首帧 640x480：第一帧走 RGA，不输出额外 placeholder。
- 目标 1080p、首帧 1080p：第一帧走直通，不使用 RGA。
- 640x480 -> 1080p source-change：先立即 RGA，恢复后切直通。
- 1080p -> 640x480 source-change：恢复后的第一帧立即切 RGA。
- source-change storm 不触发 EDID，不泄漏 fd/DMA-BUF，不重用旧 generation。

### 模块测试

只构建和运行 `test_sunshine_rkmpp`，不运行上游 `test_sunshine`。测试重点包括：

- EDID parser/projector/controller；
- HDMI RX event model；
- capture queue recovery；
- RGA/direct route selection；
- MPP import cache generation；
- 会话重连幂等性。

### 实机验收矩阵

1. Xbox 已唤醒且 HDMI 正在输出 4K，Moonlight 请求 1080p。
2. Xbox 已唤醒且 HDMI 正在输出 640x480，Moonlight 请求 1080p。
3. Xbox 完全关闭，Moonlight 先连接，Xbox 延迟唤醒。
4. Xbox 在 EDID 写入前、HPD low 期间和写入后分别开始输出。
5. 同一 1080p Moonlight 会话连续连接/断开至少 20 次。
6. Moonlight 在 720p、1080p、1440p、4K和原生 PC timing 之间切换。
7. HDMI 热拔插及稳定运行时主动切换 Xbox 输出分辨率。
8. EDID readback 不一致、写入失败和 Sunshine 非正常退出后重启。
9. 1080p 直通与 640x480/4K RGA 路径分别长时间运行。
10. 视频恢复时确认 EDID/HPD 不会被重复触发。

每次实机测试必须记录：

- Moonlight 请求尺寸和帧率；
- 选中的原生模式及其 EDID 来源；
- EDID 写入计数和内容哈希；
- 每次 HPD、signal-lost 和 source-change；
- 实际 DV timing；
- 首个真实 capture frame 时间；
- 首个真实 encoded frame 时间；
- 直通/RGA 路由；
- placeholder 和 captured frame 计数；
- HDMI 音频 present/rate 和恢复时间。

## 完成标准

全部条件满足后才允许将该计划标记完成：

1. 同一目标分辨率连续 20 次连接只有首次必要写入，后续 EDID 写入为零。
2. source-change、640x480 和 Xbox wake 永远不会增加 EDID 写入计数。
3. 任意有效 HDMI 输入的首个 dequeue 帧立即进入编码；之后不再出现由协商状态产生的绿帧。
4. 输入与 Moonlight 一致时完全绕过 RGA；不一致时第一帧立即使用 RGA。
5. 所有目标 EDID timing 都能追溯到原生 EDID，不再依赖四个硬编码生产模板。
6. 支持从完整原生模式集合选择最近分辨率，包括 base standard timing。
7. 实机通过已唤醒、延迟唤醒、480p/1080p/4K、热插拔、重连和异常恢复矩阵。
8. `test_sunshine_rkmpp` 中所有相关测试通过，改变代码覆盖率达到 100% 目标。
9. 所有新增或修改的 C++ API、类型、字段和状态均具有完整 Doxygen 文档并通过格式检查。
10. 实现行为写入 `docs/rkmpp/FEATURES.md`，用户设置写入 `docs/rkmpp/README.md`。
11. 实机确认完成后，将仍有长期价值的内容合并到 `FEATURES.md`/`README.md`，然后删除本计划文件。

## 实施纪律

- 不在现有 EDID retry 状态机上继续增加标志、次数、延迟或 Xbox 特例。
- 不以 Xbox Remote Play ready/wake 状态推断 HDMI EDID/DDC 状态。
- 不把 640x480 视为错误状态；它是可以立即捕获和转换的有效 HDMI 输入。
- 不使用重复 `VIDIOC_S_EDID` 作为 DDC 诊断手段。
- 不以单元测试通过替代真实驱动事件模型和实机时间线验收。
- 每个阶段优先运行对应的 `test_sunshine_rkmpp` 定向测试，不运行项目无关的完整测试套件。
