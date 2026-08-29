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

SKIP_APT 和 SKIP_BREW 仅适用于已确认依赖完备的机器。默认关闭文档、测试、CUDA 和托盘；托盘依赖 Qt，未安装时不影响 RKMPP 主程序构建。

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

## RGA 视频缩放与格式转换
目前已接入 Rockchip 2D 图形加速器 (RGA) 支持。仅当 Moonlight 请求的分辨率与 HDMI RX 实际接收的可见分辨率不一致时，Sunshine 才会使用 RGA 对图像进行缩放与裁剪 (信箱模式或居中裁剪)，并生成 NV12 目标缓冲。对于相同尺寸的请求，HDMI RX 的原始 DMA-BUF（包括像素格式和 stride）会直接交给 RKMPP；格式或 stride 在恢复后变化时会重建直通编码器，不会回退到 RGA。在等待 EDID 协商或短暂无信号期间，屏幕将显示一个绿色的占位帧以防串流断开。

## EDID 协商 (可选支持)
若 HDMI 输入设备支持自动解析，系统能够在新的 Moonlight 客户端连接时，修改系统的 EDID 倾向以请求符合所需分辨率及帧率。
- 只有成功写入 EDID 并且设备服从新的分辨率后，才使用硬件零拷贝路径。
- 如果上游设备不服从 EDID 或不支持此能力，Sunshine 会记录日志并自动回退到 RGA 转换模式。
- 会话结束后，原有的 EDID 将被自动恢复。

## 日志检查与性能耗时
可以通过设置 Trace/Debug 级别的日志以获取每帧的详细 RGA、MPP 以及捕获发送 (capture-to-send) 延迟信息。
- **RGA timings**: 包含 , ,  的微秒级耗时。
- **MPP encode latency**: MPP 硬件编码的消耗时间。
- **RKMPP capture-to-send latency**: 帧从 HDMI RX 被捕获，到完成打包准备发送之间的总体端到端延迟。

## 故障排查 (Troubleshooting)
- **卡在绿屏**: 表明未成功锁定到有效的 HDMI 视频时序或处于无信号状态。检查 HDMI 连接或尝试拔插。
- **RGA conversion failed**: 检查是否有不支持的格式被传递给 RGA，或者申请了超大的缩放比。日志中将输出具体的 RGA I (输入) 和 T (目标) 尺寸。
- **资源泄漏 (Fd/Handle单调增加)**: 正常情况下拔插和变更分辨率不应引发 FD 增长。如果在长时间连续测试后观察到问题，请通过  数量并带上复现步骤提交 Issue。


## RGA 视频缩放与格式转换
目前已接入 Rockchip 2D 图形加速器 (RGA) 支持。仅当 Moonlight 请求的分辨率与 HDMI RX 实际接收的可见分辨率不一致时，Sunshine 才会使用 RGA 对图像进行缩放与裁剪 (信箱模式或居中裁剪)，并生成 NV12 目标缓冲。对于相同尺寸的请求，HDMI RX 的原始 DMA-BUF（包括像素格式和 stride）会直接交给 RKMPP；格式或 stride 在恢复后变化时会重建直通编码器，不会回退到 RGA。在等待 EDID 协商或短暂无信号期间，屏幕将显示一个绿色的占位帧以防串流断开。

## EDID 协商 (可选支持)
若 HDMI 输入设备支持自动解析，系统能够在新的 Moonlight 客户端连接时，修改系统的 EDID 倾向以请求符合所需分辨率及帧率。
- 只有成功写入 EDID 并且设备服从新的分辨率后，才使用硬件零拷贝路径。
- 如果上游设备不服从 EDID 或不支持此能力，Sunshine 会记录日志并自动回退到 RGA 转换模式。
- 会话结束后，原有的 EDID 将被自动恢复。

## 日志检查与性能耗时
可以通过设置 Trace/Debug 级别的日志以获取每帧的详细 RGA、MPP 以及捕获发送 (capture-to-send) 延迟信息。
- **RGA timings**: 包含 `setup`, `fill`, `process/resize` 的微秒级耗时。
- **MPP encode latency**: MPP 硬件编码的消耗时间。
- **RKMPP capture-to-send latency**: 帧从 HDMI RX 被捕获，到完成打包准备发送之间的总体端到端延迟。

## 故障排查 (Troubleshooting)
- **卡在绿屏**: 表明未成功锁定到有效的 HDMI 视频时序或处于无信号状态。检查 HDMI 连接或尝试拔插。
- **RGA conversion failed**: 检查是否有不支持的格式被传递给 RGA，或者申请了超大的缩放比。日志中将输出具体的 RGA I (输入) 和 T (目标) 尺寸。
- **资源泄漏 (Fd/Handle单调增加)**: 正常情况下拔插和变更分辨率不应引发 FD 增长。如果在长时间连续测试后观察到问题，请通过 `/proc/<pid>/fd` 数量并带上复现步骤提交 Issue。
