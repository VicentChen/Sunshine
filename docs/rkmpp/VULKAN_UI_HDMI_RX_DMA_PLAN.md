# RKMPP Vulkan UI 与 HDMI RX DMA-BUF 原位合成计划

## 目标

在 Sunshine 的 RKMPP HDMI RX 路径中建立一个通用、可交互的 UI 系统：

- 以固定版本的 Dear ImGui 和官方 Vulkan renderer backend 建立 UI，而不是继续扩展临时 5x7 位图绘制器。
- 先按住 `Start`，再点按 `Back/Select` 立即打开或关闭 UI。
- UI 打开后，手柄导航输入由 UI 截获，不再发送给远端主机。
- UI 支持焦点、高亮、选择、返回和状态反馈。
- UI 操作通过明确的 action 接口反映到 Sunshine，而不是直接耦合到绘制代码。
- 从 Moonlight 会话的首个绿色 placeholder 帧开始自动显示连接状态；只有视频链路和
  当前应用选择的手柄输出链路都已就绪，连接状态才自动隐藏。
- 正式 UI 首个版本提供“连接状态”、“Profile”和“退出 UI”三个入口。
- Profile HUD 迁移为 UI 系统中的一个页面，不再作为独立的固定 OSD 实现。
- UI 以 Moonlight 编码输出尺寸为缩放基准，在 1080p 与 4K 下保持一致的相对占屏比例和
  可读字号；不再把所有会话固定为 `960x180`、默认 ImGui 字体。
- 除 Profile 外，主菜单、连接状态以及后续选项页统一采用从上到下的单列布局；Profile
  保留当前 Timeline、execution lane 和指标网格的信息结构。
- UI 使用 Vulkan 在 GPU 上生成不透明 BGR 内容。BGR888 直通时由 Vulkan 将 ROI 直接写入当前 HDMI RX DMA-BUF；只有视频本身已进入 NV12 RGA fallback 时，才由 RGA 覆盖转换后的目标。

本计划不要求半透明效果。生产路径不得为 alpha blend 读取目标视频区域，也不得复制整帧 HDMI 图像。

## 已验证前置条件

独立工程 `~/Documents/Projects/ProvingGround` 已在 ROCK 5B+ 上完成 Gate 1–3：

1. **Gate 1 — Vulkan hardware：PASS**
   - 设备：`Mali-G610`。
   - Vulkan API：1.3.276。
   - `VK_KHR_external_memory_fd`、`VK_EXT_external_memory_dma_buf` 和
     `VK_EXT_image_drm_format_modifier` 均存在。
2. **Gate 2 — Vulkan 写共享 DMA-BUF：PASS**
   - 当前 Mali 驱动支持导入外部分配的 DMA-BUF，不支持从 Vulkan 导出 buffer memory。
   - 已验证 DMA heap 分配、Vulkan 导入、GPU `image -> buffer` 写入同一 DMA-BUF。
3. **Gate 3 — RGA 消费 Vulkan 结果：PASS**
   - RGA 成功导入同一 DMA-BUF，将不透明 RGBA 测试图转换成 NV12。
   - BT.709 limited 红色采样为 `Y=62 U=102 V=240`，验证通过。

当前驱动不支持线性 RGBA Vulkan Image 直接导出。因此共享内存的所有权方向必须是：

```text
DMA heap 分配 -> Vulkan 导入并写入 -> RGA 导入并读取
```

## 当前 Sunshine 数据路径

HDMI RX 当前使用 V4L2 multi-planar capture：

```text
VIDIOC_REQBUFS(V4L2_MEMORY_MMAP)
  -> VIDIOC_EXPBUF 导出每个 capture slot 的 DMA-BUF FD
  -> VIDIOC_DQBUF 取得一帧
  -> MPP_BUFFER_TYPE_EXT_DMA 导入同一 FD
  -> MPP 同步消费完成
  -> captured_frame_t 释放 lease
  -> VIDIOC_QBUF 将 slot 归还 HDMI RX
```

实机当前格式为单平面 `3840x2160 NV12`，stride 为 3840，allocation size 为
12,441,600 字节，色彩空间为 Rec.709 limited range。

`captured_frame_t` 已经提供必要的 buffer lease：帧从 `DQBUF` 返回后保持为用户态所有，
直到 MPP 同步消费完成才允许 `QBUF`。原位合成必须发生在这个 lease 内。

## 目标数据路径

```text
UI state dirty
  -> Vulkan 渲染不透明 BGR panel image（缓存，非每帧重绘）

HDMI RX BGR888 直通 VIDIOC_DQBUF
  -> Vulkan 导入当前 capture slot 为 buffer
  -> vkCmdCopyImageToBuffer 写入目标 ROI
  -> MPP 编码同一个 HDMI RX DMA-BUF
  -> VIDIOC_QBUF

视频 RGA fallback
  -> Vulkan 按 revision 发布缓存 BGR panel DMA-BUF
  -> RGA: BGR panel -> NV12 fallback target 的目标 ROI
  -> MPP 编码同一个 NV12 target
```

约束：

- 不得在 capture slot 仍处于 `QBUF` 状态时写入。
- Vulkan 直写与 RGA fallback 都必须同步完成，或显式等待 fence 后才能调用 MPP。
- MPP 完成输入消费前不得释放 frame holder 或归还 V4L2 slot。
- UI 隐藏时不得执行 Vulkan UI 更新、Vulkan ROI copy 或 RGA UI 合成。
- UI 可见但状态未变化时，复用缓存的 panel image；每个视频帧只执行一次 ROI 覆盖。
- UI 为完全不透明区域；直通路径不读取目标像素，fallback 中 RGA 只做颜色转换、裁剪和目标覆盖，不启用 blending。
- NV12 目标坐标和尺寸至少满足 4:2:0 偶数对齐，并同时满足实机 RGA 限制。

