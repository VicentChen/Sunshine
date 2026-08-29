# Feature 000：RKMPP HDMI RX 低延迟串流

> 状态：已验收（2026-08-24）
>
> 本文档记录 RKMPP 的实现设计、边界条件、验证结果与后续可选测试。
> 它取代原先的实施计划；阶段性内容保留为可追溯的工程记录。

## 1. 目标

为 Sunshine 增加一套面向 ROCK 5B+ / RK3588 的 Linux 视频链路：

```text
HDMI 信号源
  → RK3588 HDMI RX
  → V4L2 捕获缓冲 / DMA-BUF
  → RKMPP 硬件 H.264/H.265 编码
  → Sunshine video::packet_t
  → 现有 RTP / FEC / 加密 / UDP 发送链路
  → Moonlight
```

主要目标：

- 从硬编码的 `/dev/video0` 读取 HDMI RX 视频。
- 不假定、不强制 RX 输出为 NV12。
- 支持 RX 驱动当前列出的全部输入帧格式。
- 原始帧通过 DMA-BUF 从 V4L2 传给 RKMPP，不做 CPU 帧复制或软件颜色转换。
- 直接用 RKMPP 编码 H.264 或 H.265，不经过 FFmpeg 编码器封装。
- 直接用 `MppPacket` 作为 Sunshine 编码包的底层存储，不额外复制编码码流。
- 复用 Sunshine 现有的会话、IDR 请求、RTP、FEC、加密和 UDP 发送实现。

## 2. 非目标与第一版边界

第一版不实现：

- AV1 编码。
- HDMI RX 音频采集。
- CPU 颜色转换。
- RGA 格式转换或缩放。
- 任意输出分辨率缩放 (已通过 RGA 实现)。
- HDR / 10-bit 输出能力宣告。
- 新增 Web UI 配置项。
- 修改 Sunshine 现有网络发送协议。
- 在量产捕获路径中调用 `VIDIOC_S_FMT` 强制 NV12 或其他像素格式。

建议复用现有 Sunshine 配置字符串选择后端：

```ini
capture = hdmirx
encoder = rkmpp
```

量产设备路径第一版硬编码为 `/dev/video0`。调试工具可允许通过命令行覆盖路径，但不应把这个参数扩展到 Sunshine Web UI。

## 3. 已验证的环境

目标机器：

- 内核：Linux 6.1.84 RK3588。
- HDMI RX 节点：`/dev/video0`。
- V4L2 驱动：`rk_hdmirx`。
- 能力：`V4L2_CAP_VIDEO_CAPTURE_MPLANE` 和 `V4L2_CAP_STREAMING`。
- 当前信号：1920×1080p60。
- RX 驱动枚举格式：BGR3、NV24、NV16、NV12。
- MPP 运行包：`librockchip-mpp1 1.5.0-1`。
- MPP 开发包：`librockchip-mpp-dev 1.5.0-1`。
- pkg-config 名称：`rockchip_mpp`。
- MPP 设备：`/dev/mpp_service`。
- 运行用户已在 `video` 和 `render` 组。

注意：`pkg-config --modversion rockchip_mpp` 当前返回 `1.3.9`，与 Debian 包版本 `1.5.0-1` 不一致。构建系统不应只依赖 pkg-config 版本号做功能判定；应以头文件、链接结果和必要 API 是否存在为准。

## 4. 格式映射

量产代码用 `VIDIOC_G_FMT` 获取当前 V4L2 输出格式，并按下表映射到 RKMPP：

| V4L2 fourcc | 实际格式 | RKMPP 格式 |
|---|---|---|
| `V4L2_PIX_FMT_BGR24` / BGR3 | 24-bit BGR | `MPP_FMT_BGR888` |
| `V4L2_PIX_FMT_NV24` | YUV 4:4:4 semi-planar | `MPP_FMT_YUV444SP` |
| `V4L2_PIX_FMT_NV16` | YUV 4:2:2 semi-planar | `MPP_FMT_YUV422SP` |
| `V4L2_PIX_FMT_NV12` | YUV 4:2:0 semi-planar | `MPP_FMT_YUV420SP` |

