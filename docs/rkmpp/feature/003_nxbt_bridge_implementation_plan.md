# Moonlight → Sunshine → Nintendo Switch 手柄桥接分步实施计划

> 执行状态：按用户决定于阶段 9 结束；阶段 10、可选里程碑 A/B 及后续发布门禁不再执行。

> 状态：待实施
>
> 目标平台：ROCK 5B+ / RK3588 / Linux aarch64
>
> 首版目标设备：Nintendo Switch 一代，单个虚拟 Pro Controller，ROCK 5B+ 板载蓝牙适配器
>
> 本计划供多个 AI Agent 按阶段顺序执行。每个 Agent 原则上只执行一个阶段；必须先读取本文件、前序阶段交接记录和相关 diff，再开始修改。

## 1. 最终目标

在不修改 Moonlight 客户端协议的前提下，实现以下输入链路：

```text
Moonlight 客户端物理手柄
  → GameStream 手柄消息
  → Sunshine 现有输入解析与状态保持
  → NXBT Bridge IPC
  → NXBT / BlueZ / Bluetooth L2CAP
  → Nintendo Switch 一代
```

首版完成后应满足：

- Moonlight 客户端的按键、方向键、双摇杆和扳机输入可以控制 Nintendo Switch。
- Sunshine 和 NXBT Bridge 任一侧异常、断流或超时后，Switch 不会残留卡住的按键或摇杆状态。
- NXBT 的 BlueZ 特殊权限和系统配置不进入 Sunshine 主进程。
- Sunshine 原有主机虚拟手柄路径保持可用，并可明确选择 `virtual`、`nxbt` 或 `both` 输出方式。
- 初次配对、快速重连、Bridge 不可用、蓝牙适配器不可用等状态有明确日志和可验证的失败行为。
- 所有新增或修改的 C/C++ API 都有 Doxygen 文档和 gtest；Bridge 纯逻辑与协议代码有自动化测试。

## 2. 已确认的架构决策

### 2.1 NXBT 必须作为独立服务

不得把 Python 解释器或 NXBT Python 对象嵌入 Sunshine 主进程。使用独立 `nxbt-bridge` 服务隔离以下故障域：

- BlueZ 重启、配置错误和 D-Bus 异常。
- Python 运行时、NXBT 多进程及依赖错误。
- 原始 Bluetooth L2CAP socket 权限。
- Switch 配对、重连和蓝牙适配器故障。

Sunshine 主进程只负责：

- 把已经解析完成的 `platf::gamepad_state_t` 转换为稳定 IPC 数据模型。
- 管理逻辑手柄的 attach、rebind、neutralize 和 detach 生命周期。
- 接收 Bridge 状态并记录可操作的错误信息。

NXBT Bridge 负责：

- BlueZ 环境预检和蓝牙适配器所有权。
- Switch Pro Controller 的创建、配对、连接和重连。
- 持有每个逻辑手柄的最新完整状态。
- 按蓝牙协议要求的节奏生成报告；不得依赖 Moonlight 包的到达频率维持报告节奏。
- watchdog 超时归零和异常退出清理。

### 2.2 首版使用本机 Unix Domain Socket

首版只支持 Sunshine 与 NXBT Bridge 运行在同一台 Linux 主机，通过 Unix Domain Socket 通信。默认路径建议为：

```text
/run/nxbt-bridge/control.sock
```

首版不开放 TCP/UDP 监听端口。Windows Sunshine → Linux VM/独立设备属于后续扩展，必须在增加认证和防重放机制后实施。

### 2.3 IPC 使用版本化的定长二进制消息

IPC 必须具有：

- 固定 magic。
- 明确的 protocol version。
- 明确的 message type 和 payload length。
- 每个手柄独立递增的 sequence。
- 发送端 monotonic timestamp。
- 显式 little-endian 编码，不直接传输带编译器 padding 的 C/C++ struct 内存。

首版至少定义以下消息：

| 方向 | 消息 | 用途 |
| --- | --- | --- |
| Sunshine → Bridge | `hello` | 协商协议版本和能力 |
| Bridge → Sunshine | `hello_ack` / `error` | 确认或拒绝版本 |
| Sunshine → Bridge | `attach` | 将 Sunshine 全局手柄槽绑定到 Bridge controller slot |
| Sunshine → Bridge | `rebind` | Moonlight session 恢复时更新 client-relative id |
| Sunshine → Bridge | `state` | 提交完整按键、扳机和摇杆状态 |
| Sunshine → Bridge | `neutralize` | 立即发送全松开状态但保留蓝牙连接 |
| Sunshine → Bridge | `detach` | 解除逻辑绑定；是否销毁蓝牙 controller 由 Bridge 策略决定 |
| Sunshine ↔ Bridge | `ping` / `pong` | 健康检查与 watchdog |
| Bridge → Sunshine | `status` | `unavailable/pairing/connecting/connected/reconnecting/failed` |

完整状态 payload 至少包含：