## Gate 4：真实 HDMI RX DMA-BUF 原位覆盖

Gate 4 只验证真实 capture buffer，不引入正式 UI。

### 当前实施状态（2026-09-01）

- 已加入唯一的 `vulkan_ui` 总控开关，默认启用；关闭后跳过全部 Vulkan UI 路径。
- 已支持外部分配并导入 RGA 的 RGBA8888 DMA-BUF，以及显式 BT.709 limited
  `RGBA -> NV12 ROI` 同步处理。
- 直通编码会在 MPP 提交前覆盖仍由 `captured_frame_t` lease 持有的同一 HDMI RX FD；
  capture generation 变化时清空旧 RGA import cache。
- 初始化、布局校验或 RGA 失败会关闭本会话的 overlay，并继续基本编码路径。
- Moonlight 4K 实机画面已确认底部居中的不透明测试区域位置正确，区域外画面正常。
- 运行日志确认同一 capture generation 的 4 个 slot 全部轮转导入；一次连续会话同步合成
  2,462 帧并正常释放，未出现 RGA import、MPP timeout 或 V4L2 queue starvation。
- Gate 4 状态为 **PASS**。该结论只覆盖真实 HDMI RX DMA-BUF 原位合成，不替代后续
  Vulkan 页面、输入截获和 action 的独立验收。

### 工作

1. 从已 `DQBUF` 的 `captured_frame_t` 取得当前 plane 的 DMA-BUF FD、stride、
   data offset、bytes used 和 allocation size。
2. 创建一个小尺寸、不透明、颜色明确的 RGBA 测试图 DMA-BUF。
3. 使用 RGA 将测试图转换并写入 HDMI RX NV12 buffer 的一个对齐 ROI。
4. RGA 同步完成后，将完全相同的 HDMI RX FD 交给现有 RKMPP encoder。
5. MPP 返回完整 access unit 后，才允许 holder 释放并触发 `VIDIOC_QBUF`。
6. 测试模式下可对 ROI 和 ROI 外少量采样点做诊断性 readback；该 readback 不进入生产路径。
7. 通过 Moonlight 观察编码结果，确认覆盖位置、颜色、稳定性和帧连续性。

### 验收

- RGA 能以 HDMI RX `VIDIOC_EXPBUF` FD 作为 NV12 destination。
- Moonlight 中能看到位置和尺寸正确的不透明测试区域。
- ROI 外画面保持正常，没有整帧复制、闪烁、撕裂或色彩平面错位。
- 测试区域不会出现在下一次复用该 capture slot 的错误位置或错误帧中。
- RGA 返回前不会提交 MPP；MPP 消费完成前不会 `QBUF`。
- 连续运行期间没有 V4L2 queue starvation、RGA import 失败或 MPP 输入超时。
- Gate 4 未通过前，不宣称真实 HDMI RX 原位合成可用于正式 UI。

## 阶段 5：Vulkan UI 渲染后端

### 当前实施状态（2026-08-31）

- 已加入独立的 `vulkan_ui` render model 与长生命周期 Vulkan renderer；renderer 持有
  instance、所选硬件 device、queue、command pool、command buffer 和 fence。
- 原先用于 Gate 5 的手写 5x7 位图页面仅证明过 Vulkan/RGA 后端；它不再是后续 UI 的实现基础。
- **本次执行顺序调整：先接入 Dear ImGui，再实现 UI 状态、Sunshine action 和手柄路由。**
  `third-party/imgui` 已固定为本仓库子模块；仅编译 ImGui core 与上游
  `imgui_impl_vulkan`，渲染到离屏 BGR image，再按当前路径 copy 到 capture DMA-BUF 或已分配的 panel DMA-BUF。
  不引入 GLFW、SDL、swapchain 或第二个窗口系统；Moonlight 手柄输入由 Sunshine input
  ingress 交给独立 UI controller，renderer 只消费其 `focus/revision` 快照。
- ImGui 初始页面保持只读。chord、owner、截获与 neutral cleanup 已有单元测试，但当前
  controller 只改变三项焦点并产生 confirm/back 事件，尚未把 action 绑定到页面控件。
- NV12 fallback 使用的 960x180 BGR DMA-BUF 由 CMA allocator 外部分配，Vulkan 只导入重复的 FD；BGR888 直通路径则按 capture generation 和 slot 缓存 Vulkan buffer import，并把 ROI 直接写入当前 leased capture DMA-BUF。
- ImGui 初始诊断页面是底部横向栏，包含不透明背景、标题和三个从左到右排列的
  只读状态项；相同 revision 直接复用缓存，不提交 Vulkan 工作。它只用于打通
  ImGui -> Vulkan -> DMA-BUF 后端，不在这个阶段接收手柄输入或执行 action。
- 当前实机可见的三列诊断页已经由 Dear ImGui 生成 draw data 并通过官方 Vulkan backend
  渲染；它仍不是包含设置项和 action 的完整 ImGui UI，不能用阶段 5 PASS 推导完整 UI 已完成。
- Vulkan fence 完成并将 buffer ownership 释放给 external queue family 后，RGA 才能读取；
  生产路径不 mmap 或读取 UI 像素。
- 首轮实机发现两个问题：RGBA -> NV12 的 BT.709 mode 被错误并入 `improcess()` usage，
  其数值与 rotate/flip 位重叠，导致面板旋转压缩；首个会话走 RGA 转换路径时也未执行
  UI 覆盖。现已改为在 librga source/destination buffer 上配置色域，并让直通与 RGA
  回退复用同一个 Vulkan 缓存和 RGA backend/allocator；Vulkan 保持正常的 top-origin 坐标。
