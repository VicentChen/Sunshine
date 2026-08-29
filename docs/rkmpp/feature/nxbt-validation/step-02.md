# NXBT validation — step 02 IPC and input-mapping contract

## Result

**PASS** — protocol version 1 and the Sunshine-to-NXBT pure input mapping are
implemented without requiring BlueZ, NXBT processes, root access, or Bluetooth
hardware.

## Changes

- Added documented C++ protocol and mapping interfaces in `src/input/`.
- Added explicit little-endian C++ and Python encoders/decoders; no serialized
  C++ structure depends on compiler padding.
- Added `docs/rkmpp/feature/nxbt-protocol-v1.md`, which freezes the 12-byte header,
  every version-1 payload, status/error values, sequence-wrap rule, and stable
  golden vectors.
- Added the NXBT mapping contract: labels and physical-position face-button
  policies, capture mapping, ignored unsupported buttons, ZL/ZR hysteresis,
  axis limiting, and separate left/right NXBT stick calibrations.

The state payload is 28 bytes and contains the Bridge slot, full NXBT button
mask, original 8-bit trigger values, four signed Sunshine stick axes,
per-controller `uint32` sequence, and monotonic microsecond timestamp.

## Automated verification

```text
python3 -m unittest discover -s tools/nxbt_bridge/tests -p 'test_*.py' -v

3 tests: PASSED

test_sunshine \
  --gtest_filter='NxbtProtocolTest.*:NxbtMappingTest.*'

8 tests from 2 suites: PASSED

test_sunshine \
  --gtest_filter='InputGamepadSessionTest.*:*Virtualhid*Gamepad*'

2 existing retained-gamepad tests: PASSED
```

The gtests cover every message type, golden-vector encode/decode round trips,
bad magic, unsupported version, unknown type, invalid and truncated length,
trailing bytes, sequence wrap-around, every supported button, ignored
paddle/touchpad bits, both face-button policies, trigger thresholds
`0/47/48/49/63/64/65/255`, and stick extrema/center with distinct
calibrations.

`llvm-cov gcov` reported 100.00% line and 100.00% branch coverage for both
`src/input/nxbt_protocol.cpp` (149 lines, 135 branches) and
`src/input/nxbt_mapping.cpp` (61 lines, 18 branches). The generated coverage
artifacts and stale pre-rebuild counters were moved outside the worktree.

## Interfaces and ownership

`input::nxbt::message_t` is the immutable wire-model value exchanged by the
future Sunshine client and Bridge service. `controller_state_t` owns one full
logical controller snapshot. `trigger_state_t` is intentionally caller-owned,
so a future Bridge slot keeps its own hysteresis state. No worker thread,
socket, process, Bluetooth adapter, or system resource was created in this
phase.

## Final checks and handoff

`git diff --check` passed. The C++ files were formatted with the repository's
LLVM 22.1.6 `clang-format`. No hardware commands or system-impact commands
were run.

The next phase is **stage 4**, the standalone Python `nxbt-bridge` service.
It must use these exact packet layouts and golden vectors, import NXBT using
`manage_bluez=False`, keep high-frequency state as latest-state-wins, and
provide fake-backend/watchdog tests. Stage 3's existing cleanup verification
remains the prerequisite for real-hardware Bridge smoke testing.