```text
controller_id
button_flags: uint32
left_trigger: uint8
right_trigger: uint8
left_stick_x: int16
left_stick_y: int16
right_stick_x: int16
right_stick_y: int16
sequence: uint32
monotonic_timestamp_us: uint64
```

控制消息必须按顺序处理。高频 `state` 消息采用 latest-state-wins：Bridge 读取队列积压时可以丢弃同一 controller 的旧 `state`，但不得丢弃 `attach`、`neutralize` 或 `detach`。

### 2.4 BlueZ 配置在部署期完成

禁止正式 Bridge 在每次启动时创建 systemd override、执行 `systemctl daemon-reload` 或重启 `bluetooth.service`。

必须先验证最小 BlueZ 参数：

```text
--compat --noplugin=input
```

如果目标机实测仍有端口冲突，只能根据日志逐项增加需要禁用的插件，禁止继续使用 NXBT 当前的：

```text
--noplugin=*
```

BlueZ 配置由安装脚本或管理员一次性完成。Bridge 启动时只做只读预检并给出明确错误，不擅自改变系统状态。

### 2.5 首版功能范围

首版支持：

- Pro Controller。
- 单个 Sunshine 手柄映射到单个板载蓝牙适配器。
- A/B/X/Y、D-pad、L/R、PLUS、MINUS、HOME、CAPTURE、双摇杆按压。
- 模拟 LT/RT 到数字 ZL/ZR 的带迟滞转换。
- 初次配对、已配对快速重连、断线归零、Moonlight pause/resume。

首版不支持：

- Switch 2 或 Switch 2 Pro Controller 的兼容承诺。
- Joy-Con L/R 合并或拆分。
- 多个虚拟控制器共享一个蓝牙适配器。
- 远程网络 Bridge。
- 蓝牙唤醒 Switch。
- Amiibo/NFC/IR。
- Moonlight motion 数据到 Switch IMU 报告。
- Switch HD Rumble 回传 Moonlight。
- 自动修改系统 BlueZ 配置。

Motion 和 rumble 在本文后续作为独立可选里程碑，不能阻塞首版输入链路验收。

## 3. 输入映射契约

首版默认按按钮标签映射：

| Sunshine | NXBT Pro Controller |
| --- | --- |
| `A` | `A` |
| `B` | `B` |
| `X` | `X` |
| `Y` | `Y` |
| `START` | `PLUS` |
| `BACK` | `MINUS` |
| `HOME` | `HOME` |
| `MISC_BUTTON` | `CAPTURE` |
| `LEFT_BUTTON` | `L` |
| `RIGHT_BUTTON` | `R` |
| `LEFT_STICK` | 左摇杆按压 |
| `RIGHT_STICK` | 右摇杆按压 |
| D-pad | D-pad |

必须提供可配置的 face button 策略：

- `labels`：A→A、B→B、X→X、Y→Y，首版默认。
- `positions`：按 Xbox/Nintendo 物理位置交换 A/B 和 X/Y。

扳机转换默认建议：

- 当前为释放状态且值 `>= 64` 时按下 ZL/ZR。
- 当前为按下状态且值 `<= 48` 时释放 ZL/ZR。
- 49–63 保持上一次状态，避免临界抖动。

摇杆规则：

- 输入范围保留 Sunshine 的 `[-32768, 32767]`。
- 转换前限幅，中心 0 必须映射到 NXBT 校准中心。
- X 正方向为右，Y 正方向为上。
- 不在 Sunshine 和 Bridge 两侧重复应用死区。
- NXBT 的左右摇杆校准分别处理，不能共用一个中心或最大值。

## 4. 全局工程约束

所有阶段必须遵守：

- 开始和结束时记录 `git status --short`，保留所有无关未提交修改。
- 当前已知无关修改包括但不限于 `src_assets/linux/assets/apps.json`、`tests/unit/test_process.cpp` 和若干应用图片；不得覆盖、格式化或提交它们，除非用户另行授权。
- 不创建 GitHub issue 或 pull request。
- 新增或修改的 C/C++ API、类型、字段和非显然逻辑必须有符合仓库格式的 Doxygen。
- 使用 `.clang-format` 格式化修改的 C/C++ 文件。
- 新增或修改方法必须添加 gtest，changed code 以 100% 覆盖为目标。
- Python Bridge 纯逻辑使用标准库 `unittest` 或项目明确引入的测试框架，测试不得要求真实蓝牙硬件。
- 本次增加本地化时只修改 `en`，不得修改 `en-US` 或其他语言。
- Sunshine 构建有且只有一个入口，不得指定或覆盖构建目录：

```bash
./scripts/build-rkmpp.sh
```

- C++ 测试必须使用该脚本产生的 `test_sunshine`，不得复用其他构建目录中的旧产物。
- Windows/MSYS2 回归遵守仓库级平台规则；本计划不定义额外构建命令。

- 硬件测试必须与无硬件单元测试分开记录。没有硬件时不得伪造通过结果。
- 任何会重启 BlueZ、改变 systemd、配对/解绑设备或占用蓝牙适配器的命令，都必须在交接中明确标为有系统影响的硬件步骤。

## 5. Agent 执行与交接规则