- 新增 model 边界、opacity 和横向静态布局测试；一次性硬件诊断确认 Vulkan RGBA 原图与
  RGA `RGBA -> NV12 -> RGBA` 回读方向一致，文字保持正向；RKMPP 专用测试 188/188 通过。
- 修复二进制已部署并通过真实 Moonlight 画面复验：首个会话即显示，页面方向、文字与
  横向布局正常；运行日志确认走 RGA fallback target 后完成同步合成。阶段 5 状态为 **PASS**。
- 2026-09-02 修复 BGR888 直通会话误按 NV12 校验并提前关闭 overlay 的问题。Mali-G610
  已验证支持 `VK_FORMAT_B8G8R8_UNORM` color attachment/transfer source；Vulkan 成功导入
  1920x1080、stride 5760 的真实 capture slot 并直接完成 ROI cover，直通 UI 不再经过 RGA。

### 资源模型

- 建立长生命周期 Vulkan instance、device、queue 和 command pool，禁止每帧创建。
- UI surface 按 panel 实际尺寸分配，不分配全屏 UI surface。
- DMA-BUF 由外部 allocator 创建后导入 Vulkan；不得依赖当前驱动不支持的 Vulkan export。
- Vulkan 使用 optimal-tiled BGR render image；直通时由 GPU copy 到导入的 capture buffer ROI，fallback 时才发布到共享 panel buffer 供 RGA 读取。
- UI DMA-BUF 的 row pitch、allocation size 和 FD 生命周期必须显式记录。
- UI 更新与 RGA 读取串行化，或使用明确的 fence；不得覆盖仍由 RGA 使用的缓存。

### 绘制能力

首个版本仅需要：

- 不透明矩形背景。
- 文本或位图字体。
- 垂直/水平菜单布局。
- 当前焦点高亮。
- 开关、枚举值、动作按钮和只读状态文本。
- scissor 限制在 panel 和 dirty region 内。

不在首个版本中实现阴影、模糊、半透明、动画框架或任意窗口叠加。

### 验收

- UI 状态不变时不重新提交 Vulkan 绘制。
- dirty 更新产生的新 UI 能在下一次合成前完成并对 RGA 可见。
- UI surface 尺寸随布局变化，但始终小于等于目标视频范围。
- 生产路径不 mmap 或读取 UI 像素进行软件合成。
- Vulkan 初始化或运行失败时能关闭 UI 后端，不影响基本 HDMI RX 编码路径。

## 阶段 6：通用 UI 模型与手柄路由

### 当前实施状态（2026-09-01）

- 已加入与 Vulkan 无关、线程安全的 `ui_controller`：以 Sunshine 全局 gamepad slot
  标识 owner，分别保存每个手柄的 chord、按键 edge 和摇杆 hysteresis 状态，避免多客户端
  都使用 `controllerNumber=0` 时发生 owner 冲突。
- 隐藏状态下先按 `Start` 会立即 neutralize 并暂存为 UI 修饰键，再按 `Back/Select` 即刻打开；
  未组成快捷键时在 Start 松开后向当前输出补发一个普通点击。触发后直到组合键完全释放才解除
  release gate。UI 可见时所有手柄输入均被消费，只有
  owner 可以通过 D-pad 或左摇杆改变焦点，A/Back 产生 confirm/back 事件。
- UI 打开或关闭时会取消当前 stream 的 Back 长按计时器并 neutralize 已分配的手柄；owner
  断开、stream reset 或 gamepad free 会关闭 UI 并清理路由状态。
- renderer 现在消费 controller 发布的 `visible/focus/revision` 快照。UI 初始隐藏；隐藏时不
  提交 Vulkan render，也不执行 RGA panel ROI 覆盖；焦点变化才提高 revision 并重绘缓存。
  旧 5x7 glyph、rectangle 列表和无效静态布局模型已从 Vulkan UI 后端删除。
- 首次实机运行发现 Moonlight 在按键状态不变时不会发送周期性重复包；随后 GDB 实机捕获又确认
  旧方案会先把单独的 Start 转发到 Xbox，导致第二个按键到达前会话已受影响。现改为有顺序的
  Start 修饰键：第一个按键即被截获，Select 到达时立即触发，不再依赖视频 tick 或保持期限。
- EDID 与缩放策略以 `SPEC.md` 为准：只有 HDMI 输入尺寸已经与 Moonlight 请求尺寸一致时
  才跳过 EDID 和 RGA；任何尺寸不匹配都先从 base DTD、CTA Video Data Block 与 YCbCr
  4:2:0 Video Data Block 中选择尺寸
  最接近且能精确生成的模式，写入 EDID 后请求 HDMI 链路重新协商。上游不接受或最终 timing
  仍不匹配时，RGA 才作为硬件 fallback。
- 受限 EDID 保留真实接收器身份、音频、HDMI VSDB 与 HDMI Forum VSDB，同时过滤普通 VDB 与
  Y420 VDB 的非目标 VIC，并删除重排后失效的 Y420 capability map。RK3588 的
  `VIDIOC_S_EDID` 已自行执行 HPD 周期，不再追加会打断该周期的 `RK_HDMIRX_CMD_SOFT_RESET`。
- 协商器恢复原 EDID 后由驱动同样请求链路重新协商。编码会话不再根据首个旧 timing 固定为 RGA：
  source change 后一旦观察到目标尺寸，会重建直通编码器并释放 RGA fallback。因此
  `1080p + 1080p` 与 `4K + 4K` 都不得因会话初始状态继续执行视频缩放。
