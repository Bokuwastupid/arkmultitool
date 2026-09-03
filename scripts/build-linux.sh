#!/usr/bin/env bash
# Cross-compiles the native injector/payload (Win64 PE, required because
# ARK: Survival Evolved's ShooterGame.exe only exists as a Windows binary,
# run under Proton) from any Linux distro via mingw-w64.
#
# Usage: scripts/build-linux.sh [--install] [--configuration Release|Debug]
#   --install   auto-install missing build tools with sudo via the
#               detected distro package manager, instead of just printing
#               the command to run.
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/deps.sh
source "${root_dir}/scripts/lib/deps.sh"

# .env.sh -- local, not in git (.gitignore: .env.*); sets KOPT_RELAY_DEFAULT
# to a real relay address for local dev builds so it never has to be typed
# by hand or hardcoded into a tracked file (see .env.sh.example, and
# CMakeLists.txt's own comment on KOPT_RELAY_DEFAULT for why this matters).
if [[ -f "${root_dir}/.env.sh" ]]; then
  # shellcheck source=/dev/null
  source "${root_dir}/.env.sh"
fi

configuration="Release"
do_install=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --install) do_install=1 ;;
    --configuration) shift; configuration="$1" ;;
    *) echo "Unknown argument: $1" >&2; exit 64 ;;
  esac
  shift
done

kopt_need cmd:cmake  pkg:apt=cmake pkg:dnf=cmake pkg:pacman=cmake pkg:zypper=cmake pkg:apk=cmake || true
kopt_need cmd:ninja  pkg:apt=ninja-build pkg:dnf=ninja-build pkg:pacman=ninja pkg:zypper=ninja pkg:apk=samurai || true
kopt_need cmd:x86_64-w64-mingw32-g++ \
  pkg:apt=g++-mingw-w64-x86-64 pkg:dnf=mingw64-gcc-c++ pkg:pacman=mingw-w64-gcc \
  pkg:zypper=mingw64-cross-gcc-c++ pkg:apk=mingw-w64-gcc || true

if [[ "$do_install" -eq 1 ]]; then
  kopt_check_deps_or_exit --install
else
  kopt_check_deps_or_exit
fi

build_dir="${root_dir}/build-mingw"

# nixpkgs' pkgsCross.mingwW64 cross gcc uses the mcfgthread threading
# model, whose headers/import lib aren't on the compiler's default search
# path when installed standalone via `nix profile install` (only wired up
# automatically inside a full nix derivation build). CMakeLists.txt links
# it statically (`-static`) so the runtime DLL it'd otherwise need never
# has to exist under Wine -- but the compile step still needs to find the
# headers, and the link step the .a, to get that far at all. Distro mingw
# packages (apt/dnf/pacman/zypper/apk) use posix/win32 threading instead
# and simply won't have any of these paths -- find_mcfg_* returning empty
# there is correct, not a failure.
find_mcfg_dir() {
  local suffix="$1"
  find /nix/store -maxdepth 1 -iname "*mcfgthread-x86_64-w64-mingw32-*${suffix}" \
    -not -name "*.drv" 2>/dev/null | head -1
}
mcfg_runtime_dir="$(find_mcfg_dir '')"
mcfg_dev_dir="$(find_mcfg_dir '-dev')"
extra_cxx_flags=""
extra_linker_flags=""
if [[ -n "$mcfg_dev_dir" ]]; then
  extra_cxx_flags="-I${mcfg_dev_dir}/include"
fi
if [[ -n "$mcfg_runtime_dir" ]]; then
  extra_linker_flags="-L${mcfg_runtime_dir}/lib"
fi

relay_default_args=()
if [[ -n "${KOPT_RELAY_DEFAULT:-}" ]]; then
  relay_default_args=(-DKOPT_RELAY_DEFAULT="${KOPT_RELAY_DEFAULT}")
fi

cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${configuration}" \
  -DCMAKE_TOOLCHAIN_FILE="${root_dir}/cmake/mingw64-toolchain.cmake" \
  -DCMAKE_CXX_FLAGS="${extra_cxx_flags}" \
  -DCMAKE_EXE_LINKER_FLAGS="${extra_linker_flags}" \
  -DCMAKE_SHARED_LINKER_FLAGS="${extra_linker_flags}" \
  "${relay_default_args[@]}"
cmake --build "${build_dir}" --parallel

echo "Built PE64 artifacts in ${build_dir}/dist"
