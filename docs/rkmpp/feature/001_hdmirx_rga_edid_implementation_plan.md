# 已归档：HDMI RX 分辨率适配、RGA 缩放与 EDID 协商实施计划

> 状态：已归档 — 本计划的软件功能均已完成；720p、1080p 与 4K RGA 串流已完成实机验证。EDID 实机回滚验证保留为可选验收记录，不构成功能待办。
>
> 目标平台：ROCK 5B+ / RK3588 / Linux aarch64
>
> 基线功能：`capture = hdmirx` 与 `encoder = rkmpp`
>
> 本计划供多个 Agent 按阶段顺序执行。每个 Agent 只承担一个阶段，必须先确认前序阶段的验收记录，完成代码、测试、文档和交接记录后再结束工作。

## 1. 背景与问题定义

当前 HDMI RX 路径把 V4L2 返回的输入尺寸直接作为 RKMPP 编码尺寸，并要求它与 Moonlight 请求的传输尺寸完全一致。只要两者不同，HDMI RX display 创建就会失败，当前串流随之结束。

需要把以下三个尺寸概念分离：

- `D0`：Moonlight 连接之前，HDMI 信号源当前输出、HDMI RX 当前锁定的默认输入分辨率。
- `T`：Moonlight 建立会话时请求的传输分辨率，即 Sunshine 必须编码并向客户端发送的最终尺寸。
- `I`：本次会话中 HDMI RX 实际锁定的输入分辨率。尝试 EDID 协商后它可能改变，也可能仍等于 `D0`。

目标链路：

```text
HDMI 信号源
  → HDMI RX 实际输入 I
  → I == T：直接 DMA-BUF 输入 RKMPP
  → I != T：RGA 缩放/颜色转换到 T 的 DMA-BUF，再输入 RKMPP
  → H.264/H.265，编码尺寸始终为 T
  → Sunshine 现有 RTP/FEC/加密/UDP 链路
  → Moonlight
```

EDID 协商是输入质量和效率优化，不是串流成功的前置条件。无论 EDID 接口不存在、写入失败、上游忽略 EDID，还是最终输入模式不符合预期，都必须以实际检测到的 `I` 为准，并回退到 RGA 路径。不能因为 `I != T` 断开 Moonlight。

## 2. 已确认的产品决策

### 2.1 实施顺序

先实现 RGA 分辨率适配，再实现 EDID 能力探测与可选协商。EDID 阶段不得反过来成为 RGA 阶段的阻塞项。

### 2.2 输出尺寸

- RKMPP 输出的 coded width/height 必须精确等于 Moonlight 请求的 `T`。
- `I == T` 时保留现有 HDMI RX → RKMPP 直接 DMA-BUF 路径。
- `I != T` 时使用 RGA；允许下采样，也允许在全部 HDMI 输入模式都小于 `T` 时上采样。
- 缩放默认保持原始宽高比，居中显示并填充黑边，不拉伸画面。
- RGA 路径允许发生一次由硬件完成的图像写入，但不得增加 CPU 像素复制或软件缩放。

### 2.3 HDMI 模式选择

若运行环境支持可恢复的 EDID 读写，按以下规则选择准备向上游请求的 HDMI 模式 `M`：

1. 候选模式必须来自 Sunshine 准备写入的、经过验证的 EDID 模式集合，不能把 V4L2 pixel format 枚举误当成 HDMI 模式集合。
2. 先筛选 `width >= T.width && height >= T.height` 的模式。
3. 若筛选结果非空，优先选择像素面积最小者；面积相同时，优先选择宽高超出量更小、宽高比更接近 `T`、刷新率更接近 Moonlight 请求值的模式。
4. 若没有同时满足宽高要求的模式，选择候选集合中像素面积最大的模式；相同面积时使用相同的宽高比和刷新率规则。
5. `M` 只是请求结果。最终链路必须重新读取 active timings 得到 `I`，不能假定 `I == M`。

第一版只以分辨率为硬约束；刷新率是 tie-breaker，不因上游未采用期望刷新率而断流。

### 2.4 EDID 安全与回退

