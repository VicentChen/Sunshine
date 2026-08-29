# Moonlight → Sunshine → Xbox Remote Play 手柄回传分步实施计划

> 状态：待实施
>
> 目标平台：ROCK 5B+ / RK3588 / Linux aarch64
>
> 首版目标：Xbox 与 ROCK 5B+ 位于同一局域网；视频来自 HDMI RX；Xbox Remote Play 连接只承担手柄输入和振动反馈
>
> 参考实现：`../LunarNX` 和 `../XStreaming`
>
> 编码前预研记录：`docs/rkmpp/feature/xbox-remote-play-validation/preflight.md`

## 1. 最终目标

在不修改 Moonlight 协议、不依赖 Xbox Remote Play 音视频输出的前提下，实现以下双向链路：

```text
Moonlight 客户端物理手柄
  → Sunshine GameStream 输入解析
  → Sunshine gamepad router
  → Xbox Remote Play input data channel
  → Xbox

Xbox vibration packet
  → Xbox Remote Play input data channel
  → Sunshine feedback_queue
  → Moonlight 客户端物理手柄

Xbox HDMI 输出
  → ROCK 5B+ HDMI RX
  → Sunshine RKMPP 视频串流
  → Moonlight
```

已确认前提：ROCK 5B+ 的 `HDMI RX → Sunshine → Moonlight` 视频链路已经验证可用。本计划不重复验证其基础功能，只在最终验收中检查新增 Remote Play 通道是否造成回归。

首版完成后必须满足：

- ROCK 5B+ 能通过 Microsoft/Xbox 服务认证并发现指定的家庭 Xbox 主机。
- 能创建、维持和显式删除 Xbox Home Remote Play session。
- WebRTC 连接保留 Xbox 所需的音频、视频和四条 data channel 协商，但不解码、不播放和不渲染媒体。
- Moonlight 的按键、方向键、摇杆和扳机能控制 Xbox，断线后不会残留卡住的输入。
- Xbox 返回的普通和扳机振动能通过 Sunshine 回传给 Moonlight 客户端。
- Remote Play 认证、信令或传输失败不会阻塞 Sunshine 输入线程，也不会破坏现有 virtual-HID 或 NXBT 输出。
- 所有新增或修改的 C/C++ API 都有 Doxygen；新增逻辑有 gtest，changed code 以 100% 覆盖为目标。

## 2. 首版边界

### 2.1 首版包含

- Microsoft Device Code OAuth 登录和 refresh token 刷新。
- Xbox User Token、XSTS 和 xHome GSSV token 链。
- 家庭主机发现和显式主机选择。
- Home session 的 play、state、configuration、SDP、ICE、keepalive 和 delete。
- 一个 Remote Play session、一个 Xbox 主机和至少一个逻辑手柄。
- `control`、`input`、`message`、`chat` 四条兼容 data channel。
- 完整手柄状态发送、短期数字按键边沿保留、latest-state-wins 队列。
- Xbox 振动反馈。
- LAN 内连接、断线归零、自动重连和显式停止。
- 只在用户选择的 Xbox/HDMI Sunshine 应用生命周期内建立 Remote Play session。

### 2.2 首版不包含

- Xbox Cloud Gaming。
- WAN/Teredo/复杂 NAT 的兼容承诺。
- Xbox Remote Play 视频解码、音频解码、渲染、录制或与 HDMI RX 混流。
- 触摸、鼠标、键盘、聊天音频和屏幕键盘协议。
- 多 Xbox session 并行运行。
- 自动开启、唤醒或关闭 Xbox。
- 在日志、诊断包或配置导出中暴露 access token、refresh token、XSTS token 或 `gsToken`。

## 3. 已确认的架构决策

### 3.1 先做独立兼容性探针，再接入 Sunshine

Sunshine 集成前先实现独立的 `xbox-remote-probe`。在真实 Xbox 上完成认证、WebRTC 和最小按键闭环前，不修改 Sunshine 热路径。

这是一个硬门槛。若候选 WebRTC 库不能接受 Xbox 的 SDP、ICE、DTLS/SCTP 或媒体协商，必须在探针阶段更换或适配传输层，不能把未验证的传输依赖直接引入 Sunshine。