格式处理规则：

- 不根据 Moonlight 的 `chromaSamplingType` 改写 RX 捕获格式。该字段描述的是编码输出能力，不是 HDMI RX 的原始帧格式。
- 宽度、高度、`bytesperline`、`sizeimage`、plane 数量和 offset 以 V4L2 实际返回值为准，不自行假设紧密排布。
- 对驱动将 multiplanar API 与单个连续 DMA-BUF 组合使用的情况，保留每个 plane 的元数据。
- 如驱动未来增加新 fourcc，但当前 RKMPP 版本没有可验证的直接映射，应清晰报错，不默默回退到 CPU 转换。

## 5. 预计文件变更

新增：

- `src/platform/linux/hdmirx.h`
- `src/platform/linux/hdmirx.cpp`
- `src/platform/linux/rkmpp.h`
- `src/platform/linux/rkmpp.cpp`
- `cmake/FindRockchipMPP.cmake`（如果 pkg-config 逻辑不直接写在 Linux CMake 中）
- `tests/unit/platform/test_hdmirx.cpp`
- `tests/unit/test_rkmpp.cpp`
- `tests/rkmpp/`下的硬件 smoke test 或等价的调试目标

修改：

- `src/platform/common.h`
- `src/platform/linux/misc.cpp`
- `src/video.h`
- `src/video.cpp`
- `cmake/prep/options.cmake`
- `cmake/compile_definitions/linux.cmake`
- `tests/CMakeLists.txt`（仅当需要独立硬件测试目标时）

原则上不修改：

- `src/stream.cpp`
- `src/rtsp.cpp`
- `src/nvhttp.cpp`
- Sunshine 现有 RTP / FEC / UDP 实现

如果实际集成需要修改上述“原则上不修改”的文件，开发 Agent 应先记录原因，确认不是在后端里重复实现现有网络功能。

## 6. 核心设计

### 6.1 HDMI RX 图像对象

增加一个从 `platf::img_t` 派生的 HDMI RX 图像类型，至少保存：

- V4L2 buffer index。
- V4L2 fourcc。
- DMA-BUF fd 及其所有权语义。
- width / height。
- 每个 plane 的 stride、offset、bytesused 和 sizeimage。
- V4L2 sequence。
- 转换为 `std::chrono::steady_clock` 基准的捕获时间戳。

该对象的 `data` 指针在正常捕获路径中不需要 CPU map，不应作为数据传递依赖。

### 6.2 V4L2 buffer 管理

捕获后端使用：

1. `VIDIOC_QUERYCAP`。
2. `VIDIOC_QUERY_DV_TIMINGS`，必要时用 `VIDIOC_S_DV_TIMINGS` 应用已检测 timing；这不得改写像素格式。
3. `VIDIOC_G_FMT` 读取驱动当前输出格式。
4. `VIDIOC_REQBUFS` + `V4L2_MEMORY_MMAP` 建立驱动 buffer pool。
5. `VIDIOC_QUERYBUF` 读取 buffer/plane 布局。
6. `VIDIOC_EXPBUF` 为 buffer 导出 DMA-BUF fd。
7. `VIDIOC_QBUF` 排队，`VIDIOC_STREAMON` 启动。
8. `poll()` + `VIDIOC_DQBUF` 获得一帧。
9. 同步编码完成后才能将该 buffer `VIDIOC_QBUF` 回驱动。

所有 fd、STREAMON 状态和 buffer 都使用 RAII 管理。任意失败分支都必须可以重复初始化，不得泄漏 fd 或让 `/dev/video0` 保持忙状态。

### 6.3 RKMPP 输入

对每个 V4L2 DMA-BUF：

