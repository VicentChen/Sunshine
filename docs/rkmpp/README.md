# RKMPP

## 使用场景

将接入 ROCK 5B+ HDMI RX 的主机画面以低延迟方式推送至 Moonlight 客户端。

## 功能

- 从 RK3588 HDMI RX 获取视频帧；
- 使用 RKMPP 硬件编码为 H.264 或 H.265；
- 复用 Sunshine 既有的协商、RTP/UDP 封包与网络发送能力；
- 由 Moonlight 解码并显示画面。

## 设计

```text
HDMI 信号源
  → HDMI RX / V4L2 DMA-BUF（RX 输出的原始帧格式）
  → RKMPP 硬件编码（H.264/H.265）
  → 可由 CPU 映射的码流缓冲
  → Sunshine RTP/UDP
  → Moonlight
```

- HDMI 输入模式由信号源决定，RX 通过 EDID 宣告可接受的能力。
- 捕获后端不假定或强制 HDMI RX 输出为 NV12，应支持 RX / V4L2 驱动提供的所有原始帧格式。
- HDMI RX 到 RKMPP 以 DMA-BUF 传递原始帧及其格式信息，避免 CPU 复制或软件颜色转换；编码所需的格式处理由 RKMPP 硬件编码链路完成。
- RKMPP 输出码流直接包装为 Sunshine 所需的数据包后发送，避免额外的码流复制。
- RK3588 不具备 AV1 硬件编码能力，本项目暂不支持 AV1 编码。

## 实现边界

需要增加 HDMI RX 捕获源和 RKMPP 编码后端；Sunshine 的网络发送部分保持复用。

## Sunshine 编译（实测）

在 ROCK 5B+（Debian 12 / ARM64）上，使用脚本可构建带 RKMPP 的 Release 版本。默认输出为 build-rkmpp-review/sunshine。

~~~bash
./scripts/build-rkmpp.sh
~~~

脚本会安装缺失的包、下载项目内依赖、配置 CMake，并在完成后检查可执行文件。它不会修改系统服务或启动 Sunshine。

### Homebrew 依赖

以下工具由 Homebrew 统一管理，脚本会按需安装：

~~~bash
brew install cmake ninja node pkgconf file desktop-file-utils glslang
~~~

其中 Node 用于 Web UI，CMake 与 Ninja 用于配置和生成，其他工具用于生成资源和着色器。脚本显式使用 Homebrew 的 CMake、Ninja 和 Node。

Homebrew 的 GCC 可以保留给其他用途，但不要将它作为本构建的 C/C++ 编译器：当前 Homebrew GCC 与 Debian 12 的 glibc 版本不匹配。脚本固定使用项目下载的 LLVM/Clang 22.1.6，并使用系统链接器。

### 系统依赖（APT）

RKMPP、内核/图形/音频开发头文件及其 ABI 必须来自 Rock OS/Debian 系统，不能以 Homebrew 包替代。脚本会安装：

~~~bash
sudo apt-get install --no-install-recommends \
  build-essential ca-certificates curl git pkg-config xz-utils \
  python3-jinja2 python3-setuptools \
  libayatana-appindicator3-dev libcap-dev libcurl4-openssl-dev \
  libdrm-dev libevdev-dev libgbm-dev libminiupnpc-dev libnotify-dev \
  libnuma-dev libopus-dev libpipewire-0.3-dev libpulse-dev libssl-dev \
  libsystemd-dev libudev-dev libva-dev libvulkan-dev libwayland-dev \
  libx11-dev libxcb-shm0-dev libxcb-xfixes0-dev libxcb1-dev \
  libxfixes-dev libxrandr-dev libxtst-dev xvfb
~~~

此外需要系统已安装 Rockchip MPP 开发文件：pkg-config --modversion rockchip_mpp 应能返回版本，且 /usr/include/rockchip/rk_mpi.h 必须存在。脚本只检查这项；它不会猜测或替换板卡仓库提供的 RKMPP 软件包。

GLAD 的生成器刻意选择系统 Python，并要求其中可导入 Jinja2；上述 python3-jinja2 满足此要求。

### 项目目录内下载的依赖