探针通过后，正式实现可选择：

1. 首版继续使用独立 `xbox-remote-bridge`，通过本机 Unix Domain Socket 与 Sunshine 通信。
2. 在传输库对所有 Sunshine 目标平台和构建系统影响可控时，将相同模块内置到 Sunshine。

步骤 7 必须根据构建体积、运行稳定性和跨平台影响记录最终选择。无论采用哪一种，协议层、session 状态机和输入编码器都不得依赖 Sunshine 输入线程。

### 3.2 Remote Play session 与 Sunshine 应用生命周期绑定

Remote Play 建连可能持续数秒，不能在 `gamepad::sink_t::alloc()` 或 `update()` 中同步执行。

- 启动 Xbox/HDMI 应用时异步创建或预热 session。
- `sink_t::alloc()` 只接受逻辑手柄绑定并保存 feedback queue。
- `sink_t::update()` 只提交完整状态到有界队列，不执行 HTTP、DNS、磁盘 I/O 或阻塞式 WebRTC 操作。
- 应用停止时先发送中立状态和 `gamepadChanged: false`，再关闭 peer connection 并删除服务器 session。
- Moonlight 短时断线和 Remote Play 断线分别管理，不能混成同一个状态。

### 3.3 保留完整 WebRTC 协商，媒体只做 no-op 消费

首版 SDP 必须包含 Xbox 兼容的媒体和应用部分：

- video：`recvonly`。
- audio：按真机兼容结果使用 `recvonly` 或服务要求的最小方向。
- application：SCTP data channel。

必须创建以下四条 data channel：

| Label | Protocol | 初始策略 |
| --- | --- | --- |
| `control` | `controlV1` | reliable + ordered |
| `input` | `1.0` | reliable + ordered |
| `message` | `messageV1` | reliable + ordered |
| `chat` | `chatV1` | reliable + ordered |

初始实现按 LunarNX 当前真机路径使用偶数 SID `0/2/4/6`。如候选库自行协商 SID，必须从生成的 SDP 和运行日志证明最终通道参数符合 Xbox 要求。

音视频数据不得进入解码器，但仍需消费收到的 RTP 并维持必要的 RTCP/transport 状态，防止接收队列无界增长。不得把“丢弃媒体”描述为“不产生 Remote Play 音视频流量”。

### 3.4 输入使用完整快照和有界队列

- Sunshine 每次提交的是完整 `platf::gamepad_state_t`。
- 同一手柄的未发送模拟状态采用 latest-state-wins。
- `attach`、`neutralize`、`detach` 和数字按键 press/release 边沿不得被普通状态覆盖。
- 数字边沿 journal 只保留短时间，初始值参考 LunarNX 的 50 ms；过期边沿不得在重连后重放。
- Xbox input sequence 在实际发送时分配，必须连续且只由 WebRTC 发送线程拥有。
- 建连或重连完成后只发送当前绝对状态；不得回放连接期间积压的旧输入。
- 停止、超时或异常断线时必须产生可验证的中立状态。

### 3.5 凭据默认安全存储

- token 文件只能由当前 Sunshine 服务用户读取和写入，权限必须为 `0600`。
- 写入使用临时文件、`fsync` 和原子 rename，避免断电产生半个 JSON。
- 日志只允许记录 token 类型、到期时间和不可逆短标识，不记录 token 内容。
- 配置和诊断导出必须执行字段级脱敏。
- 如果目标系统提供合适的 secret service，可以后续替代文件存储；首版不得因此阻塞。

## 4. 参考实现采用规则

### 4.1 从 LunarNX 参考

- `src/auth/auth_manager.*`：Device Code OAuth、Xbox User Token、XSTS 和 GSSV 登录顺序。
- `src/api/xbox_api_client.*`：Home session REST 形状和错误处理。
- `src/app/xbox_session_client.*`：`Provisioned` 状态等待和取消。
- `src/app/xbox_channel_manager.*`：message handshake、control authorization、gamepad remove/add 和启动能力消息。
- `src/webrtc/xstreaming_data_channels.h`：当前使用的四通道参数。
- `src/input/xinput_encoder.*`：C++ 输入编码、physicality 和 metadata。
- `tools/mock_xbox` 及相关测试：无 Xbox 环境的协议回归方法。