- 只有同时支持读取原 EDID、写入测试 EDID并能在会话结束时恢复原 EDID，才把设备视为“可协商”。
- 写入前保存原始 EDID 的完整字节及块数。
- 会话结束、初始化失败和异常退出清理路径都必须尽力恢复原 EDID。
- 写入成功不等于协商成功；必须等待 HDMI RX 重新锁定，并以 `VIDIOC_QUERY_DV_TIMINGS` 与稳定帧验证实际输入。
- 任意 EDID 操作失败、超时或得到意外模式时，记录明确日志并继续使用实际 `I` + RGA。
- 不支持 EDID 的驱动属于正常兼容场景，不记录为致命错误。

### 2.5 协商期间画面

- Moonlight 已连接但 HDMI 输入正在切换、暂时无信号或尚未稳定时，持续提供尺寸为 `T` 的纯绿色占位帧。
- 占位帧由 RGA/硬件 buffer 路径生成并交给 RKMPP，不从旧捕获帧复制，也不能泄露上一段 HDMI 内容。
- 进入占位状态后的第一帧和恢复真实画面后的第一帧都请求 IDR。
- 第一版纯绿色只表示“输入暂不可用”。叠加文字、错误原因、进度和本地化 UI 属于 future work，记录在本文末尾。

## 3. 范围与非目标

### 3.1 本次范围

- RGA 构建探测、格式能力探测和运行时错误报告。
- V4L2 DMA-BUF → RGA → DMA-BUF → RKMPP 的硬件处理链路。
- Moonlight 任意请求尺寸与 HDMI RX 实际输入尺寸解耦。
- 保持比例的缩放和黑边填充。
- 纯绿色协商/无信号占位帧。
- HDMI RX EDID 读写能力探测、原 EDID 保存和恢复。
- 模式选择、协商超时、实际 timing 验证与 RGA 回退。
- source-change 后重建输入侧资源，同时保持输出尺寸 `T`。
- 单元测试、集成测试、硬件 smoke 测试、文档和运行日志。

### 3.2 非目标

- 修改 Moonlight 协议。
- 修改 Sunshine RTP、FEC、加密或 UDP 发送格式。
- HDMI RX 音频采集或音频时钟重同步。
- AV1、HDR、10-bit 或 YUV444 编码输出能力扩展。
- CPU/swscale/OpenCV 分辨率回退。
- 同时支持多个 RKMPP/HDMI RX 串流会话。
- 针对每种 HDMI 信号源保证其一定服从 EDID。
- 第一版在绿色占位帧上绘制文字或复杂 UI。
- 创建 GitHub issue 或 pull request。

## 4. 全局工程约束

所有阶段都必须遵守以下约束：

- 不覆盖或清理仓库中已有的未提交修改；开始和结束时记录 `git status --short`。
- 新增或修改的 C/C++ API、类型、字段和非显然逻辑必须有符合仓库格式的 Doxygen 文档。
- 使用 `.clang-format` 格式化所有修改的 C/C++ 文件。
- 新增或修改方法必须添加 gtest 测试， changed code 以 100% 覆盖为目标。
- 本次如需增加本地化，只修改 `en`，不得修改 `en-US` 或其他语言。
- Windows/MSYS2 回归遵守仓库级平台规则；本计划不定义额外构建命令。
- Sunshine 构建有且只有一个入口：`./scripts/build-rkmpp.sh`；不得指定或覆盖构建目录。
- 主测试必须使用该脚本构建产生的 `test_sunshine`，不得复用其他构建目录中的旧产物。
- ROCK 5B+ 专用硬件验证可以使用 Linux 原生命令；必须与 Windows/MSYS2 的通用构建验证分开记录。
- 每个阶段只提交自身范围内的修改；若前序代码存在问题，先在交接记录中说明，不静默扩大范围。

## 5. 阶段交接格式

每个执行 Agent 完成阶段后，必须在其最终说明或阶段记录中提供：

1. 修改文件列表。
2. 最终采用的接口和所有权约定。
3. 执行过的构建、gtest、独立 smoke 和硬件命令。
4. 每条命令的结果；未执行项必须说明环境阻塞原因。
5. 覆盖的成功、失败和回退分支。
6. 尚未解决但不阻塞下一阶段的问题。
7. `git diff --check` 和 `git status --short` 结果。

后续 Agent 必须先阅读本文件、前序交接和相关 diff，再开始工作。

