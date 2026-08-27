#!/usr/bin/env bash
# Build Sunshine with the RKMPP backend on Debian 12 ARM64 / ROCK 5B+.
set -Eeuo pipefail

readonly PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly LLVM_VERSION=22.1.6
readonly LLVM_DIR="$PROJECT_DIR/build/toolchains/LLVM-$LLVM_VERSION-Linux-ARM64"
readonly LLVM_ARCHIVE="$PROJECT_DIR/build/toolchains/LLVM-$LLVM_VERSION-Linux-ARM64.tar.xz"
readonly LLVM_URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION/LLVM-$LLVM_VERSION-Linux-ARM64.tar.xz"

BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-rkmpp-review}"
JOBS="${JOBS:-4}"
BREW_BIN="${BREW_BIN:-}"
if [[ -z "$BREW_BIN" && -x /home/linuxbrew/.linuxbrew/bin/brew ]]; then
  BREW_BIN=/home/linuxbrew/.linuxbrew/bin/brew
fi
BREW_BIN="${BREW_BIN:-brew}"

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

run() {
  printf '+ '
  printf '%q ' "$@"
  printf '\n'
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
  local brew_prefix
  brew_prefix="$("$BREW_BIN" --prefix)"
  export PATH="$brew_prefix/bin:$PATH"

  [[ -x "$brew_prefix/bin/cmake" ]] || die "Homebrew CMake is missing."
  [[ -x "$brew_prefix/bin/ninja" ]] || die "Homebrew Ninja is missing."
  [[ -x "$brew_prefix/bin/npm" ]] || die "Homebrew npm is missing."
  require_rkmpp

  if [[ "${SKIP_SUBMODULES:-0}" != 1 ]]; then
    run git -C "$PROJECT_DIR" submodule update --init --recursive
  fi

  prepare_llvm
  prepare_ffmpeg

  local multiarch
  local link_flags
  multiarch="$("/usr/bin/gcc" -print-multiarch)"
  [[ -n "$multiarch" ]] || die "unable to determine the Debian multiarch directory."
  link_flags="-stdlib=libc++ -L$LLVM_DIR/lib/$multiarch -Wl,-rpath,$LLVM_DIR/lib/$multiarch -Wl,--no-as-needed -lc++abi -lunwind -Wl,--as-needed"

  run "$brew_prefix/bin/cmake" -S "$PROJECT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$LLVM_DIR/bin/clang" \
    -DCMAKE_CXX_COMPILER="$LLVM_DIR/bin/clang++" \
    -DCMAKE_C_FLAGS="-fuse-ld=/usr/bin/ld" \
    -DCMAKE_CXX_FLAGS="-stdlib=libc++ -include span -fuse-ld=/usr/bin/ld" \
    -DCMAKE_EXE_LINKER_FLAGS="$link_flags" \
    -DCMAKE_SHARED_LINKER_FLAGS="$link_flags" \
    -DCMAKE_LIBRARY_PATH="/usr/lib/$multiarch" \
    -DPKG_CONFIG_EXECUTABLE=/usr/bin/pkg-config \
    -DNPM="$brew_prefix/bin/npm" \
    -DFFMPEG_PREPARED_BINARIES="$BUILD_DIR/_deps/ffmpeg" \
    -DEVDEV_LIBRARY="/usr/lib/$multiarch/libevdev.a" \
    -DBUILD_DOCS=OFF \
    -DBUILD_TESTS=ON \
    -DBUILD_TESTING=ON \
    -DSUNSHINE_ENABLE_CUDA=OFF \
    -DSUNSHINE_ENABLE_TRAY=OFF \
    -DSUNSHINE_ENABLE_RKMPP=ON \
    -DSUNSHINE_SYSTEM_VULKAN_HEADERS=OFF \
    -DSUNSHINE_SYSTEM_WAYLAND_PROTOCOLS=OFF

  run "$brew_prefix/bin/cmake" --build "$BUILD_DIR" --parallel "$JOBS"
  [[ -x "$BUILD_DIR/sunshine" ]] || die "build completed without $BUILD_DIR/sunshine."
  printf '\nBuilt: %s\n' "$BUILD_DIR/sunshine"
}

main "$@"
