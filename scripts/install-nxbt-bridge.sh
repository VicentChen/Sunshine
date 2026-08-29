#!/usr/bin/env bash
# Install the NXBT Bridge and the one-time minimal BlueZ override.
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this installer explicitly as root."
  exit 1
fi

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sunshine_user="${1:-}"
install_dir=/usr/lib/nxbt-bridge
state_dir=/var/lib/nxbt-bridge
dropin_dir=/etc/systemd/system/bluetooth.service.d
dropin_path="${dropin_dir}/10-sunshine-nxbt.conf"
unit_path=/etc/systemd/system/nxbt-bridge.service
changed_bluez=0

bash "${project_dir}/scripts/nxbt-bridge-preflight.sh"

if ! getent group nxbt-bridge >/dev/null; then
  groupadd --system nxbt-bridge
  install -d -m 0750 "${state_dir}"
  touch "${state_dir}/group-created"
else
  install -d -m 0750 "${state_dir}"
fi

if [[ -n "${sunshine_user}" ]]; then
  if ! id "${sunshine_user}" >/dev/null 2>&1; then
    echo "Unknown Sunshine user: ${sunshine_user}"
    exit 1
  fi
  if ! id -nG "${sunshine_user}" | tr ' ' '\n' | grep -qx nxbt-bridge; then
    usermod -a -G nxbt-bridge "${sunshine_user}"
    printf '%s\n' "${sunshine_user}" >"${state_dir}/membership-added"
  fi
fi

bluetoothd_path="$(command -v bluetoothd || true)"
if [[ -z "${bluetoothd_path}" ]]; then
  for candidate in /usr/lib/bluetooth/bluetoothd /usr/libexec/bluetooth/bluetoothd; do
    if [[ -x "${candidate}" ]]; then
      bluetoothd_path="${candidate}"
      break
    fi
  done
fi
if [[ -z "${bluetoothd_path}" ]]; then
  echo "Unable to locate bluetoothd."
  exit 1
fi

if [[ -f "${unit_path}" && ! -f "${state_dir}/installed" ]]; then
  echo "Refusing to replace an existing unowned ${unit_path}."
  exit 1
fi
install -d -m 0755 "${install_dir}/nxbt_bridge" "${install_dir}/nxbt" "${dropin_dir}"
install -m 0644 "${project_dir}/tools/nxbt_bridge/"*.py "${install_dir}/nxbt_bridge/"
cp -a "${project_dir}/third-party/nxbt/nxbt/." "${install_dir}/nxbt/"
touch "${state_dir}/installed"
install -m 0644 "${project_dir}/tools/nxbt_bridge/systemd/nxbt-bridge.service" "${unit_path}"

generated_dropin="$(mktemp)"
trap 'rm -f "${generated_dropin}"' EXIT
sed "s|@BLUETOOTHD@|${bluetoothd_path}|g" "${project_dir}/tools/nxbt_bridge/systemd/bluetooth-nxbt.conf.in" >"${generated_dropin}"
if [[ ! -f "${dropin_path}" ]] || ! cmp -s "${generated_dropin}" "${dropin_path}"; then
  if [[ -f "${dropin_path}" ]] && ! grep -q '^ExecStart=.*--compat --noplugin=input$' "${dropin_path}"; then
    echo "Refusing to replace an unrecognized existing ${dropin_path}."
    exit 1
  fi
  install -m 0644 "${generated_dropin}" "${dropin_path}"
  changed_bluez=1
fi

systemctl daemon-reload
if [[ "${changed_bluez}" -eq 1 ]]; then
  systemctl restart bluetooth.service
fi
systemctl enable --now nxbt-bridge.service

echo "NXBT Bridge installed. Socket access is limited to root and members of nxbt-bridge."
if [[ -z "${sunshine_user}" ]]; then
  echo "Add the account running Sunshine to nxbt-bridge, then restart that user session."
fi