每个阶段开始前：

1. 阅读本计划和所有前序交接。
2. 检查前序阶段要求的文件、测试和验收记录是否存在。
3. 记录当前 HEAD、submodule HEAD、`git status --short`。
4. 若前序验收未通过，停止功能扩展，只修复阻塞当前阶段的最小问题并记录原因。

每个阶段完成后必须提交以下交接信息：

1. 完成的阶段编号及结论：`PASS`、`FAIL` 或 `BLOCKED`。
2. 修改文件列表。
3. 最终接口、线程、进程和资源所有权约定。
4. 执行过的构建、gtest、Python test、fake Bridge、硬件命令及结果。
5. 覆盖的正常、错误、超时和清理分支。
6. 未执行项及准确阻塞原因。
7. `git diff --check` 和 `git status --short` 结果。
8. 下一阶段开始前必须知道的风险。

每个阶段都应把硬件或运行证据写入：

```text
docs/rkmpp/feature/nxbt-validation/step-XX.md
```

日志、MAC 地址和配对密钥不得提交。提交前必须脱敏蓝牙地址、用户名、IP 和认证信息。

## 6. 阶段 0：固定基线与目标机能力盘点

### 6.1 目标

建立不修改系统状态的 Sunshine、BlueZ、Python、NXBT 和蓝牙硬件基线。

### 6.2 工作项

- 记录 Sunshine HEAD、NXBT submodule HEAD 和目标机发行版。
- 确认目标为 Nintendo Switch 一代，而非 Switch 2。
- 记录 BlueZ、Python、dbus-python、systemd 和 Linux kernel 版本。
- 枚举蓝牙适配器、驱动、总线类型和当前被哪个服务管理。
- 确认测试使用 ROCK 5B+ 板载蓝牙适配器；不得在此阶段配对或重启 BlueZ。
- 记录 Sunshine 当前 Moonlight 手柄到主机虚拟手柄的成功基线。
- 记录当前 `gamepad = switch` 只创建主机虚拟 HID，不连接 NS。

### 6.3 只读验证命令

```bash
git rev-parse HEAD
git -C third-party/nxbt rev-parse HEAD
git status --short
uname -a
cat /etc/os-release
bluetoothd --version
python3 --version
python3 -c "import dbus; print(dbus.__version__)"
systemctl cat bluetooth.service
systemctl show bluetooth.service -p FragmentPath -p DropInPaths -p ExecStart
bluetoothctl list
bluetoothctl show
lsusb
rfkill list bluetooth
```

### 6.4 自动化基线

执行现有 Sunshine input 测试，保存结果：

```bash
test_sunshine \
  --gtest_filter='InputGamepadSessionTest.*:*Virtualhid*Gamepad*'
```

若构建未产生 `test_sunshine`，必须将该项记为阻塞；不得通过另行配置构建目录绕过唯一构建入口。

### 6.5 验收标准

- `docs/rkmpp/feature/nxbt-validation/step-00.md` 包含版本和能力表，但不包含敏感地址。
- ROCK 5B+ 板载蓝牙适配器被明确识别。
- 没有修改 systemd、BlueZ 配置或配对记录。
- Sunshine 现有手柄基线通过，或准确记录既有失败且确认与 NXBT 无关。

## 7. 阶段 1：独立验证 NXBT 与最小 BlueZ 配置

### 7.1 目标

在不修改 Sunshine 的前提下，证明目标 USB 适配器、BlueZ 和 Nintendo Switch 一代能够完成 NXBT Pro Controller 配对和输入，并确定最小必要 BlueZ 插件配置。

### 7.2 前置条件

- 阶段 0 为 `PASS`。
- 操作者已经安排允许短时重启 BlueZ 的测试窗口。
- 已保存当前 `systemctl cat bluetooth.service` 和所有 drop-in 路径。
- 目标机没有依赖该蓝牙服务的关键键鼠或音频连接。

### 7.3 工作项

- 不直接调用 NXBT 当前自动修改 BlueZ 的启动路径。
- 建立临时、可撤销的测试配置，首先只尝试 `--compat --noplugin=input`。
- 重启 BlueZ 后确认没有 `--noplugin=*`。
- 使用 NXBT demo 或最小 Python 脚本创建一个 `PRO_CONTROLLER`。
- 首次配对时在 Switch 打开“更改握法/顺序”。
- 验证按键、D-pad 和至少一个摇杆方向。
- 退出 NXBT 后恢复 BlueZ 原配置，确认普通 BlueZ 功能恢复。
- 如果仅禁用 `input` 仍然出现 PSM 17/19 占用，保存脱敏错误和 `btmon` 证据；只根据证据决定是否还需禁用其他具体插件。

### 7.4 必测场景

1. 首次配对。
2. NXBT 正常退出后重连。
3. NXBT 进程异常终止后人工清理和再次启动。
4. BlueZ 恢复后普通适配器枚举。
5. 当前 NXBT `--noplugin=*` 与最小配置的差异确认。

### 7.5 验收标准