脚本将下列文件保存在项目内，以便构建目录可复用：

- build/toolchains/LLVM-22.1.6-Linux-ARM64.tar.xz 及解压后的 LLVM/Clang 22.1.6；
- build-rkmpp-review/_deps/ffmpeg-<build-deps 标签>/Linux-aarch64-ffmpeg.tar.gz 及解压后的 FFmpeg 预编译文件；
- CMake 的 _deps/ 缓存中的 Boost、nlohmann_json 等源码依赖；
- Web UI 的 node_modules/ 及生成文件。

FFmpeg 预编译包的标签从 third-party/build-deps 当前提交读取；脚本将该目录传给 FFMPEG_PREPARED_BINARIES，避免构建过程再次执行 Git 网络拉取。

### 网络与代理

首次运行需要访问 Homebrew、GitHub 和 npm。若机器通过 SOCKS5 出网，请先在当前终端设置 ALL_PROXY，例如：

~~~bash
export ALL_PROXY=socks5h://127.0.0.1:1082
~~~

下载前请确保系统时间正确；HTTPS 证书在时间错误时会被安全地拒绝。

### 可选参数

~~~bash
BUILD_DIR=build-rkmpp-review JOBS=4 ./scripts/build-rkmpp.sh
SKIP_APT=1 SKIP_BREW=1 ./scripts/build-rkmpp.sh
SKIP_SUBMODULES=1 ./scripts/build-rkmpp.sh
~~~

SKIP_APT 和 SKIP_BREW 仅适用于已确认依赖完备的机器。默认关闭文档、第三方自测、
CUDA 和托盘；Sunshine 的项目专用测试保持启用。脚本会构建 `sunshine`、
`test_sunshine_rkmpp`、`test_sunshine_ns`、`test_sunshine_xbox` 和
`xbox-remote-probe`。

## 启动脚本

start-sunshine.sh 会启动 build-rkmpp-review/sunshine、避免重复启动，并将项目运行配置与日志放在 runtime-home/config/sunshine/。

~~~bash
./start-sunshine.sh                 # 已登录的图形桌面
DISPLAY=:99 ./start-sunshine.sh     # 捕获 Xvfb :99 虚拟桌面
~~~

Web UI 默认监听 https://<Rock-IP-or-hostname>:47990。脚本不会启动 Xvfb 或 HDMI 预览；这两者需按 RKMPP 运行场景单独启动。

## Moonlight → NS 与手柄回传

启用 NXBT Bridge 后，`NS` 是 Moonlight 中用于串流接入 ROCK 5B+ HDMI RX 的
Sunshine 应用。它同时把 Moonlight 客户端的手柄输入转换为 Nintendo Switch Pro
Controller 输入；手柄不需要再单独连接到 ROCK 5B+。

首次部署需完成 Bridge 安装，并在实际由启动脚本使用的
`runtime-home/config/sunshine/sunshine.conf` 中配置：

~~~text
controller_output = nxbt
nxbt_socket = /run/nxbt-bridge/control.sock
nxbt_controller_slot = 0
back_button_timeout = 1000
~~~

### Moonlight Android 虚拟手柄的 HOME

部分已发布的 Moonlight Android 版本的虚拟手柄只有 `BACK` 和 `START`，没有独立的
`GUIDE`/`HOME` 按钮。将 `back_button_timeout` 设为正数后，长按虚拟手柄的 `BACK`
达到该时长会由 Sunshine 模拟一次 Home/Guide，并通过 NXBT 转发为 Switch `HOME`。
上例的 `1000` 表示长按约 1 秒。

`BACK` 的正常映射仍为 Switch `MINUS`；因此此兼容方式会先短暂发送 `MINUS`，再发送
`HOME`。请在修改配置后重启 Sunshine 使其生效。

### 按 Sunshine 应用路由手柄

手柄输出在创建串流输入时按启动的 Sunshine 应用绑定，避免不同目标同时收到虚拟手柄输入：

