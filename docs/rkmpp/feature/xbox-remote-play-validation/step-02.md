# Xbox Remote Play 验证记录：步骤 02

> 归档于 [Feature 005](../005_xbox_remote_play.md)。

步骤：02（认证链和安全 token store）

结论：PASS

Sunshine HEAD：`a8e3c460077d06484798e6744ce623b599d5d70a`

LunarNX HEAD：`5b3490deb5113714b52b3147bf11b24318b62359`

XStreaming HEAD：`9d7649d477b71a674ab72842e950e977e60baedb`

## 实现范围

- `src/xbox_remote/auth.*`：Device Code、轮询、refresh、Xbox User Token、GSSV/Web XSTS 和 xHome GSSV token 链。
- `src/xbox_remote/token_store.*`：仅持久化 Microsoft OAuth access/refresh token 和到期时间；Xbox/XSTS/GSSV token 只保留在内存。
- `src/xbox_remote/http_runtime.*`：只允许 HTTPS、目标机兼容所需的 IPv4、15 秒调用方截止时间、1 MiB 响应上限和可取消等待。
- `src/xbox_remote/probe_main.cpp`：独立 `xbox-remote-probe login|resume` 入口，只显示 Device Code 用户码和固定状态，不输出 token。
- `tests/unit/test_xbox_remote_auth.cpp` 和 `tests/unit/test_xbox_remote_token_store.cpp`：完全离线的 fake HTTP、fake clock、错误脱敏和文件安全测试。

## 参考来源和许可证

| 行为 | 交叉验证来源 | 许可证 |
| --- | --- | --- |
| MSAL client ID、scope、Device Code 和 refresh 请求 | `../LunarNX/src/auth/auth_manager.cpp`、`../XStreaming/src/xal/msal.ts` | MIT |
| Microsoft access token → Xbox User Token | 同上 | MIT |
| GSSV/Web 两种 XSTS relying party 和顺序 | 同上 | MIT |
| xHome GSSV endpoint、`offeringId`、请求头和默认 region | 同上 | MIT |

实现没有引用 LunarNX 的 PlayStation/Chiaki AGPL 路径，也没有复制 patched libpeer。

## 安全约束

- token store 在 Linux 上使用 `O_NOFOLLOW`，要求普通文件、当前有效用户所有且无 group/other 权限。
- 新文件以 `0600` 创建，经 `fsync` 后在同目录原子 rename，并再次同步父目录。
- load 在完整权限、大小和 JSON 校验成功前不修改调用方现有凭据。
- 单个 token 文件上限为 64 KiB；单个 HTTP 响应上限为 1 MiB。
- 错误对象只包含固定 stage、固定消息和 HTTP 状态；不拼接响应体、curl 错误文本或请求中的 token。
- 探针不会打印 access token、refresh token、Xbox token、XSTS token、`gsToken`、user hash 或 GSSV base URI。
- Microsoft OAuth 成功后即使 Xbox/XSTS/GSSV 暂时失败也会保存 OAuth，后续可通过 `resume` 继续；各 Xbox token 网络阶段最多重试三次。

## 自动验证

1. `./scripts/build-rkmpp.sh`
   - 退出码：0。
   - 产物：`build-rkmpp-review/sunshine`。
   - 产物：`build-rkmpp-review/tests/test_sunshine`。
   - 产物：`build-rkmpp-review/xbox-remote-probe`。
2. Xbox 定向测试
   - 最终 31/31 通过。
   - 其中认证 11 项、生产 runtime 1 项、token refresh 1 项、token store 5 项，并回归步骤 01 协议 13 项。
3. `test_sunshine --gtest_filter='-*DownloadFileTest*'`
   - 647 项运行；634 项通过；13 项因平台初始化、音频或编码器环境不可用而跳过；0 项失败。
4. Device Code fake HTTP
   - 覆盖成功、`authorization_pending`、`slow_down`、用户拒绝、code 过期、截止时间、请求前取消、等待中取消、瞬时网络错误重试、非 2xx、未知服务错误和畸形 JSON。
5. OAuth/Xbox fake HTTP
   - 覆盖有效 access token 直接复用、提前刷新、已到期刷新、刷新响应省略 refresh token 时保留旧值、form percent-encoding、Xbox User Token、GSSV XSTS、Web XSTS、xHome GSSV 和默认/fallback region。
6. fake clock
   - 覆盖有效、提前刷新边界、已过期，以及墙钟向前和向后变化。
7. token store 临时目录
   - 覆盖 `0600`、原子替换、临时文件清理、符号链接拒绝、不安全权限拒绝、损坏/超大 JSON 不覆盖内存值，以及保存失败不覆盖已提交文件。
8. 错误和输出脱敏
   - 测试把固定 secret marker 放入网络响应和请求，断言所有 caller-visible 错误不包含 marker。
   - 认证代码没有日志调用；探针只输出固定成功/失败文本，因此不存在未受控的响应体日志路径。
   - 完整回归输出经过全部固定 secret marker 扫描，无匹配。
9. `git diff --check`
   - 退出码：0。

## 真实账号验证

- 使用用户指定的 Xbox Microsoft 账号完成 Device Code 登录；未记录账号、用户码、device code 或任何 token。
- Microsoft OAuth → Xbox User Token → GSSV XSTS → Web XSTS → xHome GSSV 全链路成功。
- 终止首个探针进程后执行独立 `resume`，无需再次打开网页或输入代码即恢复完整 Xbox Home streaming authentication。
- token 文件检查结果：当前服务用户所有的普通文件，权限精确为 `0600`。
- 真实网络首次暴露默认地址选择的 TLS 失败；目标机强制 IPv4 后 Microsoft endpoint 正常。
- 后续瞬时断线暴露轮询/派生 token 的恢复缺口，已增加 Device Code 有效期内重试、Xbox token 阶段三次有限重试，以及 Microsoft OAuth 成功即安全保存；相应 fake HTTP 回归测试通过。
- access token 提前刷新、过期和墙钟变化由 fake clock 自动测试证明；本次真实 `resume` 使用仍有效的 access token，未人为破坏已保存凭据。

## 未执行项

- 本步骤不建立 Home session，不要求 Xbox 开机；Xbox 实机从步骤 03 的 create/delete 验证开始需要。
- Windows/MSYS2、Doxygen 和最终 changed-code coverage 门禁留待步骤 11；生产 curl 的真实 TLS 分支已由上述账号联机验证覆盖。

## Git 状态

开始时保留了用户已有的 `AGENTS.md`、Xbox 协议文件和构建清单改动。结束时在这些改动上新增认证、HTTPS runtime、安全存储、独立探针、测试和本记录；未改写或清理无关用户修改，未创建 issue 或 pull request。

## 下一步骤风险

- 主机发现和 Home REST 客户端必须复用截止时间和脱敏错误模型，并增加 GET/DELETE 能力。
- 401 只允许一次受控 token refresh/retry，所有已创建 session 的失败路径都必须尝试 DELETE。
- 步骤 04 的 WebRTC 兼容性探针是真机硬门槛，在四通道和无解码 media transport 通过前不得集成 Sunshine 输入热路径。
