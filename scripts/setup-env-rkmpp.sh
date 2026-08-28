#!/usr/bin/env bash
# Setup environment for building Sunshine with RKMPP backend on Debian 12 ARM64
set -Eeuo pipefail

readonly PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly LLVM_VERSION=22.1.6
readonly LLVM_DIR="$PROJECT_DIR/build/toolchains/LLVM-$LLVM_VERSION-Linux-ARM64"
readonly LLVM_ARCHIVE="$PROJECT_DIR/build/toolchains/LLVM-$LLVM_VERSION-Linux-ARM64.tar.xz"
readonly LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/LLVM-$LLVM_VERSION-Linux-ARM64.tar.xz"

BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-rkmpp-review}"
BREW_BIN="${BREW_BIN:-}"
if [[ -z "$BREW_BIN" && -x /home/linuxbrew/.linuxbrew/bin/brew ]]; then
  BREW_BIN=/home/linuxbrew/.linuxbrew/bin/brew
fi
BREW_BIN="${BREW_BIN:-brew}"

die() {
  printf "error: %s\n" "$*" >&2
  exit 1
}

run() {
  printf "+ "
  printf "%q " "$@"
  printf "\n"
  "$@"
}

install_apt_dependencies() {
  local packages=(
    build-essential ca-certificates curl git pkg-config xz-utils
    python3-jinja2 python3-setuptools
    libayatana-appindicator3-dev libcap-dev libcurl4-openssl-dev
    libdrm-dev libevdev-dev libgbm-dev libminiupnpc-dev libnotify-dev
    libnuma-dev libopus-dev libpipewire-0.3-dev libpulse-dev libssl-dev
    libsystemd-dev libudev-dev libva-dev libvulkan-dev libwayland-dev
    libx11-dev libxcb-shm0-dev libxcb-xfixes0-dev libxcb1-dev
    libxfixes-dev libxrandr-dev libxtst-dev xvfb
  )
  run sudo apt-get update
  run sudo apt-get install --yes --no-install-recommends "${packages[@]}"
}

install_brew_dependencies() {
  local formula
  local formulae=(
    cmake ninja node pkgconf file desktop-file-utils glslang
  )
  command -v "$BREW_BIN" >/dev/null 2>&1 || die "Homebrew was not found; install it first or set BREW_BIN."
  for formula in "${formulae[@]}"; do
    "$BREW_BIN" list --versions "$formula" >/dev/null 2>&1 || run "$BREW_BIN" install "$formula"
  done
}

require_rkmpp() {
  [[ -f /usr/include/rockchip/rk_mpi.h ]] || die "missing /usr/include/rockchip/rk_mpi.h; install the Rockchip MPP development package from the board OS repository."
  /usr/bin/pkg-config --exists rockchip_mpp || die "pkg-config cannot find rockchip_mpp."
}

download_file() {
  local url="$1"
  local output="$2"
  [[ -s "$output" ]] || run curl --fail --location --retry 3 --output "$output" "$url"
}

prepare_llvm() {
  [[ -x "$LLVM_DIR/bin/clang" ]] && return
  run mkdir -p "$(dirname "$LLVM_ARCHIVE")"
  download_file "$LLVM_URL" "$LLVM_ARCHIVE"
  run tar -xJf "$LLVM_ARCHIVE" -C "$(dirname "$LLVM_DIR")"
  [[ -x "$LLVM_DIR/bin/clang" ]] || die "LLVM extraction did not produce $LLVM_DIR/bin/clang."
}

prepare_ffmpeg() {
  local build_deps_dir="$PROJECT_DIR/third-party/build-deps"
  local tag
  local archive_dir
  local archive
  local url

  [[ -d "$BUILD_DIR/_deps/ffmpeg" ]] && return
  tag="$(git -C "$build_deps_dir" describe --tags --exact-match HEAD 2>/dev/null || true)"
  [[ -n "$tag" ]] || die "cannot determine the checked-out third-party/build-deps release tag."
  archive_dir="$BUILD_DIR/_deps/ffmpeg-$tag"
  archive="$archive_dir/Linux-aarch64-ffmpeg.tar.gz"
  url="https://github.com/LizardByte/build-deps/releases/download/$tag/Linux-aarch64-ffmpeg.tar.gz"
  run mkdir -p "$archive_dir" "$BUILD_DIR/_deps"
  download_file "$url" "$archive"
  run tar -xzf "$archive" -C "$BUILD_DIR/_deps"
  [[ -d "$BUILD_DIR/_deps/ffmpeg" ]] || die "FFmpeg archive did not produce $BUILD_DIR/_deps/ffmpeg."
}

main() {
  [[ "$(uname -s)" == Linux && "$(uname -m)" == aarch64 ]] || die "this script supports Linux ARM64 only."

  if [[ "${SKIP_APT:-0}" != 1 ]]; then
    install_apt_dependencies
  fi
  if [[ "${SKIP_BREW:-0}" != 1 ]]; then
    install_brew_dependencies
  fi

  command -v "$BREW_BIN" >/dev/null 2>&1 || die "Homebrew was not found."
  require_rkmpp

  if [[ "${SKIP_SUBMODULES:-0}" != 1 ]]; then
    run git -C "$PROJECT_DIR" submodule update --init --recursive
  fi

  prepare_llvm
  prepare_ffmpeg

  printf "\nEnvironment setup complete. You can now run build-rkmpp.sh\n"
}

main "$@"