不得直接移植 LunarNX patched libpeer、Switch UI、媒体解码器或平台补丁。

### 4.2 从 XStreaming 参考

- `src/xCloud/index.ts`：Home session 信令和状态语义的交叉验证。
- `src/webrtc/Channel/*`：control/message/input 通道消息。
- `src/webrtc/Packet/index.ts`：二进制包的协议基准。
- `src/webrtc/index.ts`：完整 SDP 和 data channel 建立顺序。
- `nano-rs/src/webrtc_backend.rs`：no-op media、统计和异步运行结构的参考。

不得引入 React Native、JNI、UI、媒体播放、store、云游戏目录或触摸层。不得未经验证照搬 nano-rs 对 `/connect` 的调用条件。

### 4.3 许可证记录

实现阶段必须在验证记录中保存所参考文件的 commit 和许可证。若复制或派生 MIT 代码，必须保留所需版权和许可证声明；不得复制 LunarNX 的 AGPL PlayStation/Chiaki 路径。

## 5. 全局工程与验证规则

所有阶段必须遵守：

- 开始和结束时记录 `git status --short`，保留全部无关未提交修改。
- 不创建 GitHub issue 或 pull request。
- 新增或修改的 C/C++ API、类型、字段和非显然逻辑必须有符合仓库格式的 Doxygen。
- 使用 `.clang-format` 格式化修改的 C/C++ 文件。
- 新增或修改的方法必须添加 gtest；changed code 以 100% 覆盖为目标。
- 测试中的 HTTP、时间、随机 ID、token 和 WebRTC transport 必须可注入或可替换，不能依赖真实 Xbox 才覆盖错误分支。
- 本功能增加本地化时只修改 `en`，不得修改 `en-US` 或其他语言。
- Sunshine 构建有且只有一个入口，不得指定或覆盖构建目录：

```bash
./scripts/build-rkmpp.sh
```

- 测试必须使用上述构建产生的 `test_sunshine`，不得从其他构建目录复用旧产物。
- Windows 回归构建只能在 Windows/MSYS2 UCRT64 环境执行，并使用仓库规定的命令前缀。
- 没有真实 Xbox、Moonlight 客户端或 ROCK 5B+ 时，不得伪造硬件步骤通过。
- 会登录 Microsoft 账号、启动 Xbox、创建 Remote Play session 或改变主机状态的操作必须在验证记录中明确标记。
- 每阶段证据写入：

```text
docs/rkmpp/xbox-remote-play-validation/step-XX.md
```

验证记录不得包含 token、完整 Xbox ID、Microsoft 账号、内网地址或公网地址。

## 6. 分步实施计划

### 步骤 1：冻结 Xbox 协议契约和 golden fixtures

#### 目标

在引入网络和 WebRTC 库前，先把要实现的协议形状变成自动化测试契约。

#### 工作项

- 定义 Home session 所需的最小请求/响应模型。
- 固定四条 data channel 的 label、protocol、可靠性和顺序策略。
- 固定 message handshake、authorization、gamepad remove/add 和六类启动能力消息。
- 固定 input header、ClientMetadata、Gamepad frame 和 Vibration frame 字段。
- 从两个参考项目生成脱敏 golden JSON/binary fixtures，不提交真实 token、host ID、IP 或 session ID。
- 明确 Home session 只等待 `Provisioned`；`ReadyToConnect` 和 `/connect` 不属于首版正常路径。

#### 自动验证

- JSON 序列化后与 golden fixture 进行语义比较。
- 二进制 fixture 做 byte-for-byte 比较。
- 用测试证明未知 JSON 字段可忽略、缺失必需字段会返回结构化错误。
- 用测试证明所有多字节数字显式按 little-endian 编解码。

#### 通过标准和产物

- `step-01.md` 列出每个 fixture 的来源文件和 commit。
- 协议测试无需网络和 Xbox 即可运行并全部通过。
- fixtures 扫描不包含 JWT 形状、refresh token、真实 IP、Xbox ID 或账号信息。

---

### 步骤 2：实现认证链和安全 token store

#### 目标

独立完成 Device Code 登录、刷新和 Xbox/GSSV token 链，不依赖 WebRTC。