- 断流时还复现过 PulseAudio `cleanup_time_events()` 断言。普通 `pa_mainloop` 只能由一个线程
  访问，现已改用 `pa_threaded_mainloop` 并锁住 context operation 的创建、释放和断开流程。
- 本轮最终二进制已在 Moonlight 6.1.0 实机验证：1080p 会话锁定并编码
  `1920x1080p59.94`，4K 会话锁定并编码 `3840x2160p59.94`；两者的稳定 profile window
  均只有 `rga_bypass` 计数而没有视频 `RGA` 阶段。EDID/模式选择/协商聚焦测试
  **18/18 PASS**，完整 RKMPP 专用测试 **205/205 PASS**；RGA DMA-BUF smoke 完成 3 轮且
  FD 保持 `5 -> 5`。三项退出码均为 0。
- controller 的有序 chord/release gate、Start 普通点击补发、modal owner、导航、摇杆 hysteresis、
  disconnect 与 reset 共 7 项测试已覆盖。阶段 6 尚未标记为完整实机 PASS：1080p/4K 直通
  与断流已验证，但仍需完成音频以及 UI 打开、导航、截获、关闭、恢复的整套验收后再更新结论。
- 初步实机操作已确认手柄方向输入能够移动 UI 焦点。当前页面仍没有可见的“退出 UI”选项，
  也可再次使用有序组合键关闭；这部分必须在正式页面模型中补齐后再做完整验收。

将 UI 状态、输入和绘制分成独立层：

```text
Sunshine controller event
  -> chord detector / input router
  -> UI navigation event
  -> UI state + Sunshine action
  -> render model
  -> Vulkan backend
```

### 输入规则

1. 先按住 `Start`，再点按 `Back/Select`，第二个按键到达时立即触发打开或关闭。
2. Start 修饰键从第一个数据包起即被截获；未组成快捷键时在松开后补发普通 Start 点击。
3. 触发后等待组合键完全释放，避免 `Start` 或 `Back` 泄漏到 Sunshine/Xbox。
4. UI 关闭时，除组合键检测外，手柄事件维持现有 Sunshine 路径。
5. UI 打开时，方向键/左摇杆、确认、返回等导航事件由 UI 消费。
6. UI 不识别的输入默认也不向远端透传，除非某个页面明确声明 passthrough。
7. 打开 UI 的手柄成为当前 owner；其他手柄的处理策略必须明确并测试。
8. UI 关闭、stream teardown 或输入设备断开时，发送必要的 neutral cleanup，避免远端残留按键状态。

### UI 模型

- 页面、控件、焦点顺序和 action 使用与 Vulkan 无关的数据结构。
- UI 有两个相互独立的显示来源：连接未完成时自动显示的非模态连接状态，以及由手柄组合键
  打开的模态页面。自动连接状态不得取得 modal owner，也不得截获或改变手柄输入。
- 正式首个版本的主菜单固定包含“连接状态”、“Profile”和“退出 UI”。“退出 UI”收到确认后
  关闭模态页面并执行必要的 neutral cleanup；`Back` 可作为页面返回或关闭 UI 的快捷操作。
- renderer 只消费 render model，不直接修改 Sunshine 配置或 encoder 状态。
- action 通过线程安全命令队列进入对应 Sunshine owner thread。
- 每个 action 返回成功、失败或 pending 状态，UI 显示实际结果。
- 不允许仅更新 UI 显示而未执行 Sunshine 操作。

### 连接状态模型

- 连接状态必须从首个绿色 placeholder 帧开始合成，并在连接完成前持续显示；不能等真实
  HDMI RX 帧出现或等用户手动打开 UI 后才绘制。
- “连接完成”是组合条件：HDMI RX 视频状态已进入 `streaming_direct` 或 `streaming_rga`，并且
  当前应用选择的手柄输出链路已进入可接收输入的 ready 状态。Xbox Remote Play 模式使用
  已有的 sanitized lifecycle snapshot；renderer 不直接查询或持有 Xbox worker。
- 视频已就绪但手柄仍在 authentication、discovery、wake、provisioning、signaling、transport、
  handshake 或 reconnect 时，连接状态必须继续显示。手柄先就绪但视频仍在 `starting`、
  `no_signal`、`negotiating` 或 `source_change` 时也必须继续显示。
- 两个条件首次同时满足后自动隐藏连接状态，不要求用户按键。连接完成后任一链路再次失去
  ready，连接状态应自动重新出现，并持续到两者再次就绪。
- 自动连接状态至少显示视频状态、手柄状态、Moonlight 目标分辨率和当前 HDMI 输入分辨率；
  失败信息只使用脱敏的固定状态、stage 和 failure kind，不显示凭据或设备标识。
- 用户从主菜单打开“连接状态”页面时，即使连接已经完成也保持显示，直到用户返回或退出 UI；
  自动隐藏规则只作用于非模态连接状态。
- 状态变化才增加 render revision。绿色 placeholder 期间可以每帧覆盖缓存面板，但不得每帧
  重新提交 Vulkan 绘制。

### 验收

- Start 单键不会立即转发；Start 后接 Select 会立即打开 UI，Start 单独松开会补发普通点击。
- UI 打开期间导航稳定，高亮与焦点一致，按键不会到达 Xbox/远端应用。
- 主菜单中的“退出 UI”可通过确认键关闭 UI，关闭后不需要再次输入组合键。
- UI 关闭后输入透传恢复，没有 stuck button。
- 自动连接状态不截获手柄；它只按视频与手柄 ready 条件自动显示、隐藏和重新出现。
- action 的显示状态与 Sunshine 实际状态一致。
- UI renderer 可替换，不影响输入和 action 单元测试。

