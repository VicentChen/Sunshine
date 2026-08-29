# NXBT validation — step 05 Sunshine gamepad output router

## Result

**PASS** — Sunshine gamepad output now passes through a documented sink and
router boundary while the default runtime remains virtual-HID only. This phase
uses a fake NXBT sink and does not open the Bridge socket or touch Bluetooth.

## Changes

- Added the documented `input::gamepad::sink_t` lifecycle contract:
  `alloc`, `rebind`, `update`, `neutralize`, and `free`.
- Added deterministic `virtual_output`, `nxbt`, and `both` routing. `both`
  allocates virtual-HID before NXBT, rejects incomplete sink configuration, and
  rolls back successful allocations in reverse order after a later failure.
- Added a virtual-HID adapter around the existing platform API and routed
  Sunshine allocation, rebind, state update, pause neutralization, and final
  release through it. Global and client-relative controller identifiers remain
  intact for every lifecycle operation.
- Added a monotonic five-second failure-log limiter. Every selected sink still
  receives an update when another sink returns a temporary failure.
- Preserved retained sessions: pause neutralizes without releasing the virtual
  device, resume rebinds the same allocation, and explicit session termination
  neutralizes then frees it.

## Automated verification

```text
./scripts/build-rkmpp.sh

PASSED — both C++ targets compiled and linked.

test_sunshine \
  --gtest_filter='GamepadRouterTest.*:InputGamepadSessionTest.*'

9 tests: PASSED

test_sunshine \
  --gtest_filter='GamepadRouterTest.*:InputGamepadSessionTest.*:VirtualHidDeviceTest.*'

21 tests: PASSED

python3 -m unittest discover -s tools/nxbt_bridge/tests -p 'test_*.py' -v

13 tests: PASSED
```

The router tests cover virtual-only and fake-NXBT-only selection, deterministic
dual-sink ordering, missing sink rejection, second-allocation rollback, update
failure isolation, rebind failure aggregation, forward neutralization, reverse
release, all valid global slots 0–15, both invalid boundaries, and injected-time
log limiting without sleeps. The session test observes a pressed virtual-HID
button becoming neutral while the device remains allocated, then verifies reuse
on resume and final release only on termination. All 12 existing
`VirtualHidDeviceTest` cases passed.

`llvm-cov gcov -b` reported the following for
`src/input/gamepad_router.cpp`:

```text
Lines executed:100.00% of 81
Branches executed:100.00% of 94
Taken at least once:82.98% of 94
```

An aggregate default build was also attempted. Its Sunshine executable linked,
but the unrelated web-UI target failed because the host has Node.js 18.20.4 and
the installed Vite/Vue dependencies require Node.js 20 or 22; the explicit
Sunshine and test targets above subsequently completed successfully.

## Interfaces and ownership

`router_t` owns shared references to configured sinks and defines call order;
it does not own sessions, threads, sockets, or Bluetooth resources. The
process-wide Sunshine input subsystem owns the router. `virtual_gamepad_sink_t`
borrows the process-wide platform input handle, whose lifetime extends past the
router and retained gamepad cleanup. A sink owns only the output allocation it
creates for a logical controller. On final release the router neutralizes first,
then frees selected sinks in reverse order.

The input parser remains synchronous with respect to the virtual sink in this
phase. No NXBT IPC client, worker thread, queue, or socket was introduced; those
belong to stage 6.

## Hardware status and handoff

**NOT RUN** — no Bridge process, BlueZ command, Bluetooth adapter, pairing, or
Nintendo Switch hardware was used. This is the plan's hardware-free router
milestone.

`.clang-format` was applied to changed C/C++ files and `git diff --check`
passed. Existing unrelated working-tree changes were preserved. Stage 6 must
implement a non-blocking, reconnecting NXBT IPC sink behind this interface and
must not perform socket I/O on Sunshine's input parser path.