- 构造 `MppBufferInfo`，使用 DMA-BUF fd、实际 buffer size 和 index。
- 通过 `mpp_buffer_import()` 导入，类型使用当前 MPP 支持的 DRM / DMA-BUF 外部 buffer 路径。
- 创建 `MppFrame`，设置 width、height、hor_stride、ver_stride、format、PTS 和 buffer。
- 不使用 `memcpy()` 把 RX 图像复制到 MPP 内部 buffer。
- 不使用 swscale 或其他 CPU 颜色转换。

优先使用 MPP 同步 `encode()` 接口完成一帧的输入和输出，并且不为 RKMPP encoder 设置 Sunshine `PARALLEL_ENCODING` flag。目的是保证在 MPP 完成读取前，V4L2 buffer 绝不会被重新 QBUF。

如果同步 `encode()` 在实际 MPP 版本上无法满足低延迟或多 packet 输出需求，再切换到 `encode_put_frame()` / `encode_get_packet()`；切换前必须有明确的 input-consumed 生命周期设计，不得凭假设提前 QBUF。

### 6.4 RKMPP 编码配置

用 `mpp_create()` / `mpp_init()` 创建 H.264 或 H.265 encoder，用 `MppEncCfg` 和 `MPP_ENC_SET_CFG` 配置。每个 cfg setter 和 `control()` 返回值都必须检查。

需要映射的 Sunshine 参数：

- `videoFormat` → AVC / HEVC。
- `bitrate` → CBR target/min/max bitrate。
- `framerate` / `framerateX100` → input/output fps 及时基。
- `numRefFrames` → codec reference frame 配置（仅在 MPP 实际支持时宣告）。
- `slicesPerFrame` → MPP slice split 配置。
- Sunshine IDR event → `MPP_ENC_SET_IDR_FRAME`。

低延迟要求：

- 不使用 B frame。
- 使用 CBR 或经实测更稳定的低延迟 RC 模式。
- 不依赖固定小 GOP 代替 Moonlight 的按需 IDR。
- 通过 `MPP_ENC_SET_HEADER_MODE` + `MPP_ENC_HEADER_MODE_EACH_IDR` 确保每个 IDR 携带必要的 SPS/PPS，H.265 同时包含 VPS。
- 输出 Annex-B byte stream。
- 如启用 slice split，优先使用“多 slice 但单个完整 MppPacket”的输出模式。

第一版不宣告：AV1、HDR/10-bit、YUV444 编码输出、reference frame invalidation。这不影响 RX 以 NV24 作为原始输入；NV24 到 H.264/H.265 编码输入的处理交给 RKMPP 硬件链路。

### 6.5 编码包生命周期

增加 `packet_raw_rkmpp`，从 `video::packet_raw_t` 派生，内部持有 `MppPacket`。

接口映射：

- `data()` → `mpp_packet_get_pos()`。
- `data_size()` → `mpp_packet_get_length()`。
- `frame_index()` → `mpp_packet_get_pts()` 或 Sunshine 绑定的 frame index。
- `is_idr()` → 结合 `KEY_OUTPUT_INTRA` 和 Annex-B NAL type 判定，不把普通 I frame 误报为 IDR。
- 析构函数 → `mpp_packet_deinit()`。

`MppPacket` 必须一直存活到 `stream::videoBroadcastThread()` 完成该帧的封包、FEC 和发送数据构造。后端不得把码流先复制进 `std::vector<uint8_t>`。

需要明确记录：Sunshine 现有 `stream.cpp` 为了插入协议头、RTP 分片和 FEC 仍会生成发送缓冲。本实现的零拷贝边界是“V4L2 原始帧到 MPP”，以及“MPP 输出到 Sunshine packet 接口不新增一次码流复制”。

### 6.6 Sunshine 抽象对接

预计增加：

- `platf::mem_type_e::rkmpp`。
- RKMPP 专用 `encoder_platform_formats_t` 派生类型，避免把 RKMPP 伪装成 AVCodec 或 NVENC。
- `platf::rkmpp_encode_device_t`。
- `display_t::make_rkmpp_encode_device()`。
- `video::rkmpp_encode_session_t`。
- `video::rkmpp` encoder 描述符，codec 名使用 `h264_rkmpp` 和 `hevc_rkmpp`。
- `make_encode_device()`、`make_encode_session()` 和 `encode()` 的 RKMPP 分支。

