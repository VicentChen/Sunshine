#!/usr/bin/env bash
# Capture sanitized evidence for the stage-9 Moonlight-to-Switch hardware run.
set -euo pipefail

if [[ "$#" -lt 1 || "$#" -gt 2 ]]; then
  echo "Usage: $0 OUTPUT_DIRECTORY [SOAK_SECONDS]"
  exit 2
fi

output_dir="$1"
soak_seconds="${2:-1800}"
if [[ ! "${soak_seconds}" =~ ^[0-9]+$ ]] || [[ "${soak_seconds}" -lt 1 ]]; then
  echo "SOAK_SECONDS must be a positive integer."
  exit 2
fi

mkdir -p "${output_dir}"
output_dir="$(cd "${output_dir}" && pwd)"
started_at="$(date --iso-8601=seconds)"

sanitize() {
  sed -E 's/([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}/[bluetooth-address-redacted]/g'
}

capture_snapshot() {
  systemctl show bluetooth.service nxbt-bridge.service \
    --property=Id --property=ActiveState --property=SubState --property=MainPID \
    --property=FragmentPath --property=DropInPaths --property=ExecStart 2>&1 | sanitize >"${output_dir}/service-state.txt"
  if [[ -S /run/nxbt-bridge/control.sock ]]; then
    stat -c 'socket=%n mode=%a owner=%U group=%G' /run/nxbt-bridge/control.sock >"${output_dir}/socket-state.txt"
  else
    echo "NXBT Bridge socket is absent" >"${output_dir}/socket-state.txt"
  fi
}

capture_logs() {
  journalctl --since "${started_at}" -u bluetooth.service -u nxbt-bridge.service --no-pager 2>&1 | sanitize >"${output_dir}/services.log"
  journalctl --since "${started_at}" _COMM=sunshine --no-pager 2>&1 | sanitize >"${output_dir}/sunshine.log"
}

trap capture_logs EXIT
capture_snapshot

bluetooth_pid="$(systemctl show bluetooth.service --property=MainPID --value)"
bridge_pid="$(systemctl show nxbt-bridge.service --property=MainPID --value)"
deadline=$((SECONDS + soak_seconds))
while ((SECONDS < deadline)); do
  current_bluetooth_pid="$(systemctl show bluetooth.service --property=MainPID --value)"
  current_bridge_pid="$(systemctl show nxbt-bridge.service --property=MainPID --value)"
  if [[ "${current_bluetooth_pid}" != "${bluetooth_pid}" ]]; then
    echo "FAIL: bluetooth.service PID changed during the soak." | tee -a "${output_dir}/soak.txt"
    exit 1
  fi
  if [[ "${current_bridge_pid}" != "${bridge_pid}" ]]; then
    echo "FAIL: nxbt-bridge.service PID changed during the soak." | tee -a "${output_dir}/soak.txt"
    exit 1
  fi
  if [[ ! -S /run/nxbt-bridge/control.sock ]]; then
    echo "FAIL: NXBT Bridge socket disappeared during the soak." | tee -a "${output_dir}/soak.txt"
    exit 1
  fi
  sleep 5
done

echo "PASS: services and restricted socket remained stable for ${soak_seconds} seconds." | tee "${output_dir}/soak.txt"
echo "Complete the manual control and interruption matrix in docs/rkmpp/feature/nxbt-validation/step-09.md."