## 阶段 7：连接状态、Sunshine action 与 Profile HUD 迁移

### 当前实施状态（2026-09-01）

- 正式主菜单已经替换阶段 5 的三列诊断页，固定提供“连接状态”、“Profile”和“退出 UI”三个
  入口；连接状态与 Profile 已能进入独立只读页面，Back 返回主菜单。
- “退出 UI”确认和主菜单 Back 快捷操作都通过显式 `close_modal` action 关闭 modal owner，并
  复用既有 `visibility_changed` 路径 neutralize 所有已分配手柄；无需再次输入组合键。
- HDMI RX 每帧携带脱敏的 state machine、Moonlight 目标尺寸和当前输入尺寸；UI 会合并现有的
  Xbox Remote Play sanitized lifecycle snapshot。只有视频处于 `streaming_direct`/
  `streaming_rga` 且所选手柄链路 ready 时，自动连接状态才隐藏。
- 绿色 placeholder 在 RGA fill 之后、MPP 提交之前执行同一缓存 UI 的 ROI 覆盖，因此首个
  placeholder 帧已经具备显示连接状态的代码路径。自动连接状态不取得 modal owner，也不消费
  或改变手柄输入；用户主动打开的连接状态页不受自动隐藏条件影响。
- renderer 当前显示视频状态、手柄 state/stage/failure kind、Moonlight 目标分辨率和当前 HDMI
  输入分辨率。Xbox lifecycle 最多每 100 ms 轮询一次，状态不变时不增加 render revision。
- Profile 页面现已接入逐帧 Timeline：网络线程在最后一次 `send_batch()` 返回后，将最近 32 个已完成
  captured frame 以固定容量 ring 发布。每个 concrete span 同时保留相对该帧 `RX EOF` 的 start/end，
  每帧还保留相对当前 stream epoch 的 origin，因此既能显示单帧内部阶段位置，也不会丢失后续多帧
  在途时的跨帧重叠关系。Timeline 使用 Capture、RGA、Vulkan UI、MPP 和 Network 五条稳定 execution
  lane，当前覆盖 RX EOF-DQ、Capture queue、RGA、UI render、UI compose、MPP import/output
  preparation/submit/wait、Encoded queue 和 Packetize/send；missing/invalid stage 使用 bit mask 保留，
  不会伪装成零耗时条。
- 原 5 秒 completed-window snapshot、P50/P95/P99、sample/overflow、placeholder/repeated/captured、
  RGA bypass 和旧 MPP OSD 回退路径继续保留，不与逐帧 Timeline 混合聚合。Vulkan UI 最多每 100 ms
  读取并重绘一次 Timeline revision，数据仍逐个完成帧采集；Profile 页面不可见时不会因 Timeline
  generation 单独增加 render revision。直通帧上的 UI RGA 覆盖单独记录为 `UI COMPOSE`，不再将其
  计入视频转换 `RGA`，变化页面的同步 Vulkan 提交则单独记录为 `UI RENDER`。
- Timeline/Profile/UI controller 定向测试 **29/29 PASS**，完整 RKMPP 专用测试 **218/218 PASS**；
  `scripts/build-rkmpp.sh` prepared-cache 构建完成，`sunshine` 及全部模块测试目标成功链接。
- Timeline 变更前的阶段 7 构建已部署到 ROCK 5B+ 并由 PID 53673 运行；进程 `/proc` 映像与磁盘产物 SHA256
  均为 `4f8bbb978563b8af98c40f494fe71dade568cf8048225bd6e4063ff0395e872c`，Moonlight 客户端
  已完成真实 Xbox 会话验收。首个绿色 placeholder 帧可见底部连接状态；Xbox lifecycle 从
  authentication/discovery/wake/provisioning 变化时页面持续显示并只按状态变化重绘；视频与
  手柄同时 ready 后页面自动隐藏；随后数据通道进入 `failed/data_channel/retryable` 时页面在
  流不中断的情况下自动重新出现。上述自动显示、隐藏和掉线重现均为 **PASS**。
- 新 Timeline 代码目前只完成源码、模块测试与构建验证，尚未替换运行中进程；真实 Timeline 画面和
  性能数据必须在单独获得部署/重启授权后验收，不能沿用上一构建的实机结论。
- 真实手柄的组合键打开、主菜单/子页面导航、“退出 UI”、输入截获与恢复仍需完成本轮实机验收；
  macOS Computer Use 可以操作 Moonlight 窗口和键盘，但不能合成游戏手柄事件。Profile Timeline
  的真实 Vulkan/RGA 画面布局、10 Hz 更新节奏和相对时间可读性也依赖该组合键进入，因此阶段 7
  尚未整体标记为 PASS。

1. 建立与 Vulkan 无关的连接 snapshot，将 HDMI RX state machine 与当前手柄输出后端的
   sanitized lifecycle 状态汇总成明确的 video ready、gamepad ready 和整体完成条件。
2. 在绿色 placeholder 的 RGA fill 完成后、MPP 编码前合成自动连接状态；真实帧路径复用同一
   render model 和缓存，并按组合完成条件自动隐藏或重新出现。
3. 建立包含“连接状态”、“Profile”和“退出 UI”的正式页面导航；退出 action 关闭模态 UI
   并完成输入清理，不修改视频或手柄连接本身。
