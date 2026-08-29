# NXBT validation — step 04 standalone Bridge and watchdog

## Result

**PASS** — `tools.nxbt_bridge` now provides an independently testable local
Unix `SOCK_SEQPACKET` Bridge with a fake backend, bounded latest-state input,
and monotonic watchdog cleanup. No Bluetooth hardware or system service was
modified in this phase.

## Changes

- Added `Bridge`, the protocol state machine that requires `hello` before
  controller commands, owns each controller slot, returns status/error replies,
  and enforces exclusive slot ownership.
- Added a Unix packet-socket server at the supplied path (default
  `/run/nxbt-bridge/control.sock`). It creates its directory with mode `0770`,
  creates the socket with mode `0660`, replaces only a stale socket, and
  refuses to replace another file type.
- Added state coalescing: each controller has exactly one dirty latest-state
  slot; a flush applies at most that final state. Control messages are handled
  immediately and are never placed behind a state queue.
- Added a 150 ms monotonic watchdog. A stale non-neutral controller is
  neutralized exactly once until a newer valid state arrives. Client disconnect,
  `SIGTERM`, and service shutdown neutralize before detach/backend release.
- Added `FakeBackend` and a hardware-free `NxbtBackend` boundary. The latter
  constructs NXBT only as `Nxbt(manage_bluez=False)` and its unit test injects
  an NXBT substitute to prove that it cannot take the legacy BlueZ-management
  path.

## Automated verification

```text
python3 -m unittest discover -s tools/nxbt_bridge/tests -p 'test_*.py' -v

13 tests: PASSED

python3 -m tools.nxbt_bridge --help

PASSED — exposes fake and NXBT backend options plus socket selection.
```

The tests cover normal handshake/attach/rebind/state/neutralize/detach,
malformed and unnegotiated packets, duplicate controller-slot ownership,
1,000-state latest-state coalescing, out-of-order sequence/timestamp rejection,
149 ms and expired watchdog behavior with an injected fake clock, fake backend
failure status, Unix packet-socket handshake/burst/disconnect cleanup, socket
permissions and stale-socket recovery, non-socket protection, graceful
`SIGTERM`, and `manage_bluez=False` NXBT construction plus complete direct-input
conversion.

The static Bridge path contains no `systemctl`, `daemon-reload`, Bluetooth
restart, `nxbt.conf`, or `--noplugin=*` calls. Any BlueZ group ownership setup
is deliberately deferred to the deployment/service phase; this code only sets
the restrictive socket mode.

## Interfaces and ownership

`UnixBridgeServer` owns the Unix socket and delegates every packet to `Bridge`.
`Bridge` owns logical controller records, latest-state snapshots, and watchdog
state. The selected backend alone owns NXBT and Bluetooth controller resources.
`NxbtBackend` maps logical slots to NXBT controller indices and releases them
only after `Bridge` has neutralized input. `FakeBackend` is exclusively a test
and no-hardware validation backend.

## Hardware status and handoff

**NOT RUN** — no real `--backend=nxbt` service was started, no BlueZ service
was restarted, and no Switch pairing/input test was performed in this phase.
The exact blocker is that this is the plan's hardware-free service milestone;
real Bluetooth smoke must use the approved minimal BlueZ configuration and is
recorded separately.

`git diff --check` passed. The next stage is **stage 5**, which must add the
Sunshine gamepad-output router using a fake NXBT sink only. It must not connect
the new IPC client or real Bridge until stage 6.
