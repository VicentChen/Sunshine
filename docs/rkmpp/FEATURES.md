本文档汇总当前代码已经实现的功能。

# RKMPP

- 在 Linux aarch64 的 Rockchip 平台提供 `hdmirx` 捕获后端和 `rkmpp` 编码后端；两者必须配套使用。
- 从 `/dev/video0` 的 `rk_hdmirx` V4L2 多平面设备捕获视频，默认向驱动请求四个 capture slot，支持 BGR3、NV24、NV16 和 NV12，并通过 DMA-BUF 把原始帧交给硬件链路，避免 CPU 像素复制和软件颜色转换。
- 使用 Rockchip MPP 编码 H.264 和 H.265，支持 IDR 请求及 Annex-B 参数集处理，并复用 Sunshine 现有的 RTP、FEC、加密和 UDP 发送链路。
- HDMI 输入尺寸与 Moonlight 请求尺寸一致时使用直通编码，不用 RGA 重新缩放视频；尺寸不一致时才使用 RGA 硬件缩放和格式转换，支持保持宽高比的黑边或居中裁剪。可见 Vulkan UI 的局部 ROI 覆盖独立于视频缩放，UI 隐藏时不执行该覆盖。
- HDMI RX 捕获会阻塞等待首帧，然后非阻塞丢弃积压旧帧并选择最新完整帧；主动 freshness drop、驱动丢帧和捕获年龄分别统计，避免信号恢复后继续编码过期画面。
- 直通路径按捕获 generation 与 buffer index 缓存 MPP 输入导入，RGA 路径按固定 target slot 缓存；fd 数值不作为跨重开身份，source change、encoder reconfigure 和 teardown 会使对应缓存失效。
- RKMPP 视频链路采用共享 capture、会话级 preprocess、会话级 encode 三段线程：capture 只做 V4L2 dequeue、恢复和帧扇出；preprocess 独占 RGA、Vulkan UI、私有 target 与 import 状态；encode 独占 MPP context 和码流 output pool。raw 与 prepared 两个交接点均为容量 1 的 latest-only mailbox，覆盖旧帧时立即释放 holder，并把未兑现的 IDR 粘滞转移到下一张实际提交帧。
- RGA 会话最多保留两个同格式可复用 target DMA-BUF：UI 隐藏的转换路径使用 NV12 target，UI 可见的隔离路径使用 BGR888 target；可见性导致格式切换时先等旧 lease 全部归还，再原子重建这两个 target。这样 frame N+1 的同步 RGA/Vulkan 可以与 frame N 的 MPP 编码重叠。一个 target 被 MPP 持有且另一个被旧 prepared frame 占用时，preprocess 优先丢弃旧 prepared 并立即回收；只有两个 target 都已被真实消费者取得时才进行可由 stop token 中断的等待，不动态扩池、不形成 FIFO backlog。
- HDMI RX 视频数据面采用 live-first 规则：首个有效 dequeue 帧立即进入编码，尺寸与 Moonlight 一致时直通 RKMPP，不一致时从第一帧使用 RGA；EDID 状态、640x480 timing、Xbox 状态和 source change 都不能门禁真实帧。绿色占位帧只在当前确实取不到 HDMI 帧时产生，source change 仅负责 capture queue 和 generation 恢复。
- 进程级 EDID 控制器独占写权限，encoder probe 严格只读。能力探测和正式编码会话初始化都使用目标尺寸的合成占位输入验证 RKMPP/RGA，不依赖当时能否 dequeue HDMI 帧；只有 capture loop 能发布真实 HDMI 帧。控制器从校验有效的原生 EDID 解析 Established Timing、Standard Timing、base/CTA DTD、CTA VDB 和 Y420 VDB，选择最接近 Moonlight 请求的原生模式，再把它提升为首选并过滤所有高于目标的模式；低分辨率原生 timing、接收器身份、音频、speaker allocation、HDMI VSDB/HF-VSDB 等兼容能力继续保留，避免目标孤立 EDID 使主机回落到 640x480。
- EDID 字节写入与 HDMI 模式协商是两个独立结果。实际 HDMI timing 已匹配选中原生模式时零写入；不匹配时每个 streaming display 最多执行一次 EDID/HPD 事务，即使相同字节已安装也会重新声明目标。readback 只验证接收器保存的 EDID，随后由独立 verifier 在五秒窗口内以实际捕获尺寸确认主机是否采用目标；失败会明确记录实际尺寸并继续通过 RGA 串流，不再伪报分辨率切换成功。source change、Xbox wake 和会话析构本身不能触发额外写入；异常写入只允许一次原生恢复。
- HDMI 热插拔使 PulseAudio 录音流失效时，Linux 音频采集会重建输入流，而不是永久结束本次会话的音频。
- 直通、RGA 创建和运行时 reconfigure 共用同一套 MPP 配置构造，Moonlight 请求的帧率、分数帧率、码率和 GOP 不再在直通路径静默回落到默认值。
- HEVC 会话启用 MPP `h265:auto_tile`，让 RK3588 把单帧划分为两个硬件 tile 并交给两个 RKVENC core 并行编码；H.264 会话保持单 core 配置。1080p60 与 4K60 实机测试中两个 core 的中断计数同步增长，确认配置已进入双 core 数据面。
- 编码输出使用六个固定的 non-contiguous system DMA-HEAP slot；encoder 创建时一次性分配并通过 `mpp_buffer_get_ptr()` 预热 CPU 映射，运行时不再逐帧分配或映射 output buffer。单 slot 容量为 `max(8 MiB, aligned_input_frame_size)`，4K NV12 实机会话为 12,441,600 bytes；pool 满时最多有界等待 250 ms，不允许动态扩容。`encoded_packet_t` 持有 slot lease 到网络消费者释放，避免复用仍在发送的码流。MPP 1.3.9 的公开 API 无法可靠清空全部 packet metadata/segment 状态，因此保留每帧新建轻量 `MppPacket` wrapper 的安全方案。普通 P 帧会跳过不必要的 Annex-B IDR 扫描。
- Web UI 可选择 `HDMI RX (Rockchip)` 和 `Rockchip MPP`，并配置延迟统计、低延迟实验和关闭重编码实验选项。旧 MPP palette OSD、`rkmpp_profile_overlay` 配置项及 Web UI 开关已经删除；性能采样由 `rkmpp_profile` 独立控制，显示统一由 Vulkan UI 负责。
- Vulkan UI 使用固定版本的 Dear ImGui core 与官方 Vulkan renderer backend 离屏绘制不透明 BGR 诊断页；页面尺寸、字体和留白按 Moonlight 输出分辨率及用户选择的紧凑/标准/大号档位缩放。单会话、尺寸匹配的 BGR888 capture 由 Vulkan 直接覆盖其 DMA-BUF ROI，不经过 RGA；NV12、尺寸不匹配或多会话共享输入先由 RGA 完整转换到会话私有 CMA BGR888 target，再由 Vulkan 直接覆盖该 BGR target，绝不把 Vulkan 结果用 RGA 拷回 NV12 capture。UI 隐藏且尺寸/layout 匹配时保持零拷贝直通。不创建 GLFW/SDL 窗口或 swapchain；相同 revision 复用缓存。
- Profile Timeline 把最多 32 个最近完成帧按 RX EOF 对齐后得到的平均视图放在上方，下方只显示最新一个完成帧，避免多帧实时条密集闪烁。每个 Event 的名称通过竖直引线放在彩条下方，并以前置同色块强化对应关系；相互碰撞的标签自动增加标注行。`RAW QUEUE` 表示 dequeue 到 preprocess 开始，`PREPARED QUEUE` 表示预处理完成到 encode worker 取走，RGA/UI 与 MPP 位于真实线程 lane，可直接观察相邻帧重叠。窗口同时累计 `raw_replaced`、`prepared_replaced`、`target_waits` 和 sticky IDR transfer。MPP 阶段按实际顺序显示为 `MPP OUT BUF`、`MPP PACKET INIT`、`MPP PREP`、`MPP ENCODE` 和 `MPP PACKET GET`；其中 `MPP ENCODE` 覆盖阻塞式 `encode_put_frame()` 的驱动排队、硬件编码与收尾，`MPP PACKET GET` 仅表示完成 packet 的取回。
- Vulkan UI 的 BGR surface 使用适合 Vulkan external-buffer import 的对齐行跨度；Compact（85%）Profile 不再因未对齐的 1224 像素可见宽度创建失败。尺寸切换会先完整建立一组新 surface 再原子替换，运行时创建失败则保留最后一组可用 surface，不再使整个 UI session 失效。
- 线程安全的 UI controller 支持先按住 `Start`、再点按 `Back/Select` 立即开关 UI；Start 修饰键会先被截获，未组成快捷键时在松开后补发普通 Start 点击。实现同时保留完整释放门、owner、全局输入截获、D-pad/左摇杆焦点导航和断开/reset 清理。
- UI controller 的输入截获由成功初始化的 renderer backend 引用计数门控；最后一个后端退出或运行失败时会立即关闭模态并清理按键状态。HDMI RX capture generation 在 UI 可见性判断前更新，source recovery 会主动销毁两个页面 renderer 的旧 BGR DMA-BUF import，即使 UI 当时隐藏也不会复用旧 allocation。
- Vulkan UI 已完成 1080p/4K 实机集成验收，包括三段式 preprocess 改造后的 BGR888 直接 ROI cover 与 NV12→私有 BGR888→Vulkan 路径、连接状态自动显示/隐藏、Profile、手柄组合键打开、导航、输入截获、关闭和 neutral cleanup。旧固定 MPP OSD 已删除，UI 隐藏、源变更、无信号、重连及 teardown 路径也已验收。
- 可通过 `audio_source` 选择 HDMI RX 对应的 PulseAudio/PipeWire Source，并复用 Sunshine 的 float PCM → Opus 音频链路；空值保持原有 `audio_sink` monitor 回退。PulseAudio 生命周期改由 `pa_threaded_mainloop` 和 mainloop lock 串行化，避免断流 teardown 与事件清理并发。ROCK 5B+ 已识别 ALSA `hw:3,0` 和对应的双声道 PipeWire Source，支持的枚举格式为 S16LE、S24_32LE、S32LE、32–192 kHz。Source 优先级和 monitor 回退已有自动测试；真实 PCM、Moonlight 播放、断开恢复及 A/V 同步尚未完成实机验收。