#### 工作项

- 实现 Device Code 获取、用户提示、轮询、取消和超时。
- 实现 refresh token 刷新和过期时间判断。
- 实现 Microsoft access token → Xbox User Token → XSTS → xHome GSSV 登录。
- 将 GSSV `baseUri`、`gsToken` 和到期时间保存在内存 session context 中。
- 实现权限为 `0600` 的原子 token store。
- 对所有日志和错误对象做 token 脱敏。

#### 自动验证

- fake HTTP 覆盖成功、`authorization_pending`、`slow_down`、用户拒绝、过期、网络错误、HTTP 非 2xx 和畸形 JSON。
- fake clock 覆盖 token 尚有效、提前刷新、已过期和系统墙钟变化。
- 临时目录测试文件权限、原子替换和损坏文件恢复。
- 日志捕获测试断言任何测试 token 均未出现在日志中。

#### 真机验证

- 使用测试账号完成一次 Device Code 登录。
- 终止并重启探针，证明可以通过 refresh token 恢复，不再次要求用户输入代码。
- 主动使 access token 过期或使用 fake clock，证明刷新成功。

#### 通过标准和产物

- 自动化测试全部通过。
- `step-02.md` 只记录各 token 阶段的成功/失败、到期时间区间和脱敏 ID。
- token 文件实际权限为 `0600`，输出日志中搜索不到 token 测试值和真实凭据。

---

### 步骤 3：实现主机发现和 Home session REST 状态机

#### 目标

在不建立 WebRTC 的情况下，能够发现 Xbox、创建 session、等待 `Provisioned`、keepalive 并可靠删除 session。

#### 工作项

- 实现主机发现和稳定的主机选择标识。
- 实现 `POST /v5/sessions/home/play`。
- 实现 `/state` 轮询、`/configuration` 解析和 keepalive pulse。
- 实现 SDP、ICE endpoint 的请求模型，但本步骤可不连接 peer connection。
- 实现显式 cancel、超时、服务器失败状态和幂等 session delete。
- HTTP 请求必须支持截止时间，不能无限等待。

#### 自动验证

- 使用 fake server 覆盖 `Provisioning → Provisioned`。
- 覆盖未找到主机、重复主机、401 刷新后重试、404、429、500、超时、取消和畸形响应。
- fake clock 验证 keepalive 调度，不使用真实长时间 sleep。
- 验证正常停止和所有失败分支都会尝试删除已创建的 session。

#### 真机验证

- 列出目标 Xbox，创建一个 session，观察到 `Provisioned` 后立即删除。
- 重复执行 5 次，Xbox 服务端不得残留可见的旧 session，探针不得出现线程或文件描述符增长。

#### 通过标准和产物

- fake server 测试全部通过。
- 5 次真机 create/delete 全部成功；若服务器没有 session 查询接口，则以 DELETE 响应和下一次 create 成功作为可验证证据，并在记录中说明限制。
- `step-03.md` 包含各状态耗时、HTTP 状态码摘要和脱敏后的错误样例。

---

### 步骤 4：WebRTC 传输库兼容性探针（硬门槛）

#### 目标

在 ROCK 5B+ 上证明候选 WebRTC 库可以与真实 Xbox 完成完整连接。

#### 工作项

- 首选评估原生 C++ WebRTC 传输库；若选择 libdatachannel，必须启用 media transport，不能使用 `NO_MEDIA` 构建。
- 创建 video、audio 和 application SDP 部分。
- 在生成 offer 前创建四条 data channel。
- 实现本地 ICE 收集、POST 和远端 ICE 轮询/注入。
- 设置 no-op RTP 接收回调并持续消费媒体；不初始化解码器。
- 记录 peer、ICE、DTLS 和 data channel 状态变化，不记录敏感 SDP 地址；保存日志时必须脱敏候选地址。
- 实现 peer connection 的超时、取消和确定性销毁。

#### 自动验证

- 使用脱敏 SDP fixtures 验证 offer/answer 解析、codec m-line、SCTP 参数和 ICE 候选转换。
- mock transport 验证事件乱序、重复 candidate、channel 提前关闭、ICE failed 和取消。
- 资源测试验证重复构造/销毁 peer 不泄漏线程、socket 或 callback owner。