RKMPP 分支在选择输入格式时，必须以 `hdmirx_img_t` 的实际 V4L2 fourcc 为准，不能复用当前 AVCodec/NVENC 路径中基于客户端 chroma/depth 预选输入 pix_fmt 的逻辑。

RKMPP encoder 不设置 `PARALLEL_ENCODING`，使 Sunshine 走 `captureThreadSync()` / `encode_run_sync()`，保证 V4L2 输入 buffer 的生命周期可控。

### 6.7 Encoder probing

Sunshine 启动和建立会话时会调用 `probe_encoders()` / `validate_encoder()`。RKMPP 不能绕过这个流程，否则 NVHTTP 公布的 H.264/H.265 能力会与实际不一致。

验证阶段需要一帧可编码的输入。不应为此改变 HDMI RX 格式。可选方案是让 RKMPP session 识别 probe-only dummy image，使用 MPP 内部分配的小型黑帧 buffer 完成 encoder viability test。该路径只用于探测，不得出现在正常 HDMI RX 帧处理中。

探测结果：

- H.264 成功后宣告 H.264。
- H.265 成功后宣告 HEVC Main。
- AV1 始终不宣告。
- 第一个编码包必须是 IDR。
- H.264 IDR 前必须可找到 SPS/PPS。
- H.265 IDR 前必须可找到 VPS/SPS/PPS。
- 不宣告尚未实测通过的 ref-frame invalidation、HDR、10-bit 或 YUV444 编码输出能力。

### 6.8 分辨率和帧率

- RX width/height 来自 DV timings 和 `VIDIOC_G_FMT`。
- MPP coded width/height 跟随 RX active width/height，不使用 CPU 或 RGA 缩放。
- 首轮 Moonlight 验收使用与 RX 一致的分辨率和帧率。
- 必须记录 Moonlight 请求模式与 RX 模式不一致时的实际行为。在没有缩放层的第一版中，如果尺寸不一致会破坏客户端解码或坐标映射，应显式拒绝会话并记录清晰错误，不默默转换。
- V4L2 捕获时间戳传给 `packet_raw_t::frame_timestamp`，Sunshine 继续用它产生 RTP 90 kHz timestamp 和延迟统计。

### 6.9 断线与模式切换

捕获后端订阅 `V4L2_EVENT_SOURCE_CHANGE`。发生拔线、重新插入、分辨率或帧率变化时：

1. 停止 DQBUF 循环。
2. `VIDIOC_STREAMOFF`。
3. 释放所有 MPP input reference、DMA-BUF fd 和 V4L2 buffers。
4. 返回 `platf::capture_e::reinit`。
5. Sunshine 现有 reinit 流程重建 display 和 encoder session。
6. 重新查询 timings 和当前 fourcc，不沿用旧格式。

无信号时不得 busy-loop。应使用 poll timeout 和有界的重试间隔，同时保持 shutdown 可快速响应。

## 7. 实施与验收记录

以下阶段内容保留为实现与验收记录。涉及 HDMI 信号的验证均应记录当时的 timings 和 fourcc。

本次验收已覆盖 1080p60 的 H.264/H.265 串流、非匹配分辨率会话的快速失败和恢复、回到 1080p60 的重连，以及 HDMI 拔插/信号模式变化后的恢复。

经确认，本次不执行 50 次重连、连续 2 小时串流或 4K 输入测试；这些是后续可选的耐久性验证，不构成本 feature 的验收前置条件。

### 阶段 0：基线与环境检查

工作：

- 记录当前 commit、CMake 参数和现有未跟踪文件。
- 验证 Sunshine 当前版本可以在不修改代码的情况下构建。
- 验证 MPP 头文件、pkg-config、链接库、`/dev/video0` 和 `/dev/mpp_service`。

测试：

```bash
pkg-config --cflags --libs rockchip_mpp
pkg-config --modversion rockchip_mpp
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --list-formats-ext
./scripts/build-rkmpp.sh
```

