#!/usr/bin/env bash
# Remove only deployment files and group changes owned by the NXBT installer.
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this uninstaller explicitly as root."
  exit 1
fi

state_dir=/var/lib/nxbt-bridge
dropin_path=/etc/systemd/system/bluetooth.service.d/10-sunshine-nxbt.conf
changed_bluez=0

if [[ ! -f "${state_dir}/installed" ]]; then
  echo "No installer-owned NXBT Bridge deployment was found; nothing was removed."
  exit 1
fi

systemctl disable --now nxbt-bridge.service 2>/dev/null || true
rm -f /etc/systemd/system/nxbt-bridge.service
if [[ -f "${dropin_path}" ]] && grep -q '^ExecStart=.*--compat --noplugin=input$' "${dropin_path}"; then
  rm -f "${dropin_path}"
  changed_bluez=1
elif [[ -f "${dropin_path}" ]]; then
  echo "Preserving modified unrecognized ${dropin_path}; remove it manually after review."
fi
rm -rf /usr/lib/nxbt-bridge

if [[ -f "${state_dir}/membership-added" ]]; then
  sunshine_user="$(sed -n '1p' "${state_dir}/membership-added")"
  if id "${sunshine_user}" >/dev/null 2>&1; then
    gpasswd -d "${sunshine_user}" nxbt-bridge >/dev/null || true
  fi
fi
if [[ -f "${state_dir}/group-created" ]]; then
  groupdel nxbt-bridge 2>/dev/null || true
fi
rm -rf "${state_dir}"

systemctl daemon-reload
if [[ "${changed_bluez}" -eq 1 ]]; then
  systemctl restart bluetooth.service
fi
echo "NXBT Bridge removed; the installer-owned BlueZ drop-in has been restored."