- Switch 能识别并接受一个虚拟 Pro Controller。
- 最小 BlueZ 参数被实测确认；优先结果为 `--compat --noplugin=input`。
- 测试结束后不存在 `/run/systemd/system/bluetooth.service.d/nxbt.conf` 残留。
- `systemctl show bluetooth.service -p ExecStart -p DropInPaths` 与基线一致。
- 若硬件配对失败，本阶段标记 `BLOCKED`；不得继续开发 Sunshine 集成并假设硬件最终可用。

## 8. 阶段 2：冻结 IPC 和输入转换规范

### 8.1 目标

先实现不依赖 BlueZ、NXBT 进程或真实 socket 的协议编解码与手柄转换纯逻辑。

### 8.2 工作项

- 在独立文档或公共头文件中冻结 IPC magic、version、message type 和每个字段的字节序/长度。
- 实现 C++ 编码器/解码器，不使用 `reinterpret_cast` 直接序列化 struct。
- 实现 Python 编码器/解码器，并与 C++ 使用相同 golden vectors。
- 实现 Sunshine → NXBT 按钮转换。
- 实现 `labels` 和 `positions` 两种 face button 策略。
- 实现 LT/RT 带迟滞数字化状态机。
- 实现摇杆限幅和 NXBT 校准转换。
- 对错误 magic、未知 version、非法长度、未知 message type 和截断消息返回明确错误。

建议文件边界：

```text
src/input/nxbt_protocol.h
src/input/nxbt_protocol.cpp
src/input/nxbt_mapping.h
src/input/nxbt_mapping.cpp
tests/unit/test_nxbt_protocol.cpp
tests/unit/test_nxbt_mapping.cpp
tools/nxbt_bridge/protocol.py
tools/nxbt_bridge/tests/test_protocol.py
```

实际文件名可按现有 CMake 结构调整，但协议和映射不得藏在 socket 或 BlueZ 实现内部。

### 8.3 gtest 场景

- 每一种 Sunshine 按钮单独按下后的 NXBT 映射。
- 所有按钮同时按下。
- `labels` 与 `positions` 的 A/B/X/Y 差异。
- 未支持的 paddle/touchpad 位被明确忽略且不污染其他位。
- LT/RT：0、47、48、49、63、64、65、255 及上下穿越迟滞区。
- 四个摇杆轴：`-32768`、`-1`、`0`、`1`、`32767`。
- 左右摇杆分别使用各自校准值。
- 每种消息 encode→decode round trip。
- sequence wrap-around 的比较策略。
- 所有错误输入分支。

### 8.4 跨语言 golden test

生成固定 hex vectors，由 C++ 和 Python 双方读取。至少包括：

- neutral state。
- 全按键 + 极限摇杆状态。
- attach、state、neutralize、detach、status、error。
- 最大合法 payload 和一个截断 payload。

### 8.5 验证命令

```bash
test_sunshine \
  --gtest_filter='NxbtProtocolTest.*:NxbtMappingTest.*'

python3 -m unittest discover -s tools/nxbt_bridge/tests -p 'test_*.py' -v
```

### 8.6 验收标准

- C++ 与 Python 对所有 golden vectors 产生完全相同字节。
- 纯逻辑测试不需要 root、BlueZ、NXBT 或蓝牙硬件。
- 转换方法 changed code 分支覆盖达到 100%，或记录工具无法统计的准确原因。
- 协议 version 1 文档完整，下一阶段不需要猜测字段语义。

## 9. 阶段 3：加固 NXBT fork 的运行边界

### 9.1 目标

让 NXBT 可以在“系统已经由管理员正确配置”的模式下运行，不在构造或析构时修改 BlueZ。

### 9.2 工作项

- 在 NXBT fork 中增加显式配置，例如 `manage_bluez=False`；Bridge 必须使用关闭自动管理的模式。
- 保持 NXBT CLI 现有行为是否兼容由 fork 决定，但 Sunshine Bridge 不得触发旧行为。
- 把 BlueZ 环境检查从“修复动作”分离成只读 preflight：
  - D-Bus 可连接。
  - 指定 adapter 存在且 powered。
  - adapter 未被另一个 NXBT controller 占用。
  - HID Profile/SDP 注册可用。
  - PSM 17/19 可用，失败时输出明确原因。
- 修复清理路径：controller 子进程、socket、Profile 注册和 adapter alias 必须尽力恢复。
- 消除无超时 busy wait；所有等待连接操作必须支持 timeout 和取消。
- 不在本阶段重写完整 Switch 协议。

### 9.3 无硬件测试

- mock systemd/BlueZ 操作，验证 `manage_bluez=False` 时绝不调用写文件、重启服务或 daemon-reload。
- mock D-Bus adapter 枚举成功、无 adapter、多 adapter、adapter 占用。
- mock controller 创建成功、创建异常、连接超时、取消和清理。
- 验证 SIGTERM/正常关闭均释放 NXBT 子进程。

### 9.4 静态验证

对 Bridge 将使用的执行路径做搜索，确保不存在运行期系统修改：

