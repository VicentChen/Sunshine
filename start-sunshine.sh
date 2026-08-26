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

mkdir -p "$config_dir"
touch "$config_file"

if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  printf "Warning: no graphical session was detected. Sunshine will start, but desktop capture may not work.\n" >&2
fi

nohup "$binary" "$config_file" >>"$log_file" 2>&1 &
printf "Sunshine started (PID %s). Web UI: https://rock-5b-plus.local:47990\n" "$!"
