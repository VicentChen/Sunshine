#!/usr/bin/env bash
# Read-only preflight for the Sunshine NXBT Bridge deployment.
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
status=0

check() {
  local description="$1"
  shift
  if "$@" >/dev/null 2>&1; then
    echo "PASS: ${description}"
  else
    echo "FAIL: ${description}"
    status=1
  fi
}

check "systemd can read bluetooth.service" systemctl show bluetooth.service --property=ActiveState --property=MainPID --property=FragmentPath --property=DropInPaths --property=ExecStart
check "bluetooth.service is active" systemctl is-active --quiet bluetooth.service
check "Python can import the bundled Bridge" env PYTHONPATH="${project_dir}/tools" python3 -c "import nxbt_bridge"
check "Python can import NXBT" env PYTHONPATH="${project_dir}/third-party/nxbt" python3 -c "import nxbt"
check "BlueZ exposes at least one adapter object" bash -c "busctl tree org.bluez 2>/dev/null | grep -q '/org/bluez/hci'"

exec_start="$(systemctl show bluetooth.service --property=ExecStart --value 2>/dev/null || true)"
if [[ "${exec_start}" == *"--noplugin=*"* ]]; then
  echo "FAIL: unsafe --noplugin=* is present"
  status=1
elif [[ "${exec_start}" == *"--compat"* && "${exec_start}" == *"--noplugin=input"* ]]; then
  echo "PASS: BlueZ uses the minimal NXBT arguments"
else
  echo "INFO: BlueZ still needs the deployment-time --compat --noplugin=input drop-in"
fi

if [[ -S /run/nxbt-bridge/control.sock ]]; then
  socket_mode="$(stat -c '%a' /run/nxbt-bridge/control.sock)"
  if [[ "${socket_mode}" == "660" ]]; then
    echo "PASS: existing Bridge socket is mode 0660"
  else
    echo "FAIL: existing Bridge socket mode is ${socket_mode}, expected 660"
    status=1
  fi
fi

exit "${status}"