```bash
rg -n 'systemctl|daemon-reload|restart.*bluetooth|nxbt.conf|noplugin=\*' \
  tools/nxbt_bridge third-party/nxbt/nxbt
```

允许旧 CLI 兼容路径仍包含这些字符串，但 Bridge 的 import/call graph 必须经过测试证明不会触发它们。

### 9.5 硬件 smoke

在阶段 1 已验证的部署配置下，以 `manage_bluez=False` 创建 Pro Controller、连接、输入一个按键、neutralize、退出。

### 9.6 验收标准

- Bridge 路径启动和退出时 `bluetooth.service` PID 不变化。
- `DropInPaths` 和 `ExecStart` 在运行前后完全一致。
- NXBT 连接行为不回归。
- 异常路径没有遗留 controller 子进程。

## 10. 阶段 4：实现独立 NXBT Bridge 和 watchdog

### 10.1 目标

实现可脱离 Sunshine 独立测试的 `nxbt-bridge` 服务。

### 10.2 工作项

- 创建 Unix `SOCK_SEQPACKET` 服务端；若实际 Python/runtime 不支持，使用 Unix `SOCK_STREAM` + 明确 length framing，不得依赖一次 `recv()` 等于一条消息。
- socket 目录和文件权限默认限制为 root/指定组，例如 `root:sunshine`、`0660`。
- 支持 version handshake、attach、state、neutralize、detach 和 status。
- 每个 controller 保存一个最新完整状态和 sequence。
- 收到旧 sequence、重复 sequence 或过期 timestamp 时，不覆盖新状态。
- Sunshine 心跳或输入超过默认 150ms 未更新时，发送一次 neutral state 并保持 neutral，直到新合法状态到达。
- IPC 连接断开时立即 neutralize。
- Bridge 退出时先 neutralize，再释放 controller。
- 支持 fake NXBT backend，使所有 IPC 和 watchdog 测试不依赖蓝牙。
- 连接与配对状态变更必须主动发 status，不要求 Sunshine 高频轮询。
- 日志不得打印完整 MAC、输入密钥或原始配对数据。

### 10.3 调度要求

- IPC 接收线程/协程不得直接睡眠维持蓝牙报告频率。
- NXBT controller loop 独立维持协议节奏。
- `state` 更新只替换最新快照，不建立无界队列。
- watchdog 使用 monotonic clock。
- 任何队列都有固定上限；达到上限时优先保留控制消息和最新 state。

### 10.4 自动化测试

- 正常 hello/attach/state/neutralize/detach。
- version mismatch 和 malformed packet。
- 两个客户端尝试占用同一 slot。
- state 高频 burst 后只应用最新状态。
- sequence 乱序和 wrap-around。
- 149ms 不触发 watchdog，超过阈值触发 neutral。
- IPC 断开立即 neutral。
- fake NXBT backend 抛异常后 status=`failed`，Bridge 仍可清理。
- SIGTERM 后 neutral 和子进程回收。
- socket 权限和 stale socket 恢复。

### 10.5 验证命令

```bash
python3 -m unittest discover -s tools/nxbt_bridge/tests -p 'test_*.py' -v
python3 -m tools.nxbt_bridge --backend=fake --socket=/tmp/nxbt-bridge-test.sock
```

另写测试客户端连续发送状态并故意中断，fake backend 必须记录 neutral。

### 10.6 验收标准

- 无硬件测试可重复通过。
- 所有超时测试使用可注入 fake clock，测试本身不依赖长时间 sleep。
- 真实 Bridge 启动不修改 BlueZ systemd 配置。
- 客户端崩溃或断开后 Switch 侧不会保持非中立状态。

## 11. 阶段 5：为 Sunshine 增加可测试的手柄输出路由

### 11.1 目标

把“Moonlight 输入状态”与“输出到主机虚拟 HID 或 NXBT”分离，同时保持现有平台行为。

### 11.2 工作项

- 定义带 Doxygen 的 gamepad sink/backend 接口，至少包含：
  - `alloc`
  - `rebind`
  - `update`
  - `neutralize`
  - `free`
  - 可选 `motion`、`battery` 和 feedback capability
- 实现现有 virtualhid adapter，使原有调用通过该接口工作。
- 实现 router：`virtual`、`nxbt`、`both`。
- `both` 模式必须定义原子语义：任一 sink 分配失败时回滚已成功的另一 sink，并让 alloc 失败；不得静默留下半连接状态。
- update 路径中一个 sink 的暂时错误不得阻塞或卡死另一个 sink；错误状态需要限频日志。
- 保留现有 retained gamepad session/rebind 行为。
- 本阶段使用 fake NXBT sink，不连接真实 IPC。

### 11.3 gtest 场景

- `virtual` 只调用 virtual sink。
- `nxbt` 只调用 fake NXBT sink。
- `both` 按确定顺序调用两个 sink。
- `both` 第二个 alloc 失败时回滚第一个。
- update、neutralize、free 顺序正确且各执行一次。
- pause 后 neutralize 但不 free；resume 后 rebind。
- terminate session 才最终 free。
- update failure 限频且不会造成死锁。
- 0–15 所有 Sunshine global gamepad slot 的边界检查。