#### 真机验证

- 在 ROCK 5B+ 与 Xbox 同一 LAN 下建立连接。
- 四条 data channel 均进入 open。
- 连续保持 10 分钟并发送 keepalive。
- 证明收到音频/视频 RTP 但没有创建解码器；接收队列、RSS 和线程数不随时间持续增长。
- 关闭探针后 peer、socket 和服务器 session 都被释放。

#### 通过标准和产物

- `step-04.md` 记录所选库、精确版本/commit、许可证、构建选项和依赖体积。
- 四条通道全部 open，10 分钟保持成功，无未界定增长和无残留 session。
- 若任一项失败，本步骤为 `BLOCKED`，不得进入 Sunshine 集成；记录 Xbox SDP 不兼容点并更换/修复 transport。

---

### 步骤 5：实现 Xbox data channel 启动握手

#### 目标

在不发送实际按键的情况下，使 Xbox 接受客户端身份、手柄声明和客户端能力。

#### 工作项

- `message` open 后发送 `Handshake/messageV1` 并等待 `HandshakeAck`。
- Ack 后在 `control` 发送 `authorizationRequest`。
- 发送 `gamepadChanged: false`，等待 500 ms，再发送 `gamepadChanged: true`。
- 发送 system UI、install ID、orientation、touch disabled、device capabilities 和 dimensions 消息。
- 在 `input` 发送 ClientMetadata。
- 所有等待必须可取消且有明确超时；重复 Ack 或消息乱序不能重复添加手柄。

#### 自动验证

- mock channel 对发送顺序、通道选择和 JSON 内容做精确断言。
- fake clock 验证 500 ms 间隔和 handshake timeout。
- 覆盖 Ack 缺失、错误版本、通道关闭、发送失败、取消和重复 Ack。

#### 真机验证

- Xbox 连接保持至少 5 分钟，没有发送 gamepad report。
- 日志显示 HandshakeAck、authorization、remove/add、能力和 metadata 依次完成。
- Xbox 不应因协议超时主动关闭 data channel。

#### 通过标准和产物

- 自动化测试全部通过。
- 真机连接在只完成初始化、不发送按键时稳定保持 5 分钟。
- `step-05.md` 给出脱敏状态时间线和最终通道状态。

---

### 步骤 6：实现输入编码、调度和振动解析

#### 目标

完成与网络无关、可 byte-for-byte 验证的双向手柄协议层。

#### 工作项

- 实现 14-byte header、ClientMetadata 和 23-byte Gamepad frame。
- 实现 Xbox button mask、physical physicality 和 virtual physicality。
- Sunshine `uint8` 扳机按 `value * 257` 映射到 Xbox `uint16`。
- 摇杆保留 `int16`，Y 轴方向通过真机测试确定，不盲目继承其他平台翻转。
- 实现实际发送时 sequence 和 timestamp 标记。
- 实现 bounded latest-state slot 和数字边沿 journal。
- 解析 Xbox vibration report，校验 report type、长度、手柄 index 和四马达值。

#### 自动验证

- 38-byte 单手柄包与 golden fixture byte-for-byte 一致。
- 覆盖所有按键位、最小/中心/最大摇杆、0/1/254/255 扳机和 physicality。
- 覆盖 sequence wrap、最新状态覆盖、边沿保留、journal 过期和重连清空。
- fuzz/参数化测试畸形、截断、超长和未知 vibration packet，解析器不得越界或抛出未捕获异常。

#### 真机验证

- 只发送中立状态，然后分别测试 A、D-pad Up、左右摇杆四方向、LT 和 RT。
- 每个测试动作均使用“按下/移动 → 保持 → 释放/回中”，观察 Xbox 端结果。
- 在 Xbox 端触发普通振动和扳机振动，验证探针记录正确马达值。

#### 通过标准和产物

- 自动化协议测试全部通过。
- 真机矩阵中每个方向、按钮和扳机均与预期一致；由该步骤明确 Sunshine Y 轴是否需要翻转。
- 释放后 Xbox 不存在持续按键、摇杆偏移或扳机残留。
- `step-06.md` 保存输入矩阵和振动矩阵，不保存账号或网络信息。

---

### 步骤 7：完成独立端到端探针并确定部署形态

