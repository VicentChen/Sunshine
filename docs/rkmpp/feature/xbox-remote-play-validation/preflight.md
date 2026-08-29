# Xbox Remote Play 手柄回传预研记录

> 状态：已完成静态预研；未连接 Microsoft/Xbox 服务，未创建 Remote Play session
>
> 用途：记录开始编码前可以确定的事实、参考来源和仍需真机验证的门槛。本文件不代表实施计划中的任何步骤已经 `PASS`。

## 1. 已确认前提

- 目标平台为 ROCK 5B+ / RK3588 / Linux aarch64。
- Xbox 与 ROCK 5B+ 首版按同一局域网设计。
- `HDMI RX → Sunshine → Moonlight` 视频链路已经验证可用。
- Xbox Remote Play 连接只用于手柄输入和振动反馈；其音视频不解码、不播放、不渲染。
- Sunshine 当前已有可路由的 gamepad sink 抽象，Xbox Remote Play 应作为新的异步 sink 接入，不把网络调用放入输入解析线程。

## 2. 参考源码版本与许可证

| 项目 | 本次审阅 commit | 与本功能相关的许可证结论 |
| --- | --- | --- |
| Sunshine | `c3e47eba8caa575a9cce82194a62250caab8b43e` | GPL-3.0，最终新增代码按 Sunshine 许可证处理 |
| LunarNX | `5b3490deb5113714b52b3147bf11b24318b62359` | `src/` 中除 `src/ps/` 外的原创代码为 MIT |
| XStreaming | `9d7649d477b71a674ab72842e950e977e60baedb` | MIT |

约束：

- 可以参考或派生 LunarNX 的 Xbox 路径和 XStreaming，但复制实质性代码时必须保留适用的版权和许可证声明。
- 不得复制 LunarNX `src/ps/`、Chiaki 或相关 PlayStation 路径；该部分涉及 AGPL-3.0。
- 不直接移植 LunarNX 的 patched libpeer。它属于 Switch 专用依赖方案，不适合作为 Sunshine/ROCK 的默认传输层。

## 3. 协议来源映射

### 3.1 认证

主要来源：

- `../LunarNX/src/auth/auth_manager.cpp`
- `../LunarNX/src/api/api_constants.h`

已确定的最小认证链：

```text
Microsoft Device Code OAuth
  → Microsoft access/refresh token
  → Xbox user/authenticate
  → XSTS，RelyingParty = http://gssv.xboxlive.com/
  → xHome GSSV login，offeringId = xhome
  → gsToken + region baseUri
```

首版不需要 LunarNX/XStreaming 的 Cloud Gaming token、catalog 或 entitlement 路径。

### 3.2 Home session REST

主要来源：

- `../LunarNX/src/api/xbox_api_client.cpp`
- `../LunarNX/src/app/xbox_session_client.cpp`
- `../XStreaming/src/xCloud/index.ts`

已确定的最小调用顺序：

```text
discover consoles
  → POST /v5/sessions/home/play
  → GET /state，等待 Provisioned
  → GET /configuration
  → POST/GET /sdp
  → POST/GET /ice
  → POST /keepalive
  → DELETE /v5/sessions/home/{sessionId}
```

`ReadyToConnect → POST /connect` 属于需要 MSAL `lpt` 的路径。LunarNX 和 XStreaming 旧实现只在相应状态/云路径处理；首版 Home session 不应无条件调用 `/connect`。

### 3.3 WebRTC 和 data channel

主要来源：

- `../LunarNX/src/webrtc/peer_manager.cpp`
- `../LunarNX/src/webrtc/xstreaming_data_channels.h`
- `../XStreaming/src/webrtc/index.ts`
- `../XStreaming/nano-rs/src/webrtc_backend.rs`

必须在 offer 前创建：

| Label | Protocol | 初始策略 | LunarNX 当前 SID |
| --- | --- | --- | --- |
| `control` | `controlV1` | reliable + ordered | 0 |
| `input` | `1.0` | reliable + ordered | 2 |
| `message` | `messageV1` | reliable + ordered | 4 |
| `chat` | `chatV1` | reliable + ordered | 6 |

SDP 不能先行裁剪为 input-only。第一版必须保留 video、audio 和 application m-line；音视频只进入 no-op consumer。

### 3.4 通道启动协议

主要来源：

- `../LunarNX/src/app/xbox_channel_manager.cpp`
- `../XStreaming/src/webrtc/Channel/Control.ts`
- `../XStreaming/src/webrtc/Channel/Message.ts`

已确定顺序：

```text
message Handshake
  → 等待 HandshakeAck
  → control authorizationRequest
  → gamepadChanged(false)
  → 约 500 ms
  → gamepadChanged(true)
  → 六类客户端能力消息
  → input ClientMetadata
  → gamepad snapshots
```

`chat` 首版不承载业务数据，但必须创建以保持协议兼容。

### 3.5 手柄二进制协议

主要来源：

- `../XStreaming/src/webrtc/Packet/index.ts`
- `../XStreaming/nano-rs/src/channels/input.rs`
- `../LunarNX/src/input/xinput_encoder.cpp`

单手柄完整快照为 38 bytes：

