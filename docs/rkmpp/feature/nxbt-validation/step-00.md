# NXBT validation — step 00 baseline

## Result

**BLOCKED** — the non-invasive host and Sunshine test baseline is complete and
the operator has confirmed the target is a Nintendo Switch (first generation).
Phase 1 still requires a dedicated Bluetooth-adapter decision and an
operator-approved BlueZ test window.

## Repository baseline

| Item | Value |
| --- | --- |
| Sunshine HEAD | `3791805710a3e92e777a9190052a0a0bd7af2f96` |
| NXBT submodule HEAD | `ec4b800ad6c55de96bb6c7f9f84b5bdc59a4c975` |
| Initial worktree state | Modified `docs/rkmpp/AGENTS.md`; then-untracked NXBT plan now archived as `docs/rkmpp/feature/003_nxbt_bridge_implementation_plan.md` |

All pre-existing worktree changes were preserved.  No pairing record, systemd
unit, BlueZ configuration, adapter power state, or Bluetooth connection was
changed during this phase.

## Host and Bluetooth capability

| Item | Observed value |
| --- | --- |
| Target host | ROCK 5B+ / RK3588, Linux aarch64 |
| Distribution | Debian GNU/Linux 12 (bookworm) |
| Kernel | `6.1.84-8-rk2410` |
| BlueZ package and `bluetoothctl` | 5.66 (`5.66-1+deb12u2`) |
| Python | 3.11.2 |
| `dbus-python` | 1.3.2 |
| systemd | 252 |
| Bluetooth service | active; vendor unit has no drop-ins |
| Bluetooth adapter | one powered, pairable adapter (address redacted) |
| Onboard Bluetooth hardware | ROCK 5B+ onboard Bluetooth radio, internally exposed on USB as IMC Networks Bluetooth Radio (`13d3:3572`) |

The configured Bluetooth service executes
`/usr/libexec/bluetooth/bluetoothd` without additional arguments.  Its
`DropInPaths` value was empty.  The `bluetoothd` executable and `rfkill` were
not available through `PATH`, so their requested command-line version/status
checks could not be run; the installed BlueZ package and the running
`bluetoothctl` client provide the version above.

The visible radio is the ROCK 5B+ onboard Bluetooth adapter, implemented by
the onboard RTL8852BE Wi-Fi 6 / Bluetooth 5.2 module and internally exposed
through the board USB hub.  It is not yet confirmed for exclusive NXBT use.
The operator has confirmed the target is a first-generation Nintendo Switch;
no Bluetooth addresses are recorded in this repository.

## Sunshine input baseline

An isolated `cmake-build-nxbt-tests` directory was configured with the ROCK
5B+ LLVM 22.1.6 toolchain, prepared FFmpeg binaries, RKMPP enabled, and tests
enabled.  The following existing test passed:

```text
./cmake-build-nxbt-tests/tests/test_sunshine \
  --gtest_filter='InputGamepadSessionTest.*:*Virtualhid*Gamepad*'

2 tests from InputGamepadSessionTest: PASSED
```

Static inspection confirms that `gamepad = switch` selects the existing
libvirtualhid Switch Pro profile in `src/platform/virtualhid_input.cpp`; there
is no NXBT bridge integration in that path.  This is a source-level baseline,
not evidence of a live Moonlight-to-host virtual-HID session.

## Coverage and remaining work

| Area | Result |
| --- | --- |
| Read-only system inventory | Completed |
| Existing retained-gamepad unit-test baseline | Passed (2/2) |
| Real Moonlight virtual-HID session | Not run; no client session was supplied |
| Nintendo Switch first-generation confirmation | Confirmed by operator |
| NXBT pairing and BlueZ minimal-configuration test | Not run; Phase 1 requires an approved system-impact window |

No interfaces, threads, processes, or resource-ownership contracts were added
in this phase.  No hardware commands with system impact were run.

## Handoff

Before starting Phase 1, an operator must approve exclusive NXBT use of the
ROCK 5B+ onboard Bluetooth adapter and approve a short BlueZ
restart/configuration test window.  Save the resulting desensitized hardware
evidence in `step-01.md`; do not proceed to protocol or Sunshine integration
phases while this baseline remains blocked.

At completion, `git diff --check` passed.  The worktree contains only the
pre-existing `docs/rkmpp/AGENTS.md` modification, the pre-existing untracked
plan, and this new validation record.