### 11.4 验证命令

```bash
test_sunshine \
  --gtest_filter='GamepadRouterTest.*:InputGamepadSessionTest.*'
```

### 11.5 验收标准

- 所有既有 input/virtualhid 测试通过。
- `controller_output=virtual` 的行为与改动前一致。
- router 和生命周期错误分支有测试。
- 尚未运行 Bridge 时也能完整验证路由代码。

## 12. 阶段 6：实现 Sunshine NXBT IPC 客户端

### 12.1 目标

实现非阻塞、可重连、可测试的 C++ NXBT sink，通过 Unix socket 与 fake/real Bridge 通信。

### 12.2 工作项

- NXBT sink 拥有自己的 IPC worker，禁止在 Sunshine 输入热路径执行阻塞 connect/send/recv。
- `gamepad_update()` 只写入有界 latest-state slot，并唤醒 worker。
- 控制消息可靠排队；state 可以覆盖旧 state。
- 建立连接后必须完成 hello/hello_ack 才允许 attach。
- Bridge 重启后自动重新握手、attach，并首先发送 neutral，再发送当前最新状态。
- Sunshine 关闭或 session terminate 时尽力发送 neutralize/detach，但析构不得无限等待。
- socket 不存在、权限不足、版本不兼容、Bridge 返回 error 都有明确且限频日志。
- 输入路径不因 Bridge 不可用而无限增长内存。
- 所有公共类型和线程所有权有 Doxygen。

### 12.3 测试设施

- 在 gtest 中实现 fake Unix socket server 或可注入 transport。
- fake server 能主动断开、延迟 ack、返回错误、发送 malformed reply 和重启。
- 测试使用临时目录 socket，不依赖 `/run` 或 root。

### 12.4 gtest 场景

- 正常 handshake/attach/state/detach。
- socket 不存在时快速返回，输入线程不阻塞。
- permission denied、version mismatch、malformed reply。
- 1,000 次 state burst 只保留有限队列和最终状态。
- server 断开与重启后的 neutral→attach→latest state 顺序。
- Sunshine 析构期间 server 无响应，能够在有限时间结束。
- heartbeat/pong 正常、超时和恢复。
- 日志限频。

### 12.5 验证命令

```bash
test_sunshine \
  --gtest_filter='NxbtClientTest.*:NxbtSinkTest.*:NxbtProtocolTest.*'

python3 -m tools.nxbt_bridge --backend=fake --socket=/tmp/nxbt-bridge-test.sock
```

运行 Sunshine 单元测试客户端连接 fake Bridge，确认状态序列与 golden log 一致。

### 12.6 验收标准

- Bridge 不存在时 Sunshine 不崩溃、不阻塞、不泄漏线程或 fd。
- fake Bridge 重启后无需重启 Sunshine 即可恢复。
- 高频状态更新不存在无界 backlog。
- ThreadSanitizer 可用时对新增 worker/router 测试无 data race；不可用时记录原因。

## 13. 阶段 7：接入 Sunshine 配置、生命周期和可观测性

### 13.1 目标

提供清晰配置和运行状态，并完整接入 Sunshine retained gamepad 生命周期。

### 13.2 建议配置

最终命名须遵守当前 Sunshine 配置风格，建议：

```text
controller_output = virtual
nxbt_socket = /run/nxbt-bridge/control.sock
nxbt_controller_slot = 0
nxbt_face_buttons = labels
nxbt_trigger_press_threshold = 64
nxbt_trigger_release_threshold = 48
nxbt_watchdog_timeout = 150
```

约束：

- `controller_output` 仅允许 `virtual`、`nxbt`、`both`。
- release threshold 必须小于 press threshold。
- watchdog 必须有安全上下限，例如 50–1000ms。
- `gamepad = switch` 继续表示主机虚拟 HID profile，不能改名或复用为 NXBT 开关。
- NXBT 功能仅在编译支持且目标平台为 Linux 时可选；其他平台显示不可用原因。

### 13.3 工作项

- 配置解析、验证、默认值和文档。
- Web UI 中增加独立的 controller output 区域。
- 只增加 `en` locale。
- 日志包含 controller slot 和状态，但不包含完整蓝牙 MAC。
- Bridge 状态变化时输出一次结构化日志。
- 增加诊断信息：socket 状态、协议版本、adapter availability、controller state、last error、watchdog 状态。
- Moonlight pause：neutralize 并保留逻辑/蓝牙 controller。
- Moonlight resume：rebind，成功后恢复最新状态。
- 明确 terminate：neutralize + detach/free。
- 普通传输断开不得立即销毁已保留 controller。

### 13.4 gtest 场景

- 所有配置合法值和非法值。
- threshold 关系和 watchdog 边界。
- 配置一致性测试。
- pause/resume/terminate 与 Bridge 消息序列。
- Bridge status 到 Sunshine 日志状态的转换。
- `virtual` 默认值确保老配置无行为变化。

### 13.5 验证命令

```bash
test_sunshine \
  --gtest_filter='*Config*:*Nxbt*:*InputGamepadSessionTest*'

git diff --check
```

