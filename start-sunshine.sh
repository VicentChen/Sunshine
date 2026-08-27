#!/usr/bin/env bash
set -Eeuo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
binary="$project_dir/build-rkmpp-review/sunshine"
config_dir="$project_dir/runtime-home/config/sunshine"
config_file="$config_dir/sunshine.conf"
log_file="$config_dir/sunshine.log"

if [[ ! -x "$binary" ]]; then
  printf "Sunshine executable not found: %s\n" "$binary" >&2
  exit 1
fi

if pgrep -u "$(id -u)" -x sunshine >/dev/null; then
  printf "Sunshine is already running.\n"
  exit 0
fi

# 检查是否缺少必要的 nice 和 admin 权限，如果缺少则提示输入密码并赋予权限
if ! /sbin/getcap "$binary" 2>/dev/null | grep -q "cap_sys_nice"; then
  printf "检测到 Sunshine 缺少提升线程优先级的权限，正在申请 sudo 权限进行修复...\n"
  sudo /sbin/setcap cap_sys_admin,cap_sys_nice+p "$binary" || printf "权限赋予失败，Sunshine 可能仍会出现卡顿。\n" >&2
fi

mkdir -p "$config_dir"
touch "$config_file"

if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  printf "Warning: no graphical session was detected. Sunshine will start, but desktop capture may not work.\n" >&2
fi

nohup "$binary" "$config_file" >>"$log_file" 2>&1 &
printf "Sunshine started (PID %s). Web UI: https://rock-5b-plus.local:47990\n" "$!"
