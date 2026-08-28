#!/usr/bin/env bash
# Build Sunshine with the RKMPP backend on Debian 12 ARM64 / ROCK 5B+.
set -Eeuo pipefail

readonly PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly LLVM_VERSION=22.1.6
readonly LLVM_DIR="$PROJECT_DIR/build/toolchains/LLVM-$LLVM_VERSION-Linux-ARM64"

BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build-rkmpp-review}"
JOBS="${JOBS:-4}"
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

main() {
  [[ "$(uname -s)" == Linux && "$(uname -m)" == aarch64 ]] || die "this script supports Linux ARM64 only."

  command -v "$BREW_BIN" >/dev/null 2>&1 || die "Homebrew was not found."
  local brew_prefix
  brew_prefix="$("$BREW_BIN" --prefix)"
  export PATH="$brew_prefix/bin:$PATH"

  [[ -x "$brew_prefix/bin/cmake" ]] || die "Homebrew CMake is missing."
  [[ -x "$brew_prefix/bin/ninja" ]] || die "Homebrew Ninja is missing."
  [[ -x "$brew_prefix/bin/npm" ]] || die "Homebrew npm is missing."

  [[ -x "$LLVM_DIR/bin/clang" ]] || die "LLVM is missing. Did you run setup-env-rkmpp.sh?"
  [[ -d "$BUILD_DIR/_deps/ffmpeg" ]] || die "FFmpeg is missing. Did you run setup-env-rkmpp.sh?"

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
    -DBUILD_TESTS=OFF \
    -DBUILD_TESTING=OFF \
    -DSUNSHINE_ENABLE_CUDA=OFF \
    -DSUNSHINE_ENABLE_TRAY=OFF \
    -DSUNSHINE_ENABLE_RKMPP=ON \
    -DSUNSHINE_SYSTEM_VULKAN_HEADERS=OFF \
    -DSUNSHINE_SYSTEM_WAYLAND_PROTOCOLS=OFF \
    -DSUNSHINE_ASSETS_DIR_DEF="$BUILD_DIR/assets"

  run "$brew_prefix/bin/cmake" --build "$BUILD_DIR" --parallel "$JOBS"
  [[ -x "$BUILD_DIR/sunshine" ]] || die "build completed without $BUILD_DIR/sunshine."
  printf "\nBuilt: %s\n" "$BUILD_DIR/sunshine"
}

main "$@"