## 6. 阶段 0：固定基线与采集目标机能力

### 6.1 目标

建立可复现的现状基线，并采集 RGA、MPP、HDMI RX、EDID 和 active timings 的真实能力。此阶段只增加诊断资料或测试工具，不实现缩放或协商。

### 6.2 工作项

- 记录当前同分辨率 Moonlight 串流成功的日志和编码参数。
- 记录异分辨率连接失败时的准确错误链，确认失败来自尺寸强制相等检查，而非网络或 codec 问题。
- 记录 `/dev/video0` 的 driver、capabilities、pixel formats、当前 format 和 DV timings。
- 用 `media-ctl -p` 枚举可能承载 EDID ioctl 的 `/dev/v4l-subdevX`。
- 对 video node 和相关 sub-device 做只读 `VIDIOC_G_EDID` 探测。
- 记录 `/dev/rga`、`librga`、头文件、版本和运行用户权限。
- 记录 RGA 对 BGR3、NV24、NV16、NV12 输入以及 NV12 输出的实际 `imcheck()`/demo 结果。
- 不为了完成本阶段而写入 EDID；破坏性 EDID 测试留到阶段 7。

### 6.3 目标机命令

```bash
v4l2-ctl -d /dev/video0 -D
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --list-formats-ext
v4l2-ctl -d /dev/video0 --get-fmt-video
v4l2-ctl -d /dev/video0 --query-dv-timings
v4l2-ctl -d /dev/video0 --get-edid
media-ctl -p
ls -l /dev/video0 /dev/mpp_service /dev/rga
pkg-config --modversion rockchip_mpp
pkg-config --modversion librga
```

若命令名称或 `v4l2-ctl` 版本不同，记录 `v4l2-ctl --help-edid` 输出，并使用该版本公开的等价参数。

### 6.4 验收标准

- 保存同分辨率成功和异分辨率失败的基线证据。
- 得到 RGA 输入/输出格式能力表，未知项明确标记为未知而非假定支持。
- 得到 EDID 只读接口的节点、pad/input 索引和返回结果。
- 后续 Agent 可以在不重复硬件侦察的情况下选择构建探测及 RGA API。

## 7. 阶段 1：纯逻辑尺寸、模式和 viewport 策略

### 7.1 目标

先把不依赖硬件的决策逻辑提取为可单元测试的函数，为 RGA 和 EDID 两条路径建立同一套尺寸语义。

### 7.2 工作项

- 定义带 Doxygen 的分辨率、刷新率、HDMI mode 和 viewport 数据结构；优先复用已有通用结构，避免重复类型。
- 实现 `T` 与 `I` 是否需要转换的判断。
- 实现保持比例、居中、黑边填充的 source/destination rectangle 计算。
- 明确奇数尺寸、NV12 色度对齐、stride 对齐和超大尺寸的拒绝/取整规则。
- 实现第 2.3 节的 HDMI 模式选择函数。
- 所有排序必须是确定性的，不依赖输入集合原始顺序。
- 策略代码不能包含 ioctl、RGA 或 MPP 调用。

### 7.3 gtest 场景

- `I == T`：无需 RGA，viewport 覆盖整个输出。
- 4K → 1080p、1080p → 720p：等比例下采样。
- 720p → 1080p：允许上采样。
- 4:3 → 16:9 和 21:9 → 16:9：正确 letterbox/pillarbox。
- 奇数请求尺寸、零尺寸、溢出尺寸和 NV12 不合法尺寸。
- 存在多个大于 `T` 的模式时选择最小模式。
- 所有模式都小于 `T` 时选择最大模式。
- 相同面积、不同宽高比或刷新率时 tie-breaker 稳定。
- 候选为空时返回明确的无选择状态。

### 7.4 验收标准

- 纯逻辑代码可在没有 Rockchip 库和硬件的测试环境编译。
- 所有边界分支有 gtest 覆盖。
- 后续阶段只消费策略结果，不重复实现尺寸选择算法。

## 8. 阶段 2：RGA 构建探测与最小封装

### 8.1 目标

为 Linux aarch64 增加可靠的 librga 能力探测，并提供带 RAII 和 Doxygen 的最小 RGA API 封装；暂不接入 Sunshine 串流线程。