| Sunshine 应用 | 手柄输出 |
| --- | --- |
| `Nintendo Switch` | 使用 `controller_output` 配置；本部署设为 NXBT，输入转发到 Switch。 |
| `Xbox` | 使用 Xbox Remote Play 输入后端转发手柄，不创建主机虚拟手柄；详见 [功能总览](FEATURES.md#xbox)。 |
| `HDMI Input`、`Desktop` 和其他应用 | 禁用，不转发到 NXBT 或主机虚拟手柄。 |

Xbox 串流停止但应用仍在运行时，Sunshine 会先发送中立手柄状态，并默认保留 Remote Play
连接 300 秒供快速恢复；可通过 `xbox_remote_idle_timeout` 调整，设为 `0` 表示立即关闭。
明确退出 Xbox 应用时会立即关闭连接，不等待空闲超时。

应用切换或停止时，Sunshine 会先向旧输出发送中立状态并释放该手柄。应用名称来自
`runtime-home/config/sunshine/apps.json`；如重命名 `Nintendo Switch`，需要同步更新路由规则。

### 日常使用（已配对的 Switch）

1. 打开 Switch，并停留在普通主页；不要进入“更改握法/顺序”。
2. 在 ROCK 5B+ 上运行 `./start-sunshine.sh`。
3. 在 Moonlight 中选择 `NS` 并开始串流。
4. Sunshine 会在串流会话开始时预先创建虚拟 Pro Controller，并自动重连已配对的
   Switch，不需要先按 A。连接完成后，NS 画面和手柄回传在同一条串流流程中工作。

Moonlight 断流后再次选择 `NS` 即可再次连接。若 Switch 休眠，先唤醒它并回到普通
主页后再重试。

### 首次与另一台 Switch 配对

每台 Switch 都需要各自完成一次蓝牙配对；配对记录保存在 ROCK 5B+ 的 BlueZ 中。
要接入一台从未配对过的 Switch：

1. 让已配对的其他 Switch 关机、休眠或保持不在附近，避免它先响应自动重连。
2. 运行 `./start-sunshine.sh`，并在 Moonlight 中选择 `NS`。
3. 在新 Switch 打开“控制器”→“更改握法/顺序”。
4. 等待新 Switch 显示 Pro Controller 并完成连接；连接出现后可按 A 确认输入。
5. 配对成功后退出“更改握法/顺序”。后续使用这台 Switch 时，应按“日常使用”的
   步骤在普通主页上启动，以便快速重连。

Bridge 会依次尝试已保存的 Switch；若它们均不可用，会回退到配对发现。因此首次切换
到另一台 Switch 时可能比日常重连稍慢。首版只支持一个正在串流并接收输入的 Switch，
不能同时控制多台 Switch。

## Web UI 与 RKMPP 配置

打开 `https://<Rock-IP-or-hostname>:47990`，在配置页面中选择：

- `Capture`：`HDMI RX (Rockchip)`；对应配置值 `capture = hdmirx`。
- `Encoder`：`Rockchip MPP`；对应配置值 `encoder = rkmpp`。
- `Rockchip MPP Encoder`：设置 RKMPP 专属选项。

| 选项 | 配置键 | 默认值 | 作用 |
| --- | --- | --- | --- |
| Enable Vulkan UI | `vulkan_ui` | `enabled` | Vulkan UI 唯一总控；关闭后跳过全部 Vulkan UI 渲染和 HDMI RX 合成。 |
| Profile HDMI RX and RKMPP Latency | `rkmpp_profile` | `disabled` | 采集 HDMI RX、RGA、MPP、封包和发送阶段的有界延迟统计，并周期性写入日志。 |
| Show RKMPP Latency Overlay | `rkmpp_profile_overlay` | `disabled` | 把最新统计通过 MPP OSD 烧录到 Moonlight 画面；同时启用统计采集。 |
| RKMPP Low-Delay Experiment | `rkmpp_low_delay` | `disabled` | 启用 MPP low-delay 实验配置。现有 A/B 没有证明它优于默认值，因此保持关闭。 |
| Disable RKMPP Re-encode Experiment | `rkmpp_disable_reencode` | `disabled` | 将 MPP rate-control 重编码次数设为零。现有 A/B 的尾延迟更差，因此保持关闭。 |

这些选项只适用于 Linux 上的 HDMI RX + RKMPP 路径。也可以直接写入
`runtime-home/config/sunshine/sunshine.conf`，例如：

~~~text
capture = hdmirx
encoder = rkmpp
vulkan_ui = enabled
rkmpp_profile = enabled
rkmpp_profile_overlay = enabled
rkmpp_low_delay = disabled
rkmpp_disable_reencode = disabled
~~~

### Vulkan UI 总控开关

`vulkan_ui` 是 Vulkan UI 的唯一总控，默认启用。UI 初始隐藏；按住
先按住 `Start`，再点按 `Back/Select` 即可立即打开或关闭，触发后需要完全释放组合键。Start
会先作为 UI 修饰键被截获；如果没有继续按 Select，松开 Start 时会向当前应用补发一次普通点击。
Dear ImGui 通过官方 Vulkan renderer backend 在 960x180 的 optimal-tiled BGR image 中绘制
当前诊断页面。BGR888 直通时，Vulkan 按 capture generation 与 slot 缓存 buffer import，并把
面板直接 copy 到当前 HDMI RX DMA-BUF 底部居中、距下边缘 32 像素的 ROI；这条路径不经过
RGA，也不做 CPU 像素复制。视频本身已进入 RGA fallback 时，Vulkan 才把变化后的面板发布到
共享 BGR DMA-BUF，再由 RGA 转换并覆盖 NV12 target。页面状态不变时不会重复提交 ImGui
绘制；UI 隐藏时两种路径都不执行 ROI 覆盖。

~~~text
vulkan_ui = enabled
~~~

Gate 4 已通过真实 4K HDMI RX/Moonlight 画面和 4 个 capture slot 连续轮转验证。阶段 5
也已通过真实 Moonlight 画面复验：首个会话能显示，页面方向、文字和横向布局均正常。
当前可见内容已经由 ImGui 绘制，但仍只是三个状态项组成的诊断页，不代表设置项、动作按钮
等完整 ImGui UI 已完成。打开 UI 的手柄是 owner；UI 可见时全部手柄输入都会被截获，只有
owner 能用 D-pad 或左摇杆移动三项焦点。A 与 Back 当前只产生控制器导航事件，尚未绑定
Sunshine action。owner 断开或输入状态重置会关闭 UI 并执行中立状态清理。Vulkan 初始化、
模型校验、Vulkan DMA-BUF import/copy 或 fallback RGA 失败时，Sunshine 会在当前会话中关闭 UI 并继续基本编码。
若需临时绕过所有 Vulkan UI 路径，可将唯一总控设置为 `vulkan_ui = disabled`。

## RGA 视频缩放与格式转换

仅当 Moonlight 请求的分辨率与 HDMI RX 实际可见分辨率不一致时，Sunshine 才使用 RGA
执行视频硬件缩放、颜色转换以及保持宽高比的黑边或居中裁剪。尺寸一致时不得使用 RGA
重新缩放视频，原始 HDMI RX DMA-BUF 会直接交给 RKMPP；恢复后格式或 stride 变化会重建
直通编码器。可见 Vulkan UI 的局部 ROI 覆盖不属于视频缩放：BGR888 直通时由 Vulkan
直接写 capture DMA-BUF，只有视频转换后的 NV12 target 才使用 RGA 合成 UI；隐藏时均不执行。

RGA 转换路径使用一个可复用的 NV12 目标 DMA-BUF。同步编码在消费完成后归还该缓冲；
初始化阶段产生但未编码的占位图像也会在下一次转换前主动释放。这样可以降低 4K CMA
峰值，并避免唯一缓冲被占用后导致首帧断流。1080p 与 4K 的精确输入均已验证进入直通
编码；RGA fallback 的 DMA-BUF smoke 连续三轮后 FD 数保持不变。长时间 4K fallback
稳定性仍未单独验收。

## EDID 协商

若 HDMI RX 支持可恢复的 EDID 读写，Sunshine 会尝试请求 Moonlight 所需的分辨率和帧率：

- 当前 HDMI 输入时序已经与目标尺寸一致时，跳过 EDID 重写和视频 RGA。
- 从 base DTD、CTA Video Data Block 与 YCbCr 4:2:0 Video Data Block 解析可用的逐行扫描模式；不匹配时选择尺寸距离最接近、
  且当前生成器能精确表示的模式，写入 EDID 并请求 HDMI 链路重新协商。
- 限制 EDID 时保留接收器身份、音频、HDMI VSDB 与 HDMI Forum VSDB，同时过滤普通 VDB 和
  YCbCr 4:2:0 VDB 中不属于目标尺寸的 VIC，并删除因 VDB 重排而失效的 4:2:0 capability map。
- RK3588 的 `VIDIOC_S_EDID` 自己负责 HPD 拉低、写入和延迟重新拉高；Sunshine 不再紧接着发送
  `RK_HDMIRX_CMD_SOFT_RESET`，避免打断驱动的 HPD 重协商周期。
- 写入成功且输入采用目标分辨率时使用直通 DMA-BUF 路径；即使会话最初因旧 timing 使用 RGA，
  观察到匹配输入后也会重建直通编码器并释放 RGA fallback。
- 上游不接受 EDID、设备不支持 EDID 或最终尺寸不一致时，自动使用实际输入加 RGA。
- 会话结束时恢复原 EDID，由同一驱动 HPD 周期再次请求 HDMI 链路重新协商。
- 等待协商、短暂无信号或 source change 恢复期间输出绿色占位帧，避免立即断开 Moonlight。

## HDMI RX 音频

`audio_source` 可指定 PipeWire/PulseAudio Source，非空时优先于默认的
`audio_sink` monitor 路径。ROCK 5B+ 当前识别到的 HDMI RX Source 为：

~~~text
audio_source = alsa_input.platform-hdmiin-sound.HDMI__hw_rockchiphdmiin__source.8
~~~

具体 Source 名称由系统决定，请以 `pactl list short sources` 或 PipeWire 枚举结果为准。
PulseAudio 后端使用 `pa_threaded_mainloop`，并在 mainloop lock 内串行化 context operation
的创建、释放和断开，避免断流 teardown 与事件清理并发。代码、构建和 Source 选择测试已经
完成；真实 HDMI PCM、Moonlight 播放、断开恢复和 A/V 同步尚未完成实机验收。

## 测试

`scripts/build-rkmpp.sh` 会生成三个相互独立、并与用户配对状态隔离的专用测试模块：

~~~bash
./build-rkmpp-review/tests/test_sunshine_rkmpp
./build-rkmpp-review/tests/test_sunshine_ns
./build-rkmpp-review/tests/test_sunshine_xbox
~~~

三个模块的源码分别位于 `tests/unit/rkmpp/`、`tests/unit/ns/` 和
`tests/unit/xbox/`。项目专用工作不得运行上游通用 `test_sunshine`；模块目标不会编译
或运行 `tests/unit/test_http_pairing.cpp`，并各自使用独立的测试状态路径。

## 日志检查与性能耗时

启用 Trace/Debug 日志或 `rkmpp_profile` 后，可观察：

- **RGA timings**：`setup`、`fill`、`process/resize` 的微秒级耗时。
- **MPP encode latency**：MPP submit、output wait 和完整编码耗时。
- **RKMPP capture-to-send latency**：HDMI RX dequeue、编码、封包到网络提交的主机侧分段耗时。

HDMI RX 驱动当前提供的是单调时钟的帧结束时间戳，因此 RX 第一段表示 EOF 到 dequeue，
不代表第一个 HDMI 像素进入到 EOF 的时间。

## 故障排查

- **卡在绿屏**：尚未锁定有效 HDMI 时序或输入暂时无信号。检查 HDMI 连接和信号源输出。
- **`RGA conversion failed`**：输入格式、尺寸或缩放组合不受支持；查看日志中的输入 `I`、目标 `T` 和 RGA 错误。
- **`RGA target DMA-BUF pool is exhausted`**：转换缓冲仍被上一帧持有。当前实现已修复初始化占位帧的已知泄漏；若再次出现，请保留断流前后的日志。
- **FD/handle 持续增长**：拔插和分辨率变化后 FD 数不应单调增加，可通过 `/proc/<pid>/fd` 观察并保存复现步骤。
