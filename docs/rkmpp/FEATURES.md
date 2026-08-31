# RKMPP 功能

本文档仅汇总当前代码已经实现的功能。已实现但尚未完成完整真机验收的能力会单独注明，不将计划项视为已支持功能。

## RKMPP

- 在 Linux aarch64 的 Rockchip 平台提供 `hdmirx` 捕获后端和 `rkmpp` 编码后端；两者必须配套使用。
- 从 `/dev/video0` 的 `rk_hdmirx` V4L2 多平面设备捕获视频，默认向驱动请求四个 capture slot，支持 BGR3、NV24、NV16 和 NV12，并通过 DMA-BUF 把原始帧交给硬件链路，避免 CPU 像素复制和软件颜色转换。
- 使用 Rockchip MPP 编码 H.264 和 H.265，支持 IDR 请求及 Annex-B 参数集处理，并复用 Sunshine 现有的 RTP、FEC、加密和 UDP 发送链路。
- HDMI 输入尺寸与 Moonlight 请求尺寸一致时使用直通编码；尺寸不一致时使用 RGA 硬件缩放和格式转换，支持保持宽高比的黑边或居中裁剪。720p、1080p 和 4K RGA 串流已有实机验证记录。
- HDMI RX 捕获会阻塞等待首帧，然后非阻塞丢弃积压旧帧并选择最新完整帧；主动 freshness drop、驱动丢帧和捕获年龄分别统计，避免信号恢复后继续编码过期画面。
- 直通路径按捕获 generation 与 buffer index 缓存 MPP 输入导入，RGA 路径按固定 target slot 缓存；fd 数值不作为跨重开身份，source change、encoder reconfigure 和 teardown 会使对应缓存失效。
- RGA 转换只保留一个可复用的 NV12 目标 DMA-BUF；初始化阶段未编码的占位输入会在下一次转换前释放，正常编码完成也走同一清理路径。该修复已通过 1080p Moonlight 实机连接，4K 新缓冲配置仍待长时间实机验证。
- 支持可选 EDID 协商、会话结束时恢复原 EDID，以及上游不接受 EDID 或设备不支持 EDID 时自动回退到实际输入尺寸加 RGA 的路径；当前输入已匹配目标尺寸时跳过不必要的 EDID 重写和 HDMI 链路重置。
- 支持 HDMI 拔插、信号丢失和模式变化后的重新检测与重建；协商或短暂无信号期间可输出绿色占位帧，避免会话立即断开。
- 支持有界的逐帧延迟统计，覆盖 HDMI RX、可选 RGA、RKMPP 编码、打包和主机发送阶段；可写入日志，也可通过 RKMPP 硬件 OSD 烧录到 Moonlight 画面。V4L2 时间戳当前表示帧结束，因此 RX 第一段是 EOF 到 dequeue，而不是完整 HDMI 扫描时间。
- 直通、RGA 创建和运行时 reconfigure 共用同一套 MPP 配置构造，Moonlight 请求的帧率、分数帧率、码率和 GOP 不再在直通路径静默回落到默认值。
- 编码输出使用调用方提供的 8 MiB MPP packet buffer，并由 `encoded_packet_t` 持有到网络消费者释放，不额外复制到 `std::vector`。普通 P 帧会跳过不必要的 Annex-B IDR 扫描。
- Web UI 可选择 `HDMI RX (Rockchip)` 和 `Rockchip MPP`，并配置延迟统计、统计 OSD、低延迟实验和关闭重编码实验选项。
- 可通过 `audio_source` 选择 HDMI RX 对应的 PulseAudio/PipeWire Source，并复用 Sunshine 的 float PCM → Opus 音频链路；空值保持原有 `audio_sink` monitor 回退。ROCK 5B+ 已识别 ALSA `hw:3,0` 和对应的双声道 PipeWire Source，支持的枚举格式为 S16LE、S24_32LE、S32LE、32–192 kHz。Source 优先级和 monitor 回退已有自动测试；真实 PCM、Moonlight 播放、断开恢复及 A/V 同步尚未完成实机验收。

### 性能结论

- 1080p60 HEVC 直通的 12 个连续五秒窗口中，MPP encode P50/P95 约为 5.01/5.84 ms，Host to send P50/P95 约为 6.65/8.37 ms；正常串流时 RX driver age P95 约为 0.05 ms。这是快速对比记录，不代替完整硬件耐久验收。
- `rkmpp_low_delay=enabled` 与 `rkmpp_disable_reencode=enabled` 的组合没有超过默认快照，Host to send P99 约为 13.45 ms；仅关闭重编码时 P95/P99 也回退。因此两个选项保留为实验开关并默认关闭。
- 输入 import cache 在测量窗口中稳定命中。MPP output buffer acquire P50/P95 约为 0.63/1.11 ms，packet init P50/P95 约为 0.46/0.71 ms；输出有界池仍只是有测量依据的未来候选，没有作为当前已实现能力声明。
- RKMPP 使用同步单帧 capture/encode 路径，保持完整 access unit、`split:mode=0` 和 `split:out=0`。Slice low-delay 与异步 MppTask pipeline 尚未产品化。