### 13.6 验收标准

- 不配置 NXBT 时行为与当前 Sunshine 一致。
- 错误配置在启动或配置保存时被拒绝并说明原因。
- Web UI 和文档只修改英文 locale。
- pause/resume 不导致 Switch 每次重新配对。

## 14. 阶段 8：BlueZ 部署和服务化

### 14.1 目标

把阶段 1 已验证的 BlueZ 配置和 Bridge 启动方式固化为可审计、可恢复的部署流程。

### 14.2 工作项

- 提供安装前检查脚本，但默认只读。
- 提供需要管理员明确执行的安装动作：
  - 安装最小 BlueZ drop-in。
  - 建立 `nxbt-bridge` 用户/组或明确暂时以 root 运行的理由。
  - 创建 `/run/nxbt-bridge` runtime directory。
  - 安装 systemd unit。
- systemd unit 必须：
  - 明确依赖 `bluetooth.service`。
  - 失败后有限重启，不形成无限快速重启。
  - 设置安全的 `RuntimeDirectory`、`UMask` 和 socket 权限。
  - 停止时给 Bridge 时间先 neutralize。
- 提供卸载/恢复步骤，恢复 BlueZ 原配置。
- 安装脚本重复执行必须幂等。
- 不自动解除用户已有蓝牙配对，不改其他 adapter alias。

### 14.3 安全要求

- 首版允许 Bridge 以 root 运行，但 Sunshine 必须保持普通用户权限。
- 若尝试 capabilities/polkit 降权，必须单独验证 raw L2CAP、D-Bus ProfileManager 和 adapter 管理所需权限；不得猜测 capability 集合。
- Unix socket 不允许 world-writable。
- 日志不得包含完整蓝牙地址或认证数据。

### 14.4 验证场景

1. 干净系统首次安装。
2. 重复安装不产生重复 ExecStart 参数。
3. Bridge start/stop 不重启 BlueZ。
4. Bridge crash 后 systemd 恢复且先保持 neutral。
5. 卸载后 BlueZ ExecStart 和 drop-in 恢复。
6. 普通用户不能伪造 socket 控制消息。

### 14.5 验收标准

- `systemctl restart nxbt-bridge` 不改变 `bluetooth.service` PID。
- 安装和卸载前后的 `systemctl cat bluetooth.service` diff 可解释且可完全恢复。
- 最终配置中不存在 `--noplugin=*`。
- Sunshine 用户可以连接 socket，但未授权本地用户不能连接。

## 15. 阶段 9：单手柄端到端硬件验收

### 15.1 目标

验证真实 Moonlight → Sunshine → Bridge → Switch 输入链路。

### 15.2 测试准备

- 使用阶段 1 已验证的 ROCK 5B+ 板载蓝牙适配器。
- 保存 Sunshine、Bridge 和脱敏 btmon 日志。
- 关闭会影响判断的 Steam Input 或其他本地主机虚拟手柄消费者。
- `controller_output = nxbt`，避免首轮同时输出到主机 HID。
- 测试前确认 Switch 电量和手柄配对页面状态。

### 15.3 功能矩阵

逐项验证：

- A/B/X/Y，并确认 labels/positions 两种策略。
- D-pad 四方向和对角组合。
- L/R、ZL/ZR。
- PLUS、MINUS、HOME、CAPTURE。
- 左右摇杆按压。
- 双摇杆中心、四个轴极限、圆周和小幅移动。
- 同时按键 + 摇杆。
- 至少两种 Moonlight 客户端手柄类型；若只有一种，明确记录限制。

### 15.4 生命周期矩阵

- 首次配对。
- 已配对快速重连。
- Moonlight 正常断开后重新连接。
- Moonlight pause/resume。
- Sunshine 正常退出。
- Sunshine 强制终止。
- Bridge 正常重启。
- Bridge 强制终止并由 systemd 恢复。
- 板载蓝牙适配器不可用后恢复。
- Switch 睡眠后手工唤醒再重连；不要求蓝牙唤醒。

### 15.5 watchdog 实机验证

在保持以下非中立输入时分别中断链路：

- 持续按住 A。
- 左摇杆保持最大右。
- 同时按住 ZL/ZR。

分别终止 Moonlight 网络、Sunshine IPC 和 Bridge 客户端，Switch 必须在 watchdog 上限内恢复 neutral。

### 15.6 延迟和稳定性记录

- 记录 Moonlight 输入事件到 Bridge 收到 state 的软件时间。
- 记录 Bridge state 到下一次蓝牙报告的等待时间。
- 使用高速摄像或可重复 UI 事件估计端到端输入响应；不得把软件时间误称为严格按键到像素延迟。
- 连续操作 30 分钟，记录断连、重连、CPU、RSS、fd 和输入卡住次数。

### 15.7 验收标准

- 功能矩阵所有首版按键和摇杆通过。
- 三种链路中断均无卡键/卡轴。
- 30 分钟内无不可恢复断连、进程崩溃或资源单调增长。
- BlueZ 在测试期间没有被 Bridge 重启。
- 失败项必须能稳定复现或有明确硬件/环境解释；否则首版不能标记完成。