验收标准：

- 现有 Sunshine 构建成功。
- `rk_mpi.h`、`mpp_buffer.h`、`mpp_frame.h` 可被编译器找到。
- `-lrockchip_mpp` 可成功链接。
- 两个硬件设备可读写。

### 阶段 1：构建系统和纯数据映射

工作：

- 增加 `SUNSHINE_ENABLE_RKMPP` CMake 选项。
- Linux/aarch64 上通过 pkg-config 查找 `rockchip_mpp`。
- 增加 `SUNSHINE_BUILD_RKMPP` compile definition。
- 加入 HDMI RX 和 RKMPP 源文件空骨架。
- 实现无硬件依赖的 fourcc → MPP format 映射函数。

自动测试：

- BGR3 映射为 `MPP_FMT_BGR888`。
- NV24 映射为 `MPP_FMT_YUV444SP`。
- NV16 映射为 `MPP_FMT_YUV422SP`。
- NV12 映射为 `MPP_FMT_YUV420SP`。
- 未知 fourcc 返回显式 unsupported，不回退为 NV12。
- RKMPP 选项关闭时，其他 Linux 平台仍可构建。

验收标准：

- `SUNSHINE_ENABLE_RKMPP=ON` 时编译和链接成功。
- `SUNSHINE_ENABLE_RKMPP=OFF` 时不影响现有后端。
- fourcc 映射单元测试全部通过。

### 阶段 2：HDMI RX 采集与 DMA-BUF 导出

工作：

- 实现 V4L2 设备打开、能力检查、timings 查询和 `VIDIOC_G_FMT`。
- 实现 buffer 申请、plane 布局查询、DMA-BUF 导出、QBUF/DQBUF 和 STREAMON/OFF。
- 实现 RAII 清理和 frame timestamp/sequence 传递。
- 提供硬件 smoke test：捕获指定帧数，只输出元数据，不 mmap 或保存原始画面。

硬件测试建议：

```bash
rkmpp_hdmirx_smoke --frames 300
```

测试应检查：

- 驱动名必须是 `rk_hdmirx`。
- 输出的 fourcc 与 `VIDIOC_G_FMT` 一致。
- 日志包含 width/height、plane count、stride、sizeimage、fd、sequence 和 timestamp。
- sequence 单调递增，连续捕获 300 帧不超时。
- 退出后可以立即再次打开 `/dev/video0`。
- 连续运行多次后 fd 数量不增长。
- 代码审查确认量产路径没有调用 `VIDIOC_S_FMT` 强制像素格式。

验收标准：不使用 RKMPP 也能稳定获得可导入的 DMA-BUF 帧及完整格式元数据。

### 阶段 3：独立 RKMPP 编码 smoke test

工作：

- 实现 MPP context/config/frame/packet 的 RAII 封装。
- 将阶段 2 的 DMA-BUF 直接 import 到 MPP。
- 实现 H.264 和 H.265 基本配置。
- 实现 IDR 请求和 IDR header mode。
- smoke test 允许将编码码流写到 `/tmp`，仅用于测试；Sunshine 正式路径不保存文件。

硬件测试建议：

```bash
rkmpp_rkmpp_smoke --codec h264 --frames 300 --output /tmp/rkmpp.h264
ffprobe -v error -show_streams /tmp/rkmpp.h264
ffmpeg -v error -i /tmp/rkmpp.h264 -f null -

rkmpp_rkmpp_smoke --codec h265 --frames 300 --output /tmp/rkmpp.h265
ffprobe -v error -show_streams /tmp/rkmpp.h265
ffmpeg -v error -i /tmp/rkmpp.h265 -f null -
```

格式测试矩阵：

- RX 当前输出 BGR3 时完成 H.264/H.265 编码。
- RX 实际输出 NV24 时完成 H.264/H.265 编码。
- RX 实际输出 NV16 时完成 H.264/H.265 编码。
- RX 实际输出 NV12 时完成 H.264/H.265 编码。

