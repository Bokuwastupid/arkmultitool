#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dist_dir="${KOPT_DIST_DIR:-${root_dir}/build-mingw/dist}"
injector="${dist_dir}/kopt_injector.exe"
payload="${dist_dir}/kopt_payload.dll"

if [[ -z "${PROTON:-}" ]]; then
  echo "Set PROTON to the Proton launcher path, for example:" >&2
  echo "  export PROTON=\"$HOME/.steam/steam/steamapps/common/Proton - Experimental/proton\"" >&2
  exit 2
fi
if [[ -z "${STEAM_COMPAT_DATA_PATH:-}" ]]; then
  echo "STEAM_COMPAT_DATA_PATH must point to ARK's compatdata prefix." >&2
  exit 2
fi
if [[ ! -f "${injector}" || ! -f "${payload}" ]]; then
  echo "Build artifacts were not found in ${dist_dir}. Run scripts/build-linux.sh first." >&2
  exit 3
fi

"${PROTON}" run "${injector}" --wait --timeout 300 --process ShooterGame.exe --dll "${payload}"