#### 目标

在修改 Sunshine 前，证明完整最小产品可以在 ROCK 5B+ 长时间工作，并决定 sidecar 或内置模式。

#### 工作项

- 将步骤 2–6 组合为单个可取消的后台状态机。
- 状态至少包含：`idle/authenticating/discovering/provisioning/connecting/handshaking/ready/reconnecting/stopping/failed`。
- 所有状态转换输出结构化原因和单调时间。
- 采集 CPU、RSS、线程数、文件描述符、Remote Play 网络吞吐和丢包/RTT 统计。
- 比较 sidecar 与内置所需依赖、二进制体积、跨平台构建影响和故障隔离。
- 形成架构决策记录：首版使用 `xbox-remote-bridge` 或 Sunshine 内置模块。

#### 自动验证

- fake transport/fake server 覆盖每个状态的成功、失败、取消和 cleanup 转移。
- 反复启动/停止 100 次使用 fake transport，不得有线程、FD、session handle 或 callback 泄漏。
- 队列压力测试证明 `submit_state()` 有界且不阻塞，旧模拟状态会被覆盖。

#### 真机验证

- 真实 Xbox 连续运行 30 分钟。
- 期间完成至少 500 次离散 press/release 和持续摇杆圆周输入。
- 不解码媒体，记录 5 分钟稳定窗口内的 CPU、RSS 和网络吞吐。
- 正常停止后 Xbox 输入归零，session 删除，进程退出。

#### 通过标准和产物

- 30 分钟内无粘键、通道异常关闭、keepalive 失败或持续资源增长。
- `step-07.md` 包含资源曲线摘要和部署形态决策及理由。
- 只有本步骤为 `PASS` 才能开始修改 Sunshine 输入路由。

---

### 步骤 8：实现 Sunshine Xbox Remote Play sink

#### 目标

在不改变现有输入解析的情况下，将已验证的 Remote Play 客户端接入 `gamepad::sink_t`。

#### 工作项

- 增加 `xbox_remote_sink`，实现 `alloc/rebind/update/neutralize/free`。
- `alloc()` 只登记手柄和 feedback queue，不等待 session ready。
- `update()` 只向有界队列提交完整状态，并满足现有 non-blocking 契约。
- `neutralize()` 必须产生高优先级中立状态。
- `free()` 解除逻辑手柄；是否保持预热 session 由应用生命周期决定。
- 将振动映射为 Sunshine `make_rumble()` 和 `make_rumble_triggers()` feedback 消息。
- 保持 virtual-HID 和 NXBT sink 行为不变。

#### 自动验证

- fake session 验证 alloc/rebind/update/neutralize/free 调用顺序和失败隔离。
- 验证 `update()` 在 session 未 ready、正在重连和已 failed 时均有确定、非阻塞行为。
- 验证普通/扳机振动使用正确 `clientRelativeIndex` 回传。
- 路由测试覆盖 virtual、NXBT、Xbox Remote Play 及明确支持的组合模式。
- 慢 fake sink 不得被放在 input parser 线程直接执行阻塞操作。

#### 通过标准和产物

- 所有 gamepad router 和 Xbox sink gtest 通过。
- 现有 virtual-HID/NXBT 测试结果与基线一致。
- `step-08.md` 记录最终线程所有权、队列容量、溢出规则和每个 sink 方法的阻塞契约。

---

### 步骤 9：绑定 Sunshine 应用生命周期、配置和状态可见性

#### 目标

让用户可以只对 Xbox/HDMI 应用启用 Remote Play 手柄回传，并能完成首次登录、主机选择和故障诊断。

#### 工作项

- 增加明确的 Xbox Remote Play enable/output 配置，不默认改变现有用户行为。
- 将 session start/stop 绑定到目标 Sunshine 应用，而不是绑定到第一个手柄包。
- 提供 Device Code、登录状态、脱敏账号标识、主机选择和 session 状态入口。
- 只有显式选择的 Xbox 应用可以启动 session。
- 配置校验拒绝缺失主机、不可写 token 路径、无效输出组合和不支持的平台。
- 如果增加本地化，只更新 `en`。

#### 自动验证

