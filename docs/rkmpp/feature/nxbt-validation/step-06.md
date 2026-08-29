# NXBT validation — step 06 Sunshine IPC client and sink

## Result

**PASS** — Sunshine now has a non-blocking, reconnecting C++ NXBT client and a
documented gamepad sink. The input-facing path only updates bounded in-memory
state; all connect, handshake, send, receive, heartbeat, and reconnect work is
owned by one IPC worker thread. This stage does not yet select the NXBT sink at
runtime; configuration and production lifecycle wiring remain stage 7 work.

## Changes

- Added `src/input/nxbt_client.{h,cpp}` with an injectable bounded packet
  transport, a production Linux Unix `SOCK_SEQPACKET` transport, protocol-v1
  negotiation, ordered lifecycle controls, heartbeat, reconnect, and
  rate-limited observable events.
- Added `src/input/nxbt_sink.{h,cpp}` to implement the stage-5 gamepad sink
  contract, map Sunshine buttons and triggers, retain per-slot trigger
  hysteresis, issue per-controller sequences, and timestamp complete states.
- Added `tests/unit/test_nxbt_client.cpp` with injected transports and a real
  temporary Unix packet server. Tests do not use `/run`, root, BlueZ, NXBT, or
  Bluetooth hardware.
- Registered the client and sink sources in the Sunshine target.

Each of Sunshine's 16 supported controller slots owns at most one pending
latest state. Lifecycle controls use a 64-message bounded queue; reaching that
bound forces a clean reconnect, whose socket disconnect neutralizes Bridge
state and whose desired-state replay reconstructs the final logical state.

After every successful reconnect, retained controllers are replayed in the
protocol-valid order `attach`, `neutralize`, then latest state. Sending
`neutralize` before `attach` would be rejected by the stage-4 Bridge because
the slot would not yet have an owner.

## Automated verification

```text
test_sunshine \
  --gtest_filter='NxbtClientTest.*:NxbtSinkTest.*:NxbtProtocolTest.*:NxbtMappingTest.*:GamepadRouterTest.*:InputGamepadSessionTest.*:VirtualHidDeviceTest.*'

39 tests from 7 suites: PASSED

python3 -m unittest discover -s tools/nxbt_bridge/tests -p 'test_*.py' -v

13 tests: PASSED

./scripts/build-rkmpp.sh

PASSED — the RKMPP Sunshine executable and web assets built successfully.
```

The tests cover normal hello/attach/state/neutralize/detach, production Unix
packet boundaries, missing socket behavior, permission denial, version
rejection, malformed replies, explicit Bridge errors, controller status,
invalid lifecycle calls, 1,000-state coalescing, disconnect/restart replay,
heartbeat timeout and pong recovery, error-event rate limiting, bounded
destruction with an unresponsive peer, complete sink mapping, and prior router,
retained-session, mapping, protocol, and virtual-HID behavior.

`llvm-cov gcov -b` reported:

```text
src/input/nxbt_client.cpp: lines 83.98%, branches executed 92.29%
src/input/nxbt_sink.cpp:   lines 91.89%, branches executed 100.00%
```

Uncovered client lines are predominantly forced failures of individual socket,
`fcntl`, `poll`, `getsockopt`, short-send, and truncated-kernel-packet branches
that are not safely reproducible through a real Unix socket. Semantic protocol,
lifecycle, timeout, recovery, and bounded-queue branches are covered. The
changed-code 100% line target was therefore not reached and remains recorded
rather than overstated.

## ThreadSanitizer

An independent ThreadSanitizer target compiled successfully with LLVM 22. A
halt-on-error run was stopped before gtest execution by a
pre-existing startup heap-use-after-free between `src/config.cpp` static
initialization and `src/platform/linux/input/virtualhid.cpp`. A continuation
run completed all 17 selected `NxbtClientTest`, `NxbtSinkTest`, and
`GamepadRouterTest` cases, but also reported pre-existing races in the test
process's asynchronous Boost.Log setup and formatter. No sanitizer summary or
stack referenced `src/input/nxbt_client.cpp`, `src/input/nxbt_sink.cpp`, or the
NXBT test worker. Because the unrelated process-wide findings contaminate the
run, this is recorded as **INCONCLUSIVE**, not as a clean TSan pass.

## Interfaces and ownership

`input::nxbt::client_t` owns its worker and desired controller state. The worker
is the sole owner and caller of each live `transport_t`. Public lifecycle and
state methods take only short mutex sections and perform no socket operation.
The `transport_t` contract requires every injected operation to respect its
supplied timeout, which bounds shutdown even when a peer does not respond.

`input::nxbt::sink_t` owns per-slot mapping state and a shared client reference.
It does not own platform virtual-HID, Bluetooth, BlueZ, or NXBT resources.
Worker events expose system errors, protocol errors, and controller status for
stage-7 logging and diagnostics. Non-Linux builds retain a documented
unsupported production transport that fails immediately; they do not attempt
Unix or Bluetooth operations.

## Hardware status and handoff

**NOT RUN** — no BlueZ command, Bridge NXBT backend, adapter operation, pairing,
or Nintendo Switch input was used. Stage 7 must create and select this sink from
configuration, connect retained gamepad lifecycle and status logging, and keep
the existing virtual-only default unchanged.

The production client currently exists behind the router boundary but is not
constructed by `input::init()`. This is intentional stage separation, not an
active runtime feature. Stage 7 must also decide how event callback details are
rendered in Sunshine logs without exposing a Bluetooth address.