### 8.2 工作项

- 增加 CMake 的 librga 头文件、链接库和必要 API capability check。
- 不以不可靠的 pkg-config 版本号作为唯一能力门槛。
- 定义清晰的编译宏和构建状态；RKMPP 可用但 RGA 不可用时，原有同分辨率路径仍应可构建。
- 封装 DMA-BUF import、RGA buffer handle、目标 buffer 和释放顺序。
- 封装 source/destination format 映射，不在调用点散布 RGA 常量。
- 封装同步 fill、resize 和必要的颜色转换；第一版优先使用同步执行，异步 fence 属于后续性能优化。
- 每次 RGA 调用都检查状态，并把 `imStrError()` 信息写入异常或日志。
- 在运行前使用 RGA 能力检查验证具体 source/destination rectangle 和格式组合。
- 对目标 NV12 buffer 的分配方式做一次明确选择并记录依据：优先选择能同时被 RGA 写入和 MPP 以 DMA-BUF 导入的分配器。

### 8.3 测试

- gtest：格式映射、对齐、非法 fd、非法尺寸、失败状态到错误信息的转换。
- 构建测试：librga 存在时启用，缺失时给出可理解的 CMake 状态且不破坏非 RKMPP 平台。
- 独立硬件 smoke：分配小型目标 buffer，执行纯色 fill 和一次 resize，验证返回状态、输出布局及重复创建/销毁不泄漏 fd。

### 8.4 验收标准

- Sunshine 主串流代码尚未依赖 RGA 具体类型。
- RAII 测试覆盖正常释放和中途失败释放。
- 不发生 CPU 像素 memcpy 或软件缩放。
- 对阶段 0 中驱动实际提供的每种输入格式给出“支持、需两步转换或明确不支持”的结论。

## 9. 阶段 3：解耦 RKMPP 输入尺寸与输出尺寸

### 9.1 目标

重构现有 RKMPP 输入抽象，使编码器能够接收“原始 HDMI RX frame”或“RGA 输出 frame”，并让编码输出尺寸独立固定为 `T`。

### 9.2 工作项

- 不再把 `hdmirx::capture_format_t` 指针作为 RKMPP 唯一输入契约。
- 定义通用、带所有权说明的 RKMPP 输入 frame/layout 描述：可见尺寸、stride、format、DMA-BUF fd、allocation size、PTS 和持有者。
- RKMPP encoder config 分离 input layout 与 coded output width/height。
- 直通路径继续使用 RX 的真实 stride 和 allocation metadata。
- RGA 路径使用 RGA 目标 buffer 的真实 stride 和 allocation metadata。
- MPP 完成消费前必须持有输入 buffer；同步 encode 返回后才允许释放或回收到 pool。
- MPP 编码包现有零额外码流复制语义保持不变。
- 删除“客户端尺寸必须等于 RX 尺寸”的构造期失败条件，但在本阶段尚未接通 RGA 时，对不等尺寸返回明确的“需要转换器”状态，而不是创建错误的编码器。

### 9.3 gtest 与 smoke

- gtest：输入 layout 校验、output size 校验、stride 小于可见尺寸、allocation 不足、unsupported format。
- gtest：直通与转换输入使用不同 layout，但 coded size 始终来自 `T`。
- 硬件 smoke：原有同分辨率 H.264/H.265 编码结果无回归，ffprobe 报告尺寸正确。

### 9.4 验收标准

- 同分辨率路径行为和性能不回退。
- 输入 frame 生命周期在接口文档和测试中明确。
- 后续 RGA 集成无需伪造 `hdmirx::capture_format_t`。

## 10. 阶段 4：接入 RGA 缩放主路径

### 10.1 目标

使 HDMI RX 任意合法实际输入 `I` 能被转换为 Moonlight 请求尺寸 `T`，彻底移除因尺寸不相等导致的连接失败。

### 10.2 工作项

