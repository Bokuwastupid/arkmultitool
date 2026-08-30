#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${root_dir}/build-mingw"

cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${root_dir}/cmake/mingw64-toolchain.cmake"
cmake --build "${build_dir}" --parallel

echo "Built PE64 artifacts in ${build_dir}/dist"
