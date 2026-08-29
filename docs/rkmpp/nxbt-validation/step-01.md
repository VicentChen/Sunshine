# NXBT validation — step 01 BlueZ and hardware validation

## Result

**PASS** — the operator confirmed the Nintendo Switch is first generation,
selected exclusive use of the onboard Bluetooth adapter, and approved the
temporary BlueZ restart.  NXBT created a Pro Controller, the Switch connected,
and the operator observed the virtual controller and an injected A input.

## Preconditions and baseline

| Item | Result |
| --- | --- |
| Nintendo Switch generation | First generation, confirmed by operator |
| Adapter selection | Onboard Bluetooth, approved for exclusive NXBT use |
| Bluetooth service restart | Approved by operator |
| Connected Bluetooth devices | None reported before testing |
| `bluetooth.service` | Active; vendor `ExecStart` has no extra arguments |
| Runtime or persistent BlueZ drop-ins | None |
| Original Bluetooth service PID | 839 |
| NXBT environment | Installed at `.venv/nxbt`; the directory is ignored by Git |
| Privileged operation | Performed with operator-authorized test-environment access |

The selected radio is the ROCK 5B+ onboard Bluetooth adapter, implemented by
the onboard Wi-Fi/Bluetooth module and internally exposed through the board
USB hub.  No Bluetooth address, pairing key, or other authentication data is
recorded here.

## Commands and results

The following read-only checks were performed:

```text
git rev-parse HEAD
git -C third-party/nxbt rev-parse HEAD
git status --short
systemctl show bluetooth.service -p ActiveState -p MainPID -p FragmentPath \
  -p DropInPaths -p ExecStart
bluetoothctl devices Connected
python3 -c "import nxbt"
sudo -n true
```

The repository and NXBT submodule remain at the hashes recorded in step 00.
There were no connected Bluetooth devices before testing.  The temporary
runtime override cleared the vendor `ExecStart` and used only:

```text
/usr/libexec/bluetooth/bluetoothd --compat --noplugin=input
```

BlueZ logged that it excluded only the `input` plugin.  No `--noplugin=*`
argument was used.  The runtime override was removed after every test run.

## Hardware results

| Scenario | Result |
| --- | --- |
| First pairing | PASS — one adapter was found; the Switch connected to the virtual Pro Controller |
| Initial input smoke | PASS — the operator observed injected A and D-pad right input |
| Normal exit then reconnect | PASS — a previously paired Switch reconnected; test returned exit code 0 |
| NXBT parent `SIGKILL` | PASS — four orphaned NXBT child processes and non-neutral adapter presentation were detected |
| Manual cleanup and restart | PASS — orphan processes were terminated, then NXBT reconnected successfully |
| Repository-local virtual environment | PASS — `.venv/nxbt` independently completed a reconnect test with exit code 0 |
| RKMPP environment setup | PASS — `scripts/setup-env-rkmpp.sh` creates or refreshes `.venv/nxbt` |
| BlueZ and normal adapter recovery | PASS — original `ExecStart`, empty `DropInPaths`, normal alias, and non-discoverable state were restored |

The abnormal-termination result is an expected weakness of the current NXBT
fork, not a release-quality cleanup result: killing its parent left child
processes alive and the adapter named `Pro Controller` and discoverable.  This
is recorded as a required remediation for Phase 3.  Manual cleanup terminated
the exact child processes, restored the adapter alias and discoverability, and
restarted BlueZ with its original vendor command.  No NXBT process remained.

## Required recovery and next action

The smoke script replaces the current NXBT constructor's legacy
`toggle_clean_bluez()` function before creating `Nxbt`.  This prevents the
test from writing its `nxbt.conf`, enabling `--noplugin=*`, or restarting BlueZ
itself.  Phase 3 must replace this test-only guard with a supported
`manage_bluez=False` API and fix process/profile/adapter cleanup.

The temporary test assets are `bluez-minimal-runtime.conf` and
`nxbt_phase1_smoke.py` in this directory.  The latter contains no address,
pairing, or credential data.  `/tmp` logs and the runtime override were
removed after verification.

## Final checks

`git diff --check` passed for tracked changes.  The worktree still contains
only the pre-existing `docs/rkmpp/AGENTS.md` modification, the pre-existing
untracked plan, and the validation records created for these steps.  The
repository-local `.venv/` is already ignored by `.gitignore`.
`scripts/setup-env-rkmpp.sh` installs `python3-venv`, creates the environment
with system `dbus-python` access, installs the local NXBT package without
network Python dependencies, and verifies its imports.
