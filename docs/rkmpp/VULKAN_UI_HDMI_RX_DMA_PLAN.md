# RKMPP Vulkan UI 与 HDMI RX DMA-BUF 原位合成计划

## 目标

在 Sunshine 的 RKMPP HDMI RX 路径中建立一个通用、可交互的 UI 系统：

- 按住 `Back/Select + Start` 3 秒打开或关闭 UI。
- UI 打开后，手柄导航输入由 UI 截获，不再发送给远端主机。
- UI 支持焦点、高亮、选择、返回和状态反馈。
- UI 操作通过明确的 action 接口反映到 Sunshine，而不是直接耦合到绘制代码。
- Profile HUD 迁移为 UI 系统中的一个页面，不再作为独立的固定 OSD 实现。
- UI 使用 Vulkan 在 GPU 上生成不透明 RGBA 内容，再由 RGA 将 UI 直接覆盖到当前 HDMI RX NV12 DMA-BUF 的目标区域，最后交给 MPP 编码。

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
  -> Vulkan 渲染不透明 RGBA panel
  -> UI DMA-BUF（缓存，非每帧重绘）

HDMI RX VIDIOC_DQBUF
  -> RGA: UI RGBA DMA-BUF -> 当前 HDMI RX NV12 DMA-BUF 的目标 ROI
  -> MPP 编码同一个 HDMI RX DMA-BUF
  -> VIDIOC_QBUF
```

约束：

- 不得在 capture slot 仍处于 `QBUF` 状态时写入。
- RGA 必须以同步模式完成，或显式等待 fence 后才能调用 MPP。
- MPP 完成输入消费前不得释放 frame holder 或归还 V4L2 slot。
- UI 隐藏时不得执行 Vulkan UI 更新或 RGA UI 合成。
- UI 可见但状态未变化时，复用缓存的 UI DMA-BUF；每个视频帧只执行一次 ROI 覆盖。
- UI 为完全不透明区域；RGA 只做颜色转换、裁剪和目标覆盖，不启用 blending。
- NV12 目标坐标和尺寸至少满足 4:2:0 偶数对齐，并同时满足实机 RGA 限制。

## Gate 4：真实 HDMI RX DMA-BUF 原位覆盖

Gate 4 只验证真实 capture buffer，不引入正式 UI。

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

### 资源模型

- 建立长生命周期 Vulkan instance、device、queue 和 command pool，禁止每帧创建。
- UI surface 按 panel 实际尺寸分配，不分配全屏 RGBA UI surface。
- DMA-BUF 由外部 allocator 创建后导入 Vulkan；不得依赖当前驱动不支持的 Vulkan export。
- Vulkan 使用 optimal-tiled render image，完成绘制后由 GPU copy 到导入的线性 DMA-BUF buffer。
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

1. `Back/Select + Start` 必须连续保持 3 秒才触发打开或关闭。
2. 触发后等待组合键完全释放，避免 `Start` 或 `Back` 泄漏到 Sunshine/Xbox。
3. UI 关闭时，除组合键检测外，手柄事件维持现有 Sunshine 路径。
4. UI 打开时，方向键/左摇杆、确认、返回等导航事件由 UI 消费。
5. UI 不识别的输入默认也不向远端透传，除非某个页面明确声明 passthrough。
6. 打开 UI 的手柄成为当前 owner；其他手柄的处理策略必须明确并测试。
7. UI 关闭、stream teardown 或输入设备断开时，发送必要的 neutral cleanup，避免远端残留按键状态。

### UI 模型

- 页面、控件、焦点顺序和 action 使用与 Vulkan 无关的数据结构。
- renderer 只消费 render model，不直接修改 Sunshine 配置或 encoder 状态。
- action 通过线程安全命令队列进入对应 Sunshine owner thread。
- 每个 action 返回成功、失败或 pending 状态，UI 显示实际结果。
- 不允许仅更新 UI 显示而未执行 Sunshine 操作。

### 验收

- 组合键不足 3 秒不会打开 UI。
- UI 打开期间导航稳定，高亮与焦点一致，按键不会到达 Xbox/远端应用。
- UI 关闭后输入透传恢复，没有 stuck button。
- action 的显示状态与 Sunshine 实际状态一致。
- UI renderer 可替换，不影响输入和 action 单元测试。

## 阶段 7：Sunshine action 与 Profile HUD 迁移

1. 先接入只读状态和低风险、可逆 action，验证 UI 到 Sunshine 的命令边界。
2. 对需要重建 encoder、切换输入或断开 session 的 action，明确展示确认和执行结果。
3. 将 Profile HUD 数据源转换为一个只读 UI page。
4. 删除 Profile HUD 对固定 640x176 palette bitmap 的 UI 职责；旧 MPP OSD 可暂时作为回退后端。
5. 确认 Vulkan UI 后端稳定后，再单独计划移除旧的固定 OSD 实现。

## 测试与验证

### 离线测试

- chord 的 3 秒边界、释放门和重复触发。
- UI focus、navigation、返回和 action dispatch。
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
3. UI 静态页面显示与隐藏。
4. UI dirty 更新和高频手柄导航。
5. Moonlight -> Sunshine -> Xbox 输入截获与恢复。
6. HDMI source change、无信号、Moonlight 重连和 Sunshine teardown。
7. 记录 UI 隐藏/显示时的 RGA、MPP、端到端延迟以及队列等待，不用单次样本代替稳定性结论。

## 性能验收

- UI 隐藏时，视频路径没有额外 Vulkan submission 或 RGA UI operation。
- UI 静态显示时，Vulkan 不重复渲染；仅对每个编码帧执行 panel ROI 覆盖。
- UI 更新不产生完整 4K RGBA surface 或完整视频帧副本。
- UI 合成不会使 capture queue 长期饥饿或导致 MPP 超时。
- 以关闭 UI 的同配置基线为对照，报告 RGA、MPP 和 host latency 的 p50/p95/p99；性能门限在获得 Gate 4 基线后确定。

## 失败处理与回滚

- Gate 4、Vulkan 初始化或 UI renderer 失败时，关闭新 UI 合成并继续现有 HDMI RX -> MPP 路径。
- 新 UI 使用独立运行时开关，首个版本默认关闭。
- 保留现有 MPP OSD/Profile HUD 作为迁移期间回退，不在同一变更中删除。
- 任何异常路径都必须释放 RGA/Vulkan 引用，并保证已 `DQBUF` 的 slot 最终能够安全 `QBUF`。
- 不修改 HDMI RX 驱动配置、GPU 驱动或系统 Vulkan ICD 作为功能运行时的一部分。

## 完成定义

只有同时满足以下条件，才能将本计划中的已实现能力合并到
`docs/rkmpp/FEATURES.md` 并删除本计划：

- Gate 4 在真实 HDMI RX capture buffer 上通过。
- Vulkan UI、RGA 原位覆盖和 MPP 编码链路在 Moonlight 中完成实机验证。
- 手柄打开、导航、截获、关闭和 neutral cleanup 全部通过集成验证。
- 至少一个 Sunshine action 能执行并将真实结果反馈到 UI。
- Profile HUD 已作为 UI page 工作，旧固定 OSD 的保留或移除状态有明确记录。
- RKMPP 专用测试通过，性能数据和已知限制已记录。