- 配置默认值证明功能默认关闭。
- 应用 A 启用、应用 B 未启用的生命周期测试证明 B 不会创建 Xbox session。
- start/cancel/stop、重复 start、Moonlight reconnect 和 Sunshine shutdown 均有状态机测试。
- ConfigConsistencyTest 和相关 Web UI/API 测试通过。

#### 人工验证

- 从干净 token store 完成一次 Device Code 登录和主机选择。
- 启动非 Xbox 应用，确认没有 Remote Play 网络活动。
- 启动 Xbox/HDMI 应用，确认 session 进入 ready；停止应用，确认中立状态和 DELETE 完成。

#### 通过标准和产物

- 默认配置对现有 Sunshine 行为零变化。
- 状态页面和日志可以区分认证、Xbox 不在线、Provisioning、ICE、Handshake 和输入通道故障。
- UI、日志和配置导出不包含任何 token。
- `step-09.md` 记录用户操作流程和各失败状态截图/文本，敏感信息必须脱敏。

---

### 步骤 10：重连、超时、清理和故障注入

#### 目标

证明 Xbox、网络、Moonlight 或 Sunshine 任一侧异常时都不会粘键、泄漏 session 或阻塞输入。

#### 工作项

- 对 HTTP、ICE、handshake、data channel 和 keepalive 分别设置截止时间与退避。
- 区分可恢复错误、需要重新认证错误和永久配置错误。
- 重连开始时清空旧 transition journal；ready 后发送当前绝对状态。
- 实现 session epoch，忽略旧 peer connection 的延迟 callback 和 vibration。
- 所有退出路径执行：中立状态、gamepad remove、peer close、session delete；已断网时记录 delete 未确认。
- Sunshine shutdown 必须可在有网络调用进行时被及时取消。

#### 自动验证

- 在每个状态注入一次错误并断言最终资源和状态。
- 覆盖 Xbox 关机、401、429、HTTP timeout、ICE failed、DTLS failed、channel close、keepalive failed 和 malformed packet。
- 验证旧 epoch callback 不会影响新 session。
- watchdog 测试证明停止接收 Moonlight 状态后会发送中立状态。

#### 真机验证

- 按住一个按键时依次执行 Moonlight 断开、拔掉 ROCK 网络、停止 Sunshine、关闭 Xbox Remote Play session。
- 恢复网络后完成一次自动重连。
- 每种故障恢复后先观察中立状态，再输入新动作；旧 press 不得重放。

#### 通过标准和产物

- 故障矩阵每项都有预期状态、实际状态、归零结果和 cleanup 结果。
- 所有可恢复场景能在设定重试预算内恢复；不可恢复场景给出可操作错误且停止无限重试。
- 没有测试场景留下粘键、后台线程、socket 或持续 keepalive。
- `step-10.md` 包含完整故障矩阵。

---

### 步骤 11：构建、测试、文档和许可证门禁

#### 目标

证明新增功能符合 Sunshine 工程约束，并且功能关闭时不影响现有平台。

#### 工作项

- 补齐所有新增 C/C++ API 的 Doxygen。
- 格式化修改的 C/C++ 文件。
- 完成协议、认证、REST、状态机、sink、router、rumble 和配置测试。
- 对新增第三方依赖固定版本、校验值、许可证和构建选项。
- 更新英文配置文档和用户操作文档。
- 若功能只支持 Linux/aarch64，非目标平台必须清晰禁用或使用无副作用 stub，不得破坏编译。

#### 验证方法

```bash
git diff --check
./scripts/build-rkmpp.sh
```

随后运行上述构建产生的 `test_sunshine`，不得从其他构建目录复用旧产物。

根据项目现有覆盖工具生成 changed-code coverage 报告。Windows 构建在可用的 MSYS2 UCRT64 环境中另行执行。

#### 通过标准和产物

- `git diff --check` 无错误。
- ROCK 5B+ 正式 Sunshine 目标和 `test_sunshine` 构建成功。
- `test_sunshine` 全部通过，无新增回归。
- changed code 达到 100% 覆盖；硬件专属、编译条件或不可达防御分支如无法覆盖，必须逐项记录并获得确认，不能笼统豁免。
- Doxygen 构建无 undocumented API 错误。
- 依赖许可证与 Sunshine 分发方式兼容，归属声明齐全。
- `step-11.md` 保存完整命令、退出码、测试数量和覆盖摘要。