- display 创建时记录实际捕获尺寸 `I`，同时保留 Sunshine 会话请求尺寸 `T`。
- `I == T` 且格式/layout 可被 MPP 直接导入时选择直通路径。
- 其他情况按阶段 1 viewport 使用 RGA：先清黑目标 buffer，再将源图像缩放到目标 rectangle；若 API 支持且验证通过，可用一次综合操作完成 resize + color conversion。
- 目标格式优先使用 MPP 已验证可接受的 NV12；若硬件能力要求其他格式，必须通过阶段 2 的能力表决定，不在运行时猜测。
- 使用有限大小的目标 DMA-BUF pool，加入明确的 acquire/release/back-pressure 语义。
- RGA 同步操作完成后才归还 V4L2 source buffer；MPP 完成读取后才归还 RGA target buffer。
- RGA 单帧失败必须分类：可重试错误丢帧并继续，连续失败或配置错误触发受控重建；不得使用 CPU fallback。
- 更新触控/viewport 映射中与 letterbox 有关的尺寸，确保使用客户端输出坐标时不引用错误的 capture 尺寸。
- 日志至少包含 `I`、`T`、direct/RGA path、RGA input/output format、stride 和 viewport。

### 10.3 gtest

- path selector：完全匹配走 direct，其余走 RGA。
- viewport 和 RGA job descriptor 的组合。
- buffer pool 正常、耗尽、归还和异常释放。
- source buffer 与 target buffer 的释放先后顺序。
- 单次可重试失败、连续失败阈值和重建信号。

### 10.4 硬件验收矩阵

至少对 H.264 与 H.265 分别验证：

| HDMI 输入 `I` | Moonlight `T` | 预期路径 | 预期结果 |
|---|---|---|---|
| 1920×1080 | 1920×1080 | direct | 正常串流，无 RGA |
| 3840×2160 | 1920×1080 | RGA downscale | 输出精确为 1080p |
| 1920×1080 | 1280×720 | RGA downscale | 输出精确为 720p |
| 1280×720 | 1920×1080 | RGA upscale | 输出精确为 1080p，不断流 |
| 4:3 输入 | 16:9 请求 | RGA + padding | 比例正确，黑边正确 |

每项使用 `ffprobe`/码流分析确认 coded width/height，使用 Moonlight 连续播放确认没有绿边、花屏和持续重连。

### 10.5 验收标准

- 删除当前“HDMI RX 尺寸与请求尺寸不同即失败”的行为。
- 未实现 EDID 的情况下，上述矩阵已经能够稳定串流。
- direct path 不额外分配 RGA frame。
- RGA path 不执行 CPU 像素复制。
- 连续运行和重复启停无 fd、RGA handle、MppBuffer 或 V4L2 buffer 泄漏。

## 11. 阶段 5：绿色占位帧与输入状态机

### 11.1 目标

在无稳定 HDMI 帧、source-change 或未来 EDID 协商期间维持 Moonlight 会话，输出确定性的纯绿色帧。

### 11.2 状态模型

至少定义并记录以下状态；命名可按项目风格调整：

```text
starting/negotiating/no-signal
  → 输出绿色帧 @ T
  → 获得连续稳定 timing 和有效帧
streaming-direct 或 streaming-rga
  → source-change / signal loss
starting/negotiating/no-signal
```

致命设备错误、MPP 无法恢复或 shutdown 仍可结束会话；“分辨率不同”和“EDID 未生效”不得归类为致命错误。

### 11.3 工作项

- 使用阶段 2 的 fill 封装创建尺寸为 `T` 的绿色 NV12 DMA-BUF。
- 绿色值必须根据当前编码色彩范围/矩阵确定并有测试，不能假定 RGB 字节布局。
- 占位期间以足以维持会话的低帧率编码，避免无意义地按 60 FPS 重复填充；具体帧率应成为内部常量并有 Doxygen。
- 第一个绿色 frame 请求 IDR，后续按正常 GOP；恢复真实输入时再次请求 IDR。
- 输入恢复判定至少要求 timing 有效且获得有效捕获帧；避免只看到 source-change 就立即切换。
- 不复用进入无信号状态前的最后一帧。
- shutdown 必须能中断占位循环和 timing 等待。

### 11.4 测试

- gtest：状态转移、超时、shutdown、source-change、恢复时 IDR。
- gtest：绿色 NV12 值和目标 buffer layout。
- 硬件：串流中拔线、重新插线、切换输入模式；Moonlight 会话保持，期间显示绿色，恢复后画面继续。

### 11.5 验收标准