## 16. 阶段 10：回归、覆盖率与发布门禁

### 16.1 目标

完成全量软件回归、目标机构建、覆盖率和文档门禁。

### 16.2 必须执行

```bash
python3 -m unittest discover -s tools/nxbt_bridge/tests -p 'test_*.py' -v

test_sunshine

./scripts/build-rkmpp.sh

git diff --check
git status --short
```

使用唯一构建脚本提供的文档构建能力验证所有新增 C/C++ 符号满足项目要求。若脚本未提供文档构建，必须将该项记为阻塞，不得另行配置构建目录并声称通过。

### 16.3 Windows 回归

虽然首版 NXBT sink 只在 Linux 启用，共享配置、头文件和条件编译不能破坏 Windows。具备 Windows 环境时执行完整构建和相关测试，所有命令使用规定 MSYS2 UCRT64 前缀。

若没有 Windows 环境：

- 交接中标记 `NOT RUN`，不能写 `PASS`。
- 静态检查所有 Linux-only include 和 symbol 都在正确条件编译内。
- 不得为了通过 Linux 编译而在公共头文件泄漏 Unix socket 类型。

### 16.4 覆盖率要求

重点检查：

- IPC encode/decode 所有错误分支。
- 按钮、摇杆、迟滞转换。
- router 分配回滚。
- latest-state 队列覆盖。
- watchdog、断线和重连。
- Bridge status/error 映射。
- pause/resume/terminate。

changed code 以 100% 覆盖为目标；不可达或平台硬件分支必须说明，并优先通过依赖注入而不是排除覆盖。

### 16.5 验收标准

- 全量 gtest 和 Bridge test 通过。
- RKMPP Release 构建通过。
- Doxygen 构建通过。
- Windows 已通过或明确记录未执行。
- 没有修改无关用户文件。
- 阶段 9 硬件验收证据完整。
- 配置默认值仍为现有虚拟手柄行为。

## 17. 可选里程碑 A：Switch 输出反馈回传 Moonlight

此里程碑只能在首版输入链路稳定后执行。

### 17.1 目标

解析 Switch 发给虚拟 Pro Controller 的输出报告，把可用 rumble 转换为 Sunshine `gamepad_feedback_msg_t` 并回传 Moonlight。

### 17.2 必须先解决

- NXBT 当前 vibration enable ACK 与真实设备行为的差异。
- NXBT 去重输入报告可能影响 Switch output cadence 的问题。
- HD Rumble 到 Moonlight 双电机 rumble 的有损归一化规则。

### 17.3 新 IPC 消息

- Bridge → Sunshine `feedback_rumble`。
- 必须包含 controller id、sequence、low/high frequency normalized intensity。
- Sunshine rebind 后 feedback 必须路由到新的 client-relative index。

### 17.4 验收标准

- 使用捕获的合法 output report golden vectors 完成无硬件测试。
- 游戏内 rumble 能稳定回传，而不只是在“寻找控制器”系统功能中出现。
- Bridge 断开或 rebind 后 feedback 不会发送给错误 Moonlight client。
- 输入链路关闭 rumble 后仍无回归。

## 18. 可选里程碑 B：Moonlight motion 到 Switch IMU

此里程碑不能通过继续发送 NXBT 当前固定 IMU 样本完成。

### 18.1 目标

把 Moonlight accelerometer/gyroscope 数据转换为 Switch Pro Controller report 0x30 中的三个 IMU sample。

### 18.2 工作项

- 冻结 Moonlight 单位与坐标系到 Switch 单位与坐标系的转换。
- 为不同输入采样率建立重采样策略。
- 使用单调时间戳生成三个有序 sample，避免重复或时间倒退。
- 校准数据与实际编码量程一致。
- 缺少 motion 数据时明确发送静止样本或关闭 IMU capability。

### 18.3 验收标准

- 纯逻辑测试覆盖单位、坐标轴、饱和和重采样。
- Switch 校准/测试界面方向正确。
- 长时间运行无漂移爆发或非法 sample。

## 19. 最终完成定义

只有同时满足以下条件，主目标才能标记完成：

1. 阶段 0–10 全部 `PASS`；Windows 可为明确记录的 `NOT RUN`。
2. 单个 Moonlight 手柄能够稳定控制 Nintendo Switch 一代。
3. 配对、快速重连、pause/resume 和明确 terminate 行为符合计划。
4. Moonlight、Sunshine 或 Bridge 任一侧中断都不会产生持续卡键/卡轴。
5. Bridge 启停不修改或重启 BlueZ，部署配置不使用 `--noplugin=*`。
6. Sunshine 默认配置保持原有主机虚拟手柄行为。
7. 所有 C/C++ 新接口有 Doxygen，测试与覆盖率满足仓库门禁。
8. `docs/rkmpp/feature/nxbt-validation/` 中存在逐阶段、已脱敏、可复查的验收记录。

可选里程碑 A/B 不属于首版完成条件，必须单独报告完成状态。