# NS

- 内置 `Nintendo Switch` 应用把 Moonlight 手柄输入按应用路由到 NXBT Bridge；这是 NS 功能在代码中的精确应用名，其他应用不会误发输入到 Switch。
- 支持 `virtual`、`nxbt` 和 `both` 三种手柄输出模式，可只输出到主机虚拟手柄、只输出到 Switch，或同时输出到两者。
- Sunshine 与独立 NXBT Bridge 通过受限的本机 Unix `SOCK_SEQPACKET` 通信；协议具有版本握手、定长二进制消息、序列号、心跳、连接状态和错误状态。
- 支持 A/B/X/Y、方向键、PLUS/MINUS、HOME、CAPTURE、L/R、L3/R3、双摇杆和数字化 ZL/ZR；支持按键标签/位置两种面键策略、摇杆校准及可配置的扳机按下/释放阈值。
- 支持把 Moonlight Android 虚拟手柄的长按 BACK 转换为 HOME，同时保留短按 BACK 到 MINUS 的映射。
- Bridge 会优先重连 BlueZ 中已保存的 Switch；没有可用的已配对设备时可进入首次配对流程。首次配对后，已有实机记录证明可从 Switch 普通 HOME 页面自动快速重连。
- 输入发送使用非阻塞的最新完整状态模型；Sunshine/Bridge 断开、应用切换、手柄释放或 watchdog 超时时会发送中立状态并释放资源，降低粘键风险。
- 提供安装、卸载、部署前检查和硬件验证脚本，并以受限 socket 权限及独立 systemd 服务运行 Bridge。
- 当前首版只支持一个活动 NXBT 手柄和一台正在接收输入的 Switch。真实 Moonlight→Sunshine→Switch 全键位、重启后重连、30 分钟 soak，以及 Moonlight/Sunshine/Bridge/Switch/蓝牙断开恢复与 watchdog 矩阵均已完成实机验收。