- 短暂无信号和 mode switch 不再直接断开 Moonlight。
- 绿色帧 coded size 始终等于 `T`。
- 占位期间无忙等、高 CPU 使用或资源增长。

## 12. 阶段 6：EDID 数据模型、解析与 ioctl 抽象

### 12.1 目标

在不改变真实设备 EDID 的前提下，实现可测试的 EDID 读写抽象、模式数据模型和验证逻辑。

### 12.2 工作项

- 封装 video node 与 sub-device 的 `VIDIOC_G_EDID`/`VIDIOC_S_EDID` 调用。
- 支持正确的 input/pad、block count、128-byte block 和最大大小校验。
- 对 `EINVAL`、`ENOTTY`、`ENODATA`、`E2BIG`、权限错误和设备消失分别分类。
- 读取接口与写入接口分开报告能力；只有可读、可写且可恢复时才允许自动协商。
- EDID parser/generator 优先复用成熟、可链接的既有能力；若必须在仓库中实现最小逻辑，只处理本次确实需要的模式块并对 checksum 做完整测试。
- 准备经过 `edid-decode` 验证的测试 fixtures，至少包含 720p、1080p、1440p 和 2160p 模式，以及坏 checksum/截断数据。
- 将原始 EDID 保存为 RAII session restore guard；restore 失败必须记录，但析构不得抛异常。
- 本阶段测试使用 mock/fake ioctl，不在单元测试中操作真实 HDMI 设备。

### 12.3 gtest

- 多 block EDID 读取和完整保存。
- 不支持、只读、只写、权限错误和无数据能力分类。
- 部分读取/写入、错误 block count、checksum 错误。
- restore guard：正常结束、初始化中途失败、异常展开和重复 restore。
- fixture 模式解析后交给阶段 1 selector 的结果正确。

### 12.4 验收标准

- 纯单元测试不需要 root、不接触 `/dev/video0`。
- ioctl 细节不泄漏到 HDMI capture/session 状态机。
- 所有可能写 EDID 的路径都具备原 EDID 恢复策略。

## 13. 阶段 7：目标机 EDID 能力和重协商实验

### 13.1 目标

在 ROCK 5B+ 上验证“能否动态替换 EDID”以及“如何触发并检测上游重新锁定”。本阶段允许短暂中断 HDMI，但不接入 Sunshine 自动流程。

### 13.2 前置条件

- 获得测试者明确许可，确认当前 HDMI 输入可以短暂中断。
- 保存原 EDID 到独立文件并使用 `edid-decode` 验证。
- Sunshine 和其他 `/dev/video0` 使用者均已停止。
- 已准备恢复命令和本地控制台/SSH，不能只依赖可能受 HDMI 切换影响的界面。

### 13.3 实验步骤

1. 对阶段 0 找到的 video/sub-device node 执行 G_EDID，并逐字节保存原 EDID。
2. 写入只把选定模式作为 preferred mode 的已验证测试 EDID。
3. 同时观察 kernel log、V4L2 source-change event、`QUERY_DV_TIMINGS` 和有效帧恢复。
4. 记录上游不改变、改变为期望模式、改变为其他模式、暂时断链四种结果。
5. 恢复原 EDID，并验证上游与 RX 返回原始/可接受状态。
6. 重复至少三轮，检查 fd、设备 busy 状态和恢复可靠性。

写入命令必须使用目标机 `v4l2-ctl --help-edid` 给出的本机语法，不能从其他版本复制参数后盲目执行。

### 13.4 验收标准

- 明确结论：不支持、只支持 video node、只支持 sub-device，或支持指定 node/pad。
- 明确是否需要清空 EDID、HPD pulse、重新 STREAMON 或其他驱动步骤。
- 得到实际重锁耗时范围和合理超时值。
- 原 EDID 在每轮实验后恢复并再次读取比对一致。
- 若任一恢复路径不可靠，自动 EDID 协商必须保持禁用；后续仍可完成 RGA 功能。

## 14. 阶段 8：EDID 会话协商与无条件 RGA 回退

### 14.1 目标

把已验证的 EDID 控制接入单会话 HDMI RX 状态机；不支持或不服从 EDID 的设备继续正常串流。

### 14.2 会话流程