测试程序不应为达成某个矩阵项而在量产代码路径中强制格式。每次测试必须记录 `VIDIOC_G_FMT` 实际值。

验收标准：

- ffprobe 识别出正确 codec、width、height 和 framerate。
- ffmpeg 能解码全部帧，没有 invalid NAL / missing parameter set 错误。
- H.264 码流含 SPS/PPS/IDR；H.265 码流含 VPS/SPS/PPS/IDR。
- 运行期间不复制原始帧，V4L2 buffer 只在同步编码返回后 QBUF。
- 连续运行和重复启停不泄漏 fd、MppFrame、MppBuffer 或 MppPacket。

### 阶段 4：接入 Sunshine capture / encoder 框架

工作：

- 在 Linux source selector 中注册 `hdmirx`。
- 实现 `display_names()` 和 `display()` 的 HDMI RX 分支。
- 注册 `rkmpp` encoder 和 H.264/H.265 codec。
- 对接 `make_encode_device()`、`make_encode_session()`、`encode()`。
- 使 `probe_encoders()` 能验证 RKMPP 并设置 `active_hevc_mode`。
- 保持 `PARALLEL_ENCODING` 关闭。

测试：

1. 在 Sunshine 配置中写入 `capture = hdmirx` 和 `encoder = rkmpp`。
2. 启动 Sunshine，暂不连接 Moonlight。
3. 检查日志中的 capture source、H.264 probe、H.265 probe 和能力宣告。
4. 分别将 `encoder = rkmpp` 配合非 HDMI capture，以及 `capture = hdmirx` 配合不兼容 encoder，确认能清晰失败而不崩溃。

验收标准：

- Sunshine 日志出现 `Found H.264 encoder ... [rkmpp]`。
- MPP HEVC probe 成功时，Sunshine 向 Moonlight 宣告 HEVC。
- Sunshine 不宣告 RKMPP AV1。
- 启动 probe 不修改 RX 当前 fourcc。
- 其他 capture/encoder 组合仍可正常构建和启动。

### 阶段 5：`MppPacket` 零额外拷贝对接和 Moonlight 首帧

工作：

- 实现 `packet_raw_rkmpp`。
- 将 MPP packet 直接 raise 到 `mail::video_packets`。
- 传递 frame index、IDR 标志、channel data 和 capture timestamp。
- 验证 MPP 不返回需要后端拼接的 partition packet；若会返回，先调整 split output mode 使每帧为单个完整 packet，不立即引入码流 memcpy。

测试：

- Moonlight 使用与 HDMI RX 相同的 1080p60 模式连接。
- 分别强制 H.264 和 H.265 会话。
- 连接后确认首帧为 IDR，画面可见且持续更新。
- 在串流中触发 Moonlight IDR 请求，确认下一帧是带 parameter sets 的 IDR。
- 观察 Sunshine `Frame processing latency` 和 frame send 日志。

验收标准：

- H.264 和 H.265 都可在 Moonlight 稳定显示。
- 没有绿屏、明显色彩错乱、持续花屏或首帧卡住。
- IDR 请求后客户端可恢复。
- `packet_raw_rkmpp` 不拥有码流 `std::vector` 副本。
- packet 被网络线程消费后 `MppPacket` 正确释放。

### 阶段 6：重建、异常和资源生命周期

工作：

- 订阅和处理 `V4L2_EVENT_SOURCE_CHANGE`。
- 实现无信号、poll timeout、DQBUF 失败和 MPP 失败的错误分类。
- 对可恢复模式变化返回 `capture_e::reinit`。
- 对不可恢复错误停止当前 session，不让 capture thread 卡死。
- 确保 shutdown 可以中断 poll/encode 并在有界时间内结束。

硬件测试：

- 串流中拔掉 HDMI，等待后重新插入。
- 串流中将信号源在 1080p60 和另一个 EDID 支持的模式之间切换。
- 在捕获和编码活跃时停止 Moonlight session。
- 连续建立/停止串流 50 次。
- 连续运行至少 2 小时。

验收标准：