4. Profile 页面同时消费逐帧固定容量 Timeline 与现有 completed-window snapshot；Timeline 保留每帧
   公共 epoch origin、各阶段相对 RX EOF 的 start/end 和 execution lane，统计路径继续保留
   P50/P95/P99、sample/overflow、placeholder/repeated/captured 和 RGA bypass 等既有语义。
5. 先接入退出 UI 这类低风险、可逆 action，验证 UI 到 Sunshine controller owner 的命令边界；
   对需要重建 encoder、切换输入或断开 session 的后续 action，明确展示确认和执行结果。
6. 删除旧固定 Profile HUD 对 640x176 palette bitmap 的 UI 职责；迁移验收完成前，旧 MPP OSD
   暂时作为回退后端。确认 Vulkan UI 后端稳定后，再单独计划移除旧实现。

## 阶段 8：分辨率自适应、可读性与垂直布局

### 当前实施状态（2026-09-02）

- 已加入纯 CPU `layout_metrics`，以 Moonlight 编码输出为唯一缩放基准；1080p 精确生成普通页
  `1280x720`、Profile `1440x360`、正文 28 px、标题 36 px，4K 对应全部线性尺寸 2 倍。
  720p 与超宽输出使用限制轴缩放，正文低于 18 px 的输出会在创建 UI 前明确拒绝。
- UI session 现在按编码配置一次性建立普通页与 Profile 两类稳定 BGR/Vulkan surface；HDMI input
  timing 或 source change 不参与尺寸选择。两类 surface 共用同一套 margin、字体和几何 metrics，
  direct-BGR 与 NV12 RGA ROI 都使用当前页面的真实 panel 尺寸。
- ImGui 显式使用内置矢量字体分别生成正文和标题字号，不使用 `FontGlobalScale` 放大 13 px 位图。
  window padding、item spacing、Timeline label/axis/lane 和条形标签阈值均已从固定像素迁移到 metrics。
- 主菜单已按“连接状态 -> Profile -> 退出 UI”改为单列全宽卡片，连接状态四项改为单列 label/value
  行；Up/Down 继续沿 focus index 移动，Left/Right 只报告导航事件、不再改变主菜单 focus。
- Profile 仍保留标题字段、Timeline 五条 execution lane、相对时间条和 4 列 completed-window 指标
  网格；只采用新的宽低 surface、矢量字号、换行和缩放后的几何常量。
- 新增布局与导航测试后，`test_sunshine_rkmpp` 为 **223/223 PASS**；`git diff --check` 通过，
  `scripts/build-rkmpp.sh` prepared-cache 构建成功链接 `sunshine`、三个模块测试目标和 Xbox probe，
  最终退出码为 0。
- 已在 ROCK 5B+ 重启到本轮构建；运行中 `/proc/<pid>/exe` 与磁盘二进制 SHA-256 均为
  `2482ef47d00f64e053a51ca81c4b99c0d1222672b88217b0100fcb6aff15da2d`，47984/47989/47990/48010
  均由新进程监听。启动 smoke 同时确认 H.264/HEVC Vulkan UI 与首个 DMA-BUF 合成成功。
- 已用 macOS Moonlight 6.1.0 完成 1080p 与 4K 短会话截图验收。1080p 日志为普通页
  `1280x720`、Profile `1440x360`、正文/标题 `28/36`；4K 为 `2560x1440`、`2880x720`、
  `56/72`。同一 Moonlight 窗口中两档连接状态 panel 和文字保持相同视觉比例，连接状态四项按
  `VIDEO -> GAMEPAD -> MOONLIGHT -> HDMI INPUT` 从上到下完整显示，无裁切、重叠或 ROI 越界。
- Xbox Remote Play 实机验收已按完整 lifecycle 等待，而不是在启动初期提前判定：日志实际经过
  authentication、discovery、wake、provisioning、transport 和 handshake，最终进入
  `state=ready stage=ready`。在真实 Xbox 视频和已完成 5 秒统计窗口上，分别取得 4K 与 1080p
  Profile modal 截图；两档均完整显示 FRAME/RX EOF/SEND/SPANS/MISSING/INVALID 标题和 Capture、
  RGA、Vulkan UI、MPP、Network 五条 Timeline lane，视觉占屏比例一致且文字可读。
- 4K completed window 记录 `captured=189`、`dropped_samples=0`；1080p 稳态窗口记录
  `captured=300`、`rga_bypass=300`、`freshness_drops=0`、`dropped_samples=0`。现场无可由 macOS
  Computer Use 合成的物理手柄事件，因此截图使用了仅在临时二进制中存在的一次性 Profile 预览入口；
  该入口仍通过正式 controller policy 执行 Start+Back、Down 和确认导航，不伪造 Profile 数据。
- 两档会话退出后均记录 `CLIENT DISCONNECTED` 与 Vulkan UI teardown。临时预览进程、二进制和源码
  hook 已删除，正式 `src/video.cpp` 和 `build-rkmpp-review/sunshine` 已恢复；Moonlight 已恢复原始 4K
  与关闭“Force gamepad #1 always connected”的设置并退出。真实物理手柄输入透传与扩大 ROI 的相对
  性能门限仍保留为后续实机验收项。

### 当前问题与边界（2026-09-02）

- UI session 目前固定分配 `960x180` BGR panel，1080p 时占画面约 `50% x 16.7%`，4K 时只占
  `25% x 8.3%`，所以同一 UI 在 4K 下会缩小一半。
- ImGui 当前使用默认字体图集，没有按编码分辨率选择实际像素字号；窗口 padding、卡片高度、
  Timeline label width 等也都是固定像素值。
- 主菜单使用 3 列，连接状态使用 4 列，视觉方向与线性 focus 顺序不一致；controller 当前还会让
  上下左右四个方向都循环同一条 focus 链。