```text
14-byte header
  uint16 little-endian report type
  uint32 little-endian sequence
  float64 little-endian timestamp

1-byte frame count

23-byte gamepad frame
  uint8 gamepad index
  uint16 button mask
  int16 left X/Y
  int16 right X/Y
  uint16 left/right trigger
  uint32 physical physicality
  uint32 virtual physicality
```

Sunshine 映射中可在编码前确定的规则：

- `uint8` trigger 映射到 `uint16` 使用 `value * 257`。
- 多字节字段必须显式 little-endian 编解码，不能发送 C++ struct 内存。
- sequence 在实际发送时生成，不在入队时生成。
- physical physicality 是独立字段，不能直接复用 button mask。
- 摇杆 Y 轴符号需要真机验证，当前不能仅凭 LunarNX 或 React Native 的平台坐标系确定。

## 4. 参考实现之间的冲突与处理结论

### 4.1 Data channel 顺序和可靠性

- XStreaming TypeScript 明确把 `input` 设为 ordered，其他通道使用 WebRTC 默认值。
- XStreaming nano-rs 为每条通道提供各自 spec。
- LunarNX 当前源码和测试将四条通道全部设为 reliable + ordered，并说明 unreliable input 会造成序号缺口和永久丢控。

首版结论：使用 LunarNX 当前真机路径的 reliable + ordered 配置；PoC 必须记录最终 SDP/SCTP 参数。

### 4.2 SID 描述不一致

LunarNX 部分技术说明与当前源码可能不一致。当前源码使用偶数 SID `0/2/4/6`，因此实现时以对应 commit 的源码和测试为准，不以旧说明文字为准。

### 4.3 `/connect` 条件不一致

XStreaming nano-rs 在 `ReadyToConnect` 且存在 web token 时调用 `/connect`；旧 TypeScript 和 LunarNX 对 Home/Cloud 有更明确的区分。

首版结论：Home session 的正常成功状态为 `Provisioned`，不无条件调用 `/connect`。若真实 Home session 返回 `ReadyToConnect`，记录脱敏响应并停止扩展，由单独兼容性修正处理。

### 4.4 Audio transceiver 方向不一致

不同实现对 audio 使用 `recvonly` 或 `sendrecv`。这不能仅靠静态源码决定。

PoC 顺序：

1. 先使用 `recvonly`，避免声明实际不存在的麦克风发送路径。
2. 若 Xbox 拒绝 SDP 或通道无法 ready，再对照真实 answer 尝试服务要求的最小兼容方向。
3. 无论方向如何，首版都不创建音频采集或解码器。

### 4.5 媒体反馈

XStreaming nano-rs 包含 processed-frame/统计反馈，旧实现更多依赖真实渲染状态。静态分析不能证明 Xbox Home session 是否要求合成反馈保持连接。

PoC 结论：先只维持标准 WebRTC RTP/RTCP 和 no-op consumption；若 Xbox 在稳定复现的时间点因缺少上层反馈关闭连接，再基于抓到的协议行为增加最小反馈。不得预先伪造已处理帧。

## 5. Sunshine 和目标环境依赖盘点

当前 Sunshine 构建已经具备：

| 依赖 | 本机发现结果 | 可承担工作 |
| --- | --- | --- |
| OpenSSL | 3.0.20 | TLS、凭据相关加密基础能力 |
| libcurl | 7.88.1 | OAuth、Xbox REST 和 GSSV HTTP |

当前 `pkg-config` 未发现：

- libdatachannel
- libnice
- usrsctp
- libsrtp2

结论：

- 认证和 REST 不需要先引入新的 HTTP/TLS 栈，应优先复用 Sunshine 已有 OpenSSL/libcurl 依赖。
- WebRTC 不能假设使用系统现成组件，必须固定并构建完整依赖链。
- 如果采用 libdatachannel，必须启用 media transport；仅 Data Channel 的 `NO_MEDIA` 构建不能生成本计划要求的完整 Xbox SDP。
- WebRTC 依赖必须先在独立 probe 中验证，再决定进入 Sunshine 主二进制或保留 sidecar。

## 6. 编码前可以完成的工作

以下内容不需要修改 Sunshine C/C++ 源码，也不需要连接 Xbox：

- 本文件中的版本、许可证、协议来源和冲突审计：已完成。
- 从参考实现提取脱敏的 REST JSON、channel JSON 和二进制 golden fixture：可继续执行。
- 设计 token store 权限、原子写入和日志脱敏测试矩阵：可继续执行。
- 设计 session 状态机、错误分类、超时和 cleanup 状态表：可继续执行。
- 比较 sidecar/内置两种构建与部署影响：可继续做静态分析，但最终决策仍需 WebRTC PoC 数据。
- 准备 fake Xbox server 的请求/响应清单：可继续执行。

以下内容无法通过静态预研确认：

- Microsoft/Xbox 认证链对目标账号的实时可用性。
- Xbox 当前服务实际返回的 SDP、ICE 和 Home session 状态。
- 候选 WebRTC 库与 Xbox DTLS/SCTP/媒体协商的兼容性。
- audio transceiver 的最小可接受方向。
- no-op 媒体接收能否在长时间 session 中保持稳定。
- Sunshine 摇杆 Y 轴相对 Xbox 协议的正确符号。
- Xbox 振动数据在目标固件上的实际格式和触发行为。

这些项目必须由实施计划中的独立 probe 和真实 Xbox 验证，不能在编码前标记为完成。
