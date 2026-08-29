# NXBT validation — step 03 runtime boundary hardening

## Result

**PASS** — NXBT now supports `manage_bluez=False`; the hardware smoke test
created and reconnected a Pro Controller without NXBT restarting BlueZ.

## Changes

- Added `manage_bluez` to `Nxbt`; the backwards-compatible default remains
  `True`, while Bridge callers use `False`.
- Added bounded controller-start and connection waits.
- Joined and force-killed stubborn controller children during normal removal
  and manager shutdown.
- Restored adapter alias, pairable settings, and discoverability after a
  controller server exits; the registered profile is unregistered during
  cleanup.
- Added a concise fork Modification Note to the NXBT README and no-hardware
  lifecycle tests.

## Hardware smoke

With the temporary minimal BlueZ configuration from step 01, the modified
NXBT reconnected to the paired Switch and delivered A plus D-pad right.  The
BlueZ daemon PID was identical immediately before and after the NXBT process,
demonstrating that `manage_bluez=False` did not restart the service.  Adapter
alias and discoverability were restored when NXBT exited.

The runtime override and temporary log were removed.  BlueZ finished with its
vendor `ExecStart`, no drop-ins, and a powered non-discoverable adapter.  No
Bluetooth address, pairing data, or credentials are recorded here.

## Automated checks

```text
.venv/nxbt/bin/python -m unittest discover -s third-party/nxbt/tests \
  -p 'test_*.py' -v

2 tests: PASSED
```

The tests cover bounded connection waiting and termination/join/kill cleanup
for a stuck child.  `git diff --check` passed for both the Sunshine tree and
the NXBT submodule.

## Remaining risk

`SIGKILL` cannot run in-process cleanup.  A production Bridge service must
therefore remain the supervisor for NXBT and neutralize input before shutdown;
the later Bridge watchdog milestone remains required.