```text
Moonlight 请求 T
  → 从验证过的 EDID 模式集合选择 M
  → EDID 不可安全写入：跳过协商
  → EDID 可安全写入：保存原 EDID，写入 M 为 preferred 的会话 EDID
  → 输出绿色帧并等待 source-change/重新锁定
  → 查询实际 timing 得到 I
  → I == T 且 layout 可直通：direct
  → 其他有效 I：RGA I → T
  → 超时/写入失败/无信号：保持绿色并按现有无信号策略重试
  → 会话结束：恢复原 EDID
```

### 14.3 工作项

- EDID capability probe 结果按设备生命周期缓存，但设备重建后重新探测。
- 使用阶段 1 selector，不能在状态机中复制排序逻辑。
- 写入 EDID 后进入绿色占位状态；等待必须可被 shutdown 中断。
- timing 稳定标准至少包含连续一致的查询结果和一帧合法 metadata；阈值写入 Doxygen 和测试。
- 实际 `I != M` 只记录 warning/info，立即按实际 `I` 选择 direct/RGA。
- EDID 失败不进入 capture fatal/reinit 无限循环。
- 会话结束恢复 `D0` 对应的原 EDID；恢复期间不得阻塞会话线程无限等待。
- 现有单 RKMPP 会话限制保持不变，避免两个会话竞争 EDID。

### 14.4 gtest

- 无 EDID 支持：跳过并使用当前 `I` + RGA。
- 写入失败：恢复/回退，串流不因尺寸失败。
- 上游服从：`I == M`，选择正确 direct/RGA path。
- 上游忽略：`I == D0`，使用 RGA。
- 上游选择另一个合法模式：按实际 `I` 使用 RGA。
- 超时、source-change 风暴、拔线和 shutdown。
- 正常结束、初始化失败和异常结束都调用 restore，且只调用一次。

### 14.5 硬件验收

- 在至少两个不同 Moonlight 请求尺寸下观察 EDID 写入、绿色过渡、timing 锁定、画面恢复和最终 coded size。
- 使用一个会服从 EDID 的上游和一个会忽略/固定输出的上游；若无法获得第二种设备，用固定输出配置模拟忽略行为。
- 重复连接/断开至少 20 次，原 EDID 每次都恢复，无设备 busy 和资源增长。
- EDID 完全不支持时，阶段 4 的 RGA 验收矩阵仍全部通过。

### 14.6 验收标准

- EDID 成功能减少不必要的上采样或过度下采样。
- EDID 失败不会造成 Moonlight 因分辨率不一致断开。
- 日志能区分 requested `T`、preferred `M`、actual `I` 和 fallback 原因。

## 15. 阶段 9：异常恢复、性能与长期运行

### 15.1 目标

对完整链路做资源、延迟、吞吐和恢复验证，避免功能正确但无法长期使用。

### 15.2 工作项

- 为 RGA fill/resize、MPP encode 和 capture-to-send latency 增加 debug/trace 级耗时。
- 检查 4K60 输入到 1080p60 输出是否能维持目标帧率。
- 验证 RGA 与 MPP 对 DMA-BUF 的 cache/fence 同步语义；若同步 API 已足够，记录依据，不为追求异步而提前复杂化。
- 验证 buffer pool back-pressure 不会提前 QBUF，也不会永久饿死 V4L2。
- 对连续 RGA 错误、MPP reset、source-change、无信号、重新插线和 shutdown race 做压力测试。
- 用 `/proc/self/fd`、驱动状态和已有 smoke 统计检查资源稳定。

### 15.3 验收标准

- 目标硬件持续串流至少 2 小时，无 fd/handle/buffer 单调增长。
- 4K60 → 1080p60 不出现持续积压；若硬件达不到，记录可复现的上限而不是静默丢帧。
- 拔插、模式切换和 Moonlight 重连后无需重启 Sunshine。
- shutdown 在有界时间内结束，不能卡在 poll、RGA 或 MPP。

## 16. 阶段 10：文档、测试入口与最终回归

### 16.1 目标

完成面向维护者和使用者的文档，统一测试入口，并确认没有破坏其他平台和编码器。

### 16.2 文档工作