- 本阶段只改变 UI panel、字体、布局和相应导航语义，不改变连接完成条件、action、输入 owner、
  neutral cleanup、Profile 数据源、Timeline stage/lane、统计口径或视频缩放策略。
- “Profile 保持现状”指保留当前标题信息、Timeline 五条 execution lane、相对时间含义和统计
  网格结构；Profile 仍采用适合时间线的横向布局，但会使用共同的分辨率缩放和可读字号。

### 自适应尺寸模型

1. 新增与 Vulkan 资源无关的 `ui_layout_metrics`，输入使用 Moonlight 编码输出宽高，而不是当前
   HDMI 输入 timing。所有 panel 尺寸、safe margin、字号、padding、row height 和 Timeline
   几何都由同一份 immutable metrics 产生，renderer 与 ROI copy 不各自重复计算。
2. 以 `1920x1080` 为设计基准，缩放因子取
   `min(output_width / 1920, output_height / 1080)`；非 16:9 输出再受可用宽高、安全边距和
   BGR/NV12 对齐约束限制，最终尺寸向下做必要的偶数/字节对齐，禁止越出编码画面。
3. 第一版目标如下；它们是实现和截图验收基线，不使用 `FontGlobalScale` 放大低分辨率字形，
   而是按目标像素大小生成字体图集：

   | Moonlight 输出 | 主菜单/连接状态 panel | Profile panel | 正文字号 | 标题字号 | 外边距 |
   | --- | --- | --- | --- | --- | --- |
   | 1920x1080 | 1280x720 | 1440x360 | 28 px | 36 px | 36 px |
   | 3840x2160 | 2560x1440 | 2880x720 | 56 px | 72 px | 72 px |

   这样 1080p 与 4K 的相对占屏比例一致；相比当前 `960x180`，普通页面有足够高度容纳单列内容，
   Profile 则继续保持宽而低的时间线形态。
4. UI session 应从编码配置取得稳定目标尺寸，或在首次拿到完整 Moonlight 尺寸后延迟创建资源；
   同一 session 的 HDMI source change 不应因为输入 timing 短暂变化而反复重建 UI。若编码输出尺寸
   确实变化，等待当前 Vulkan/RGA/MPP 使用结束后再原子替换 panel、font atlas 和相关 import cache。
5. 对低于设计基准或非标准宽高比使用同一算法缩小，且设置“内容能完整显示”的下限检查；无法满足
   最小 panel/字号时关闭本会话 UI 并记录明确错误，不能裁切、越界或影响基本编码路径。

### 页面布局与导航

1. 建立可复用的垂直 list/card primitive。主菜单固定按“连接状态 -> Profile -> 退出 UI”从上到下
   排列，每项占一整行；focus index 和 action 映射保持 `0/1/2`，避免把视觉重排变成 action 重排。
2. 连接状态按“视频 -> 手柄 -> Moonlight -> HDMI 输入”从上到下排列，每行左侧为状态名，右侧为
   当前值；failure kind 或较长 stage 在行内换行/截断规则必须明确，不允许挤压相邻选项。
3. 普通设置页的默认规则也是单列自上而下；只有页面明确声明为 data visualization 时才允许网格或
   横向布局。Profile 是本阶段唯一例外：保留 Timeline 和 completed-window 指标的现有横向结构。
4. 主菜单导航改为 D-pad/左摇杆“上、下”沿可见顺序移动；左、右在主菜单不改变 focus，避免界面
   已经垂直但导航仍暗示横向。确认、返回、组合键开关、输入截获和退出后的 neutral cleanup 不变。
5. 标题、正文、辅助文字、卡片 padding、行间距和 focus border 使用 metrics 中的语义 token；不得
   在各页面继续散落 `20`、`14`、`74`、`82` 等只适用于 `960x180` 的固定像素常量。

### 分阶段实施与验收

1. **布局模型（纯 CPU）**：实现 metrics 与 1080p/4K/非 16:9 边界计算，先用单元测试确认 panel、
   字号、safe margin、ROI 对齐和越界拒绝，不接触 Vulkan 或手柄状态机。
2. **资源生命周期**：让 BGR panel、Vulkan framebuffer、font atlas、RGA source 和 direct-BGR ROI
   使用同一 metrics；验证一次 session 只按输出尺寸创建，source change 不造成资源抖动。
3. **普通页面垂直化**：迁移主菜单和连接状态，调整 Up/Down 导航语义；确认 focus 高亮、action index、
   自动连接状态非模态行为以及“退出 UI”均与现有逻辑一致。
4. **Profile 兼容适配**：只把固定像素几何替换为 metrics，并保持标题字段、lane 顺序、条形相对位置、
   metric 顺序和数据语义不变；使用同一 snapshot 对比改造前后 Timeline geometry 的归一化结果。
5. **离线验证**：运行 `git diff --check`，构建并只运行 `test_sunshine_rkmpp`；不运行上游
   `test_sunshine`。任何实机画面结论都不能由单元测试或构建成功代替。
6. **实机短验收**：在另行获得部署/重启授权后，分别用 Moonlight 1080p 与 4K 截图验证主菜单、
   连接状态和 Profile；每档完成一次打开、上下导航、进入/返回、退出 UI 与输入恢复，不增加耐久测试。

本阶段完成必须同时满足：

- 1080p 与 4K 截图中，普通页面和 Profile 的相对占屏比例一致，4K 不再显示为 1080p 的一半；
  正文分别使用 28 px 与 56 px 目标字形，标题分别使用 36 px 与 72 px 目标字形。
