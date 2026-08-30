# Xbox Remote Play 验证记录：步骤 01

> 归档于 [Feature 005](../005_xbox_remote_play.md)。

步骤：01（冻结 Xbox 协议契约和 golden fixtures）

结论：PASS

Sunshine HEAD：`a8e3c460077d06484798e6744ce623b599d5d70a`

LunarNX HEAD：`5b3490deb5113714b52b3147bf11b24318b62359`

XStreaming HEAD：`9d7649d477b71a674ab72842e950e977e60baedb`

## 修改文件

- `src/xbox_remote/protocol.h`
- `src/xbox_remote/protocol.cpp`
- `tests/unit/test_xbox_remote_protocol.cpp`
- `tests/fixtures/xbox_remote/*`
- `cmake/compile_definitions/common.cmake`
- `scripts/build-rkmpp.sh`
- `tests/CMakeLists.txt`
- `tests/unit/test_input.cpp`
- `docs/rkmpp/feature/xbox-remote-play-validation/step-01.md`

`AGENTS.md` 在开始本步骤前已有用户修改，本步骤未改动其内容。

## Fixture 来源和许可证

| 契约或 fixture | 交叉验证来源 | 许可证 |
| --- | --- | --- |
| Home console、play、state、configuration、SDP 和 ICE JSON | `../LunarNX/src/api/xbox_api_client.cpp`、`../LunarNX/src/app/xbox_session_client.cpp`、`../XStreaming/src/xCloud/index.ts` | LunarNX Xbox 路径 MIT；XStreaming MIT |
| 四条 data channel 的 label、protocol、SID、可靠性和顺序 | `../LunarNX/src/webrtc/xstreaming_data_channels.h`、`../XStreaming/src/webrtc/index.ts` | MIT |
| message handshake、control authorization、gamepad remove/add 和六类启动消息 | `../LunarNX/src/app/xbox_channel_manager.cpp`、`../XStreaming/src/webrtc/Channel/Control.ts`、`../XStreaming/src/webrtc/Channel/Message.ts` | MIT |
| 14-byte header、ClientMetadata、23-byte Gamepad frame 和 physicality | `../LunarNX/src/input/xinput_encoder.cpp`、`../XStreaming/src/webrtc/Packet/index.ts`、`../XStreaming/nano-rs/src/channels/input.rs` | MIT |
| Vibration frame | `../LunarNX/src/webrtc/xbox_input_feedback.cpp`、`../XStreaming/src/webrtc/Packet/index.ts`、`../XStreaming/nano-rs/src/channels/input.rs` | MIT |

实现没有引用或复制 LunarNX 的 `src/ps/`、Chiaki、patched libpeer 或其他 AGPL 路径。fixture 使用 RFC 5737 文档地址和固定的脱敏标识，不含真实服务响应或凭据。

## 执行的自动化测试及结果

1. `git diff --check`
   - 退出码：0。
2. `./scripts/build-rkmpp.sh`
   - 退出码：0。
   - 产物：`build-rkmpp-review/sunshine`。
   - 产物：`build-rkmpp-review/tests/test_sunshine`。
   - 构建入口已恢复 `BUILD_TESTS=ON`，并只要求正式 `sunshine` 和 `test_sunshine` 目标；`BUILD_TESTING=OFF` 避免无关第三方自测覆盖 Sunshine 的测试开关。
3. `test_sunshine --gtest_filter='XboxRemoteProtocolTest.*'`
   - 13/13 通过。
   - 覆盖 JSON 语义比较、未知字段、必需字段和类型错误、Home session 仅接受精确 `Provisioned`、显式 little-endian 编解码、38-byte gamepad golden packet、ClientMetadata、Vibration 校验和 fixture 敏感信息扫描。
4. `test_sunshine --gtest_filter='-*DownloadFileTest*'`
   - 631 项运行；618 项通过；13 项因平台初始化、音频或编码器环境不可用而跳过；0 项失败。
5. 未排除测试的完整运行
   - 一次运行结果为 633 项中 620 项通过、13 项环境性跳过、0 项失败。
   - 最后的干净复跑中，两个既有 `DownloadFileTest` 因 `httpbin.org` TLS 连接返回 curl code 35 而失败；同一复跑的其余 631 项无失败。该外网依赖失败与 Xbox 协议代码无关，未把它记录为通过。

## 正常路径覆盖

- Home play、SDP offer 和 ICE request 与 golden JSON 语义一致。
- Console discovery、session creation、state、configuration 和 exchange response 可解析脱敏 fixture。
- 四条 data channel 参数固定为 `control/input/message/chat`、`controlV1/1.0/messageV1/chatV1`、SID `0/2/4/6`、reliable + ordered。
- handshake、authorization、gamepad remove/add、六类启动消息、ClientMetadata 和单手柄快照均有固定契约。
- 多字节整数和 `double` timestamp 显式使用 little-endian，不发送 C++ struct 内存布局。

## 错误、超时和取消路径覆盖

- 本步骤不含网络、等待或取消逻辑，超时和取消不适用。
- JSON 覆盖 invalid JSON、错误顶层类型、缺失字段、错误字段类型、空值和范围错误。
- 二进制覆盖错误长度、错误 report type、错误 frame count、未知 vibration flag、非零首版 gamepad index、非法马达百分比、截断和超长包。

## 资源和清理结果

- 协议测试不创建线程、socket、session 或真实网络连接。
- 测试生成的根目录日志和写入临时文件已移入系统回收站。
- 为避免旧编译器生成的 coverage 计数污染结果，构建目录中的可再生 `.gcda` 文件在最终复跑前已清理。

## 未执行项及原因

- 无 Xbox 或 Microsoft 登录：步骤 01 不需要网络或硬件。
- changed-code coverage 百分比报告：留待步骤 11 的覆盖率门禁；步骤 01 的协议分支已通过参数化错误测试覆盖。
- Windows/MSYS2 回归：留待步骤 11。

## 敏感信息脱敏检查

- fixture 扫描测试拒绝 JWT 形状、`refresh_token`、`access_token`、`gsToken` 和 RFC 1918 私网地址。
- fixture 中的主机、session、SDP、ICE 和地址均使用固定脱敏值或 RFC 5737 文档地址。

## Git 状态

开始时：

```text
 M AGENTS.md
 M cmake/compile_definitions/common.cmake
?? src/xbox_remote/
?? tests/fixtures/xbox_remote/
?? tests/unit/test_xbox_remote_protocol.cpp
```

结束时仍保留上述用户/既有改动，并新增本步骤的构建门禁、测试修正和验证记录；未创建 issue 或 pull request。

## 下一步骤风险

- Device Code OAuth 和 Xbox token 链必须通过可注入 HTTP/clock 覆盖所有错误、刷新和取消路径。
- refresh token 只能落入权限为 `0600` 的原子文件，任何日志或结构化错误不得包含 token 内容。
- 真正登录 Microsoft 账号属于外部状态操作，自动测试完成后再由用户明确参与验证。
