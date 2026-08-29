# NXBT validation — step 08 deployment and service

## Status

**TARGET DEPLOYMENT PASS; DISRUPTIVE RECOVERY CHECKS PENDING** — the operator installed the service on the target,
the production Bridge is active and enabled, and an authorized local client completed a protocol handshake. Forced
service termination, an unrelated-user denial check, and uninstall restoration remain part of the hardware window.

## Implemented controls

- `scripts/nxbt-bridge-preflight.sh` is read-only and reports BlueZ, adapter, Python, and socket readiness.
- `scripts/install-nxbt-bridge.sh` installs only `--compat --noplugin=input`, records installer-owned group changes, and
  restarts BlueZ only when that drop-in actually changes.
- `scripts/uninstall-nxbt-bridge.sh` removes the installer-owned unit, drop-in, files, and recorded membership change.
- `nxbt-bridge.service` requires and starts after `bluetooth.service`, uses bounded restart, a group-restricted runtime
  directory, mode `0660` socket via `UMask=0007`, and a five-second graceful SIGTERM window.
- The first service version runs as root because raw L2CAP and BlueZ ProfileManager permissions have not yet been
  validated under a narrower capability or policy set. Sunshine remains unprivileged and receives socket access only
  through membership in `nxbt-bridge`.
- Neither Bridge startup nor shutdown edits systemd or restarts BlueZ. The production backend uses
  `manage_bluez=False` and performs a read-only effective-ExecStart check.
- No deployment action unpairs devices or modifies an adapter alias.

## Automated evidence

- Python Bridge/protocol/deployment tests cover restricted socket permissions, stale socket recovery, SIGTERM cleanup,
  watchdog neutralization, `manage_bluez=False`, minimal BlueZ argument validation, and static unit/script safeguards.
- Shell syntax checks cover preflight, install, uninstall, and hardware evidence scripts.

## Target deployment evidence

- The 2026-08-29 target preflight passed service, adapter, Python import, and minimal BlueZ argument checks.
- `bluetooth.service` is active with exactly `--compat --noplugin=input` from the installer-owned drop-in.
- `nxbt-bridge.service` is loaded, active, enabled, and configured with `Restart=on-failure`.
- The runtime directory is `root:nxbt-bridge` mode `0770`; its control socket is `root:nxbt-bridge` mode `0660`.
- The Sunshine account is a member of `nxbt-bridge`. A fresh group context opened the socket and received a version 1
  `HELLO_ACK` from the production service.
- The pre-existing login session does not inherit a newly added supplementary group, so it must be refreshed before
  launching Sunshine for hardware acceptance.

## Target acceptance commands

Run these only during an approved maintenance window because install/uninstall can restart BlueZ once:

```bash
scripts/nxbt-bridge-preflight.sh
sudo scripts/install-nxbt-bridge.sh <sunshine-user>
systemctl show bluetooth.service -p MainPID -p ExecStart -p DropInPaths
sudo systemctl restart nxbt-bridge.service
systemctl show bluetooth.service -p MainPID
sudo scripts/install-nxbt-bridge.sh <sunshine-user>
sudo scripts/uninstall-nxbt-bridge.sh
```

Record that the BlueZ PID is unchanged across the Bridge-only restart, the second install adds no arguments, an
authorized Sunshine process can open the socket, an unrelated local user receives permission denied, and uninstall
restores the pre-install `systemctl cat bluetooth.service` output.