# Xbox

- 在受支持的 Linux 构建中，内置 `Xbox` 应用默认自动启动 Xbox Remote Play 手柄回传；可配置精确应用名或关闭功能，非 Xbox 应用不会建立会话、唤醒主机或接收该路由的手柄输入。
- 提供 `xbox-remote-probe` 完成 Microsoft Device Code 登录、凭据恢复、主机枚举、唤醒、REST 会话、WebRTC、启动握手、输入和 soak 诊断；OAuth 凭据保存在属主专用的 `0600` 文件中，Xbox/XSTS/GSSV 临时令牌只保留在内存。
- 账号只有一台 Home Xbox 时可自动选择；多台主机时可通过稳定 ID 显式选择。会话可按配置执行 XCCS WakeUp 门禁。
- 支持 Home Remote Play 会话创建、状态轮询、SDP/ICE 交换、四条必需 WebRTC data channel、启动握手、keepalive 和会话删除；视频/音频轨只消费 RTP，不创建第二套解码、渲染或播放链路。
- 支持一个 Xbox 手柄的完整状态快照，包括按键、方向键、双摇杆和双扳机；使用有界非阻塞队列、状态合并、边沿保留、重连中立边界和输入静默 watchdog，避免阻塞 Sunshine 输入线程或重放旧按键。
- 支持解析 Xbox 的普通与扳机振动反馈，并把四马达强度转发到当前 Moonlight 手柄；无效、过期或旧会话反馈会被丢弃。
- 后台 worker 支持取消、分级错误、有限指数退避重试和会话 epoch 隔离；Moonlight 最后一条流结束时会发送中立手柄状态，并默认保留 Home 会话和 WebRTC 连接 300 秒供快速恢复。保活时间可配置，窗口内恢复会复用同一 worker，超时后恢复会创建新 worker 并迁移保留的手柄；明确退出应用仍会立即移除手柄、关闭 WebRTC 并删除 Home 会话。
- Web UI 和认证状态 API 只显示脱敏的状态、阶段、失败类型和 epoch。使用 RKMPP 串流时，认证、发现、唤醒、建连、失败和断开阶段通过 Vulkan UI 连接状态页显示，进入可用状态后自动隐藏。
- 独立探针已通过真实 Xbox 的认证、Home REST、SDP/ICE、四通道、启动握手、输入协议投递和清理验证；Sunshine 集成的 Moonlight→Sunshine→Xbox 按键、摇杆/扳机方向、普通/扳机振动、重连和故障真机矩阵也已完成。会话建立与退出期间的 CPU、线程、Remote Play 网络、系统调频和风扇响应已一并核查。