- 更新 `docs/rkmpp/README.md`：新链路、依赖、构建、权限、日志和故障排查。
- 更新 Feature 000：把“RGA/任意输出分辨率不实现”改为历史边界，并链接本计划或新的已验收 feature 文档。
- 记录 EDID 支持矩阵、实际 node/pad、超时和恢复行为。
- 记录 direct 与 RGA 路径的性能结果。
- 记录纯绿色占位帧的含义。
- Future work 明确列出：占位帧文字/图标、协商进度、错误原因、更多语言提示、可配置颜色和可配置超时。
- 若新增配置项或 UI 文本，只更新 `en` 本地化。

### 16.3 最终测试

- 运行受影响的全部 gtest，最终运行完整 `test_sunshine`。
- 运行 RKMPP Annex-B、layout、HDMI RX、RGA 与编码硬件 smoke。
- H.264/H.265 码流用 `ffprobe` 和 `ffmpeg -f null` 验证。
- 非 RKMPP Linux 构建验证 librga 缺失不造成回归。
- Windows/MSYS2 配置或构建验证确保 Linux-only 文件没有泄漏到 Windows target。
- 运行 clang-format 检查、Doxygen/文档构建和 `git diff --check`。

Sunshine 构建命令：

```bash
./scripts/build-rkmpp.sh
```

不得增加其他 Sunshine 构建命令、指定构建目录或覆盖脚本的构建目录选择。Windows/MSYS2 回归遵守仓库级平台规则，但本计划不定义第二套构建入口。测试必须使用该脚本产生的 `test_sunshine`。

### 16.4 最终验收标准

- Moonlight 请求尺寸不再要求等于 HDMI RX 实际输入尺寸。
- 同尺寸走 direct path，异尺寸走 RGA，输出 coded size 始终等于请求尺寸。
- EDID 支持与否、上游服从与否都不影响 RGA 基础串流能力。
- 协商和短暂无信号期间显示绿色占位帧，恢复后自动回到真实画面。
- 会话结束恢复原 EDID。
- H.264/H.265、重复启停、source-change、拔插和长期运行通过。
- changed code 接近或达到 100% 测试覆盖，所有新增 API 有 Doxygen，格式检查和文档构建通过。

## 17. 建议的 Agent 分工

阶段必须顺序合入，但可以由不同 Agent 接力：

| Agent | 负责阶段 | 主要产物 | 硬件要求 |
|---|---|---|---|
| A | 0 | 基线和能力报告 | 必须有 ROCK 5B+ |
| B | 1 | 纯策略与 gtest | 无 |
| C | 2 | RGA 构建/RAII 封装 | 构建可无，最终 smoke 需硬件 |
| D | 3 | RKMPP 输入/输出尺寸解耦 | 最终 smoke 需硬件 |
| E | 4 | RGA 串流集成 | 必须有 ROCK 5B+ |
| F | 5 | 绿色占位与状态机 | 最终验收需硬件 |
| G | 6 | EDID 抽象与 fixtures | 无 |
| H | 7 | EDID 破坏性能力实验 | 必须有 ROCK 5B+ 和测试许可 |
| I | 8 | EDID 会话集成 | 必须有 ROCK 5B+ |
| J | 9 | 压力、性能和恢复 | 必须有 ROCK 5B+ |
| K | 10 | 文档与全量回归 | Windows/MSYS2 + ROCK 5B+ 最佳 |

若阶段 7 得出“EDID 不支持或不能可靠恢复”，阶段 8 不实现真实写入，只保留能力探测、清晰日志和 RGA 回退；阶段 9、10 仍继续执行，整体需求仍可通过 RGA 完成核心验收。

## 18. Future work

- 在绿色占位帧上叠加“正在协商 HDMI 输入”“无信号”“已回退到缩放”等状态。
- 对状态文字做 `en` 起始的完整本地化设计。
- 允许用户配置协商超时、占位颜色、EDID 自动协商开关和模式白名单。
- RGA 异步 fence 与 MPP pipeline 并行，以进一步降低处理延迟。
- 根据画质/功耗在多个满足 `T` 的 HDMI 模式之间进行策略选择。
- 多 RKMPP 会话或多个客户端共享同一 HDMI RX 输入。
- HDMI RX 音频在 mode switch 期间的时钟恢复与 A/V 同步。