---

### 步骤 12：ROCK 5B+ + Xbox + Moonlight 最终验收

#### 目标

在真实 HDMI RX 使用方式下验证功能、延迟、稳定性和回滚。

#### 验收矩阵

| 场景 | 操作 | 可验证通过标准 |
| --- | --- | --- |
| 冷启动 | Sunshine、Xbox 和客户端均从未连接状态启动 | 应用启动后 session 进入 ready，首个按键正确，无需重启 Sunshine |
| 基本输入 | 测试全部按键、方向键、摇杆、扳机 | Xbox 动作与 Moonlight 输入逐项一致，释放后归零 |
| 组合输入 | 双摇杆 + 扳机 + 两个按钮同时操作 | 无丢失、无错误映射、无持续状态 |
| 振动 | 触发普通和扳机振动 | Moonlight 客户端相应马达收到反馈，停止后归零 |
| 5 分钟交互 | 正常游戏并快速 press/release | 无可观察粘键或明显输入积压 |
| 2 小时耐久 | 持续 HDMI 串流和 Remote Play 输入 | 无断连、持续 RSS/FD 增长、keepalive 失败或媒体队列增长 |
| Moonlight 重连 | 客户端断开后重新连接 | Xbox session 策略符合配置；旧输入不重放，新 feedback queue 生效 |
| Xbox/网络故障 | 断网或 Xbox 暂时不可用后恢复 | 明确状态、输入归零、在预算内重连或给出可操作失败 |
| 应用停止 | 正常退出 Xbox/HDMI 应用 | gamepad remove、peer close、session delete，Xbox 无残留输入 |
| 功能关闭 | 禁用 Xbox Remote Play 输出 | 无 Xbox 认证/网络活动，virtual-HID/NXBT 行为保持原状 |

#### 性能记录

必须记录并比较功能关闭/开启两组数据：

- Sunshine 和 bridge/probe 的 CPU 使用率。
- RSS、线程数和文件描述符。
- Remote Play 接收网络吞吐。
- HDMI RX → Moonlight 视频帧率和现有延迟 HUD 指标。
- 手柄事件进入 Sunshine 到 WebRTC `send()` 接受之间的 P50/P95/P99。
- 若可在 Xbox 端观测，再记录主观或外部测得的按钮到响应延迟；不得把 `send()` 延迟冒充端到端延迟。

#### 通过标准和产物

- 验收矩阵全部为 `PASS`，硬件不可用项目必须为明确的 `BLOCKED`，不得写成通过。
- 2 小时稳定测试期间没有持续资源增长、粘键或 Remote Play session 泄漏。
- HDMI 视频链路相对关闭功能的基线没有无法解释的帧率下降或延迟回归。
- `step-12.md` 包含测试环境、配置、矩阵、性能对比和已知限制。

## 7. 后续可选阶段：WAN、Teredo 和多手柄

只有 LAN 首版完成并稳定后才评估：

- Teredo/IPv6 candidate 识别和优先级调整。
- 更复杂 NAT 下的 STUN/TURN 策略。
- 多逻辑手柄 frame 和独立 feedback 路由。
- 多 Xbox 主机快速切换。
- 凭据接入系统 secret service。
- 将已验证的 sidecar transport 内置 Sunshine。

每个扩展都必须重新执行步骤 4、6、10、11 和 12 中适用的兼容、协议、故障和硬件验证，不能以 LAN 结果推定 WAN 或多手柄正确。

## 8. 阶段交接模板

每个步骤完成后，验证记录必须包含：

```text
步骤：XX
结论：PASS / FAIL / BLOCKED
Sunshine HEAD：
LunarNX HEAD：
XStreaming HEAD：
修改文件：
执行的自动化测试及结果：
执行的真机测试及结果：
正常路径覆盖：
错误/超时/取消路径覆盖：
资源和清理结果：
未执行项及原因：
敏感信息脱敏检查：
git diff --check：
git status --short：
下一步骤风险：
```

任何前置步骤为 `FAIL` 或 `BLOCKED` 时，后续实现只能修复该门槛，不得把后续步骤标记为完成。