- 主菜单三个入口和连接状态四项均从上到下排列；Up/Down 与视觉顺序一致，Left/Right 不改变主菜单
  focus，确认和返回仍触发原 action。
- 普通页面在 safe area 内完整显示，没有文字裁切、卡片重叠或 ROI 越界；较长状态值有稳定的换行
  或截断表现。
- Profile 的信息结构、lane/metric 顺序、相对时间和 completed-window 统计语义不变；字体可读，
  Timeline 条形与标签没有裁切或错位。
- UI 隐藏时仍无额外 Vulkan/RGA UI 工作；静态 UI 仍复用缓存；尺寸变化或失败路径不会泄漏
  DMA-BUF/Vulkan/RGA 资源，也不会阻止 V4L2 buffer 归还或基本编码继续。

## 测试与验证

### 离线测试

- 有序 chord 的首键截获、立即触发、普通 Start 补发、释放门和重复触发。
- UI focus、navigation、返回和 action dispatch。
- 连接完成组合条件的全部边界：仅视频 ready、仅手柄 ready、两者 ready 和任一链路重新断开。
- 自动连接状态从 placeholder 开始显示、两者 ready 后隐藏、断线后重新出现，且始终不截获输入。
- 主菜单页面切换和“退出 UI”确认后的关闭与 neutral cleanup。
- 1080p、4K 与至少一个非 16:9 输入的 layout metrics、字体像素尺寸、safe margin、对齐与越界拒绝。
- 主菜单和连接状态的单列顺序，以及 Up/Down 移动、Left/Right 不移动 focus 的边界。
- Profile 改造前后归一化 Timeline geometry、lane/metric 顺序和统计 snapshot 语义保持一致。
- UI 打开/关闭时的 input interception。
- NV12 ROI 的对齐、范围、stride、offset 和 allocation 校验。
- frame holder 在 RGA 与 MPP 完成前保持有效。
- Vulkan/RGA 失败时的回退与 buffer 归还。

按照 `docs/rkmpp/AGENTS.md`：

- 使用 `scripts/build-rkmpp.sh` 构建。
- 只构建和运行 `test_sunshine_rkmpp`。
- 不运行 `test_sunshine`。

### 实机验证

1. Gate 4 单帧测试图。
2. Gate 4 连续帧稳定性与 capture slot 轮转。
3. 首个绿色 placeholder 帧即显示连接状态；视频或手柄任一未 ready 时持续显示，两者 ready 后
   自动隐藏，并在任一链路断开后自动重新出现。
4. 主菜单、“连接状态”页面、“Profile”页面和“退出 UI”的导航、显示与隐藏。
5. 1080p 与 4K 分别截图确认相对尺寸、字号、单列布局和 Profile 兼容性。
6. UI dirty 更新和高频手柄导航。
7. Moonlight -> Sunshine -> Xbox 输入截获、退出 UI 后恢复以及无 stuck button。
8. HDMI source change、无信号、手柄重连、Moonlight 重连和 Sunshine teardown。
9. 记录 UI 隐藏/显示时的 RGA、MPP、端到端延迟以及队列等待，不用单次样本代替稳定性结论。

## 性能验收

- UI 隐藏时，视频路径没有额外 Vulkan submission 或 RGA UI operation。
- UI 静态显示时，Vulkan 不重复渲染；仅对每个编码帧执行 panel ROI 覆盖。
- UI 更新不产生完整 4K RGBA surface 或完整视频帧副本。
- 4K 只按 2 倍线性尺寸扩大 panel，不得因面积扩大而在每个视频帧重建 font atlas、framebuffer
  或 import cache；这些资源只在 session/输出尺寸变化时创建。
- UI 合成不会使 capture queue 长期饥饿或导致 MPP 超时。
- 以关闭 UI 的同配置基线为对照，报告 RGA、MPP 和 host latency 的 p50/p95/p99；性能门限在获得 Gate 4 基线后确定。

## 失败处理与回滚

- Gate 4、Vulkan 初始化或 UI renderer 失败时，关闭新 UI 合成并继续现有 HDMI RX -> MPP 路径。
- 新 UI 只使用唯一的 `vulkan_ui` 总控开关，默认启用；关闭后跳过全部 Vulkan UI 路径。
- 保留现有 MPP OSD/Profile HUD 作为迁移期间回退，不在同一变更中删除。
- 任何异常路径都必须释放 RGA/Vulkan 引用，并保证已 `DQBUF` 的 slot 最终能够安全 `QBUF`。
- 不修改 HDMI RX 驱动配置、GPU 驱动或系统 Vulkan ICD 作为功能运行时的一部分。

## 完成定义

只有同时满足以下条件，才能将本计划中的已实现能力合并到
`docs/rkmpp/FEATURES.md` 并删除本计划：

- Gate 4 在真实 HDMI RX capture buffer 上通过。
- Vulkan UI、RGA 原位覆盖和 MPP 编码链路在 Moonlight 中完成实机验证。
- 手柄打开、导航、截获、关闭和 neutral cleanup 全部通过集成验证。
- 连接状态从首个绿色 placeholder 帧持续显示到视频与手柄同时 ready，并能在连接丢失后
  自动重新出现；连接完成后无需用户操作即可自动隐藏。
- 正式 UI 包含“连接状态”、“Profile”和可用的“退出 UI”入口。
- 1080p/4K UI 的相对尺寸和字号通过截图验收；普通页面为垂直单列，Profile 保持现有信息结构。
- 至少一个 Sunshine action 能执行并将真实结果反馈到 UI。
- Profile HUD 已作为 UI page 工作，旧固定 OSD 的保留或移除状态有明确记录。
- RKMPP 专用测试通过，性能数据和已知限制已记录。