### 构建与验证

- `scripts/build-rkmpp.sh` 构建 `sunshine`、隔离状态的 `test_sunshine_rkmpp` 和 `xbox-remote-probe`。RKMPP 专用测试不编译 HTTP pairing 测试，也不会写入用户的真实配对状态。
- Sunshine 状态文件不存在，或已有文件缺少有效 `root.uniqueid` 时，会生成并持久化新的 GameStream host ID；缺少整个 `root` 对象的损坏状态会安全恢复为空状态。
- 2026-08-31 的完整 RKMPP 测试共 706 项：693 项通过、13 项按平台条件跳过、0 项失败；随后 ROCK 5B+ 上的 Moonlight 1080p 串流连接成功。

## NS

- 内置 `Nintendo Switch` 应用把 Moonlight 手柄输入按应用路由到 NXBT Bridge；这是 NS 功能在代码中的精确应用名，其他应用不会误发输入到 Switch。
- 支持 `virtual`、`nxbt` 和 `both` 三种手柄输出模式，可只输出到主机虚拟手柄、只输出到 Switch，或同时输出到两者。
- Sunshine 与独立 NXBT Bridge 通过受限的本机 Unix `SOCK_SEQPACKET` 通信；协议具有版本握手、定长二进制消息、序列号、心跳、连接状态和错误状态。
- 支持 A/B/X/Y、方向键、PLUS/MINUS、HOME、CAPTURE、L/R、L3/R3、双摇杆和数字化 ZL/ZR；支持按键标签/位置两种面键策略、摇杆校准及可配置的扳机按下/释放阈值。
- 支持把 Moonlight Android 虚拟手柄的长按 BACK 转换为 HOME，同时保留短按 BACK 到 MINUS 的映射。
- Bridge 会优先重连 BlueZ 中已保存的 Switch；没有可用的已配对设备时可进入首次配对流程。首次配对后，已有实机记录证明可从 Switch 普通 HOME 页面自动快速重连。
- 输入发送使用非阻塞的最新完整状态模型；Sunshine/Bridge 断开、应用切换、手柄释放或 watchdog 超时时会发送中立状态并释放资源，降低粘键风险。
- 提供安装、卸载、部署前检查和硬件验证脚本，并以受限 socket 权限及独立 systemd 服务运行 Bridge。
- 当前首版只支持一个活动 NXBT 手柄和一台正在接收输入的 Switch。真实 Moonlight→Sunshine→Switch 功能及重启后重连已通过；30 分钟 soak 和完整故障/watchdog 矩阵未执行。

## Xbox

- 在受支持的 Linux 构建中，内置 `Xbox` 应用默认自动启动 Xbox Remote Play 手柄回传；可配置精确应用名或关闭功能，非 Xbox 应用不会建立会话、唤醒主机或接收该路由的手柄输入。
- 提供 `xbox-remote-probe` 完成 Microsoft Device Code 登录、凭据恢复、主机枚举、唤醒、REST 会话、WebRTC、启动握手、输入和 soak 诊断；OAuth 凭据保存在属主专用的 `0600` 文件中，Xbox/XSTS/GSSV 临时令牌只保留在内存。
- 账号只有一台 Home Xbox 时可自动选择；多台主机时可通过稳定 ID 显式选择。会话可按配置执行 XCCS WakeUp 门禁。
- 支持 Home Remote Play 会话创建、状态轮询、SDP/ICE 交换、四条必需 WebRTC data channel、启动握手、keepalive 和会话删除；视频/音频轨只消费 RTP，不创建第二套解码、渲染或播放链路。
- 支持一个 Xbox 手柄的完整状态快照，包括按键、方向键、双摇杆和双扳机；使用有界非阻塞队列、状态合并、边沿保留、重连中立边界和输入静默 watchdog，避免阻塞 Sunshine 输入线程或重放旧按键。
- 支持解析 Xbox 的普通与扳机振动反馈，并把四马达强度转发到当前 Moonlight 手柄；无效、过期或旧会话反馈会被丢弃。
- 后台 worker 支持取消、分级错误、有限指数退避重试和会话 epoch 隔离；Moonlight 最后一条流结束时会发送中立状态、移除手柄、关闭 WebRTC 并删除 Home 会话，后续恢复会创建新会话。
- Web UI 和认证状态 API 只显示脱敏的状态、阶段、失败类型和 epoch。使用 RKMPP 串流时，认证、发现、唤醒、建连、失败和断开阶段还会临时显示 OSD，进入可用状态后自动清除。
- 独立探针已通过真实 Xbox 的认证、Home REST、SDP/ICE、四通道、启动握手、输入协议投递和清理验证；Sunshine 集成的最终 Moonlight→Sunshine→Xbox 按键、摇杆/扳机方向、振动、重连和故障真机矩阵仍待执行。