- 不崩溃、不死锁、不 busy-loop。
- 信号恢复后能重建 capture/encoder，或者以明确错误结束会话并允许下次连接。
- 模式切换后重新读取 fourcc、stride 和 sizeimage。
- `/proc/<pid>/fd` 数量不随重连持续增长。
- Sunshine 停止后 `/dev/video0` 和 `/dev/mpp_service` 可立即被再次使用。

### 阶段 7：性能、延迟和最终回归

验收结果：

- 1080p60 H.264 与 H.265 均已完成低延迟端到端串流验收。
- RKMPP 布局测试与 Annex-B 参数集/IDR 测试均通过。
- 启用 RKMPP 的正式构建和关闭 RKMPP 的常规构建均通过；后者不链接 Rockchip MPP。
- 静态检查确认 HDMI RX 到 MPP 使用 DMA-BUF import，未发现原始帧 CPU map、memcpy() 或软件颜色转换。
- 当时的常规构建未配置测试；完整 `test_sunshine` 受 GCC 12 缺少 `std::format` 支持阻塞，问题位于既有 locale 测试而非本 feature。
- 50 次重连、两小时长稳和 4K 输入测试按本次验收范围豁免。

工作：

- 测量 1080p60 H.264/H.265 的 CPU、内存、帧丢失和 Sunshine frame processing latency。
- 在硬件允许时测试 4K 输入。
- 检查原始帧路径是否出现意外 `memcpy()` / CPU map。
- 运行 Sunshine 现有单元和集成测试。
- 验证 RKMPP 选项关闭的常规 Linux 构建。

测试：

```bash
./scripts/build-rkmpp.sh
```

随后运行该脚本产生的 `test_sunshine`，不得复用其他构建目录中的旧产物。

另外记录：

- H.264/H.265 实际码率与 Moonlight 目标码率的偏差。
- 捕获 sequence 间断数。
- MPP 输出帧数和网络发送帧数。
- 平均、P95 和最大 frame processing latency。
- Sunshine 进程 CPU 使用率和 RSS。

最终验收标准：

- 1080p60 H.264 和 H.265 都能连续稳定串流。
- RX 当前输出为 BGR3、NV24、NV16 或 NV12 时，都有对应的格式映射和硬件编码路径，不做 CPU 转换。
- HDMI RX 到 RKMPP 没有原始帧拷贝。
- RKMPP 到 `video::packet_t` 没有新增码流拷贝。
- IDR、SPS/PPS/VPS、码率和时间戳行为符合 Moonlight 需求。
- 拔插、模式切换、会话停止和重连不泄漏资源。
- 现有 Sunshine 后端不因 RKMPP 代码发生回归。

## 8. 开发时必须保持的不变量

1. 不强制 HDMI RX 为 NV12。
2. 不对真实 RX 帧执行 CPU 颜色转换。
3. V4L2 buffer 在 MPP 完成读取前不得 QBUF。
4. `MppPacket` 在 Sunshine 网络线程消费完前不得释放。
5. 不用 FFmpeg encoder 代替 RKMPP 原生编码。
6. 不在 RKMPP 后端重复实现 Sunshine RTP/FEC/UDP 逻辑。
7. 不宣告没有通过硬件测试的 codec 或功能。
8. 任意失败路径都必须释放 V4L2 fd、DMA-BUF fd、MppBuffer、MppFrame、MppPacket 和 MppCtx。

## 9. 开发 Agent 交付要求

每个阶段完成时，提交或交接记录应包含：

- 本阶段修改的文件。
- 实际执行的测试命令和结果。
- 测试时的 HDMI timings、V4L2 fourcc 和 codec。
- 尚未覆盖的格式或硬件情况。
- 是否发生原始帧复制、码流复制或 CPU map。
- 所有新增已知限制和后续工作。

如一个阶段的硬件验收失败，不应通过引入 CPU 拷贝、强制 NV12 或绕过 Sunshine encoder probing 来掩盖问题。应保留日志和实际格式元数据，单独分析驱动 / MPP 接口行为。
