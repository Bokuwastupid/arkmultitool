#!/usr/bin/env bash
# Runs the injector against ShooterGame.exe through Proton -- the manual
# attach path for a game that's already running (needs root to cross the
# Steam Linux Runtime sandbox; see docs/LINUX_BUILD.md). For normal use,
# scripts/install-linux-proxy.sh is the path that needs neither root nor a
# manual run every launch.
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/steam.sh
source "${root_dir}/scripts/lib/steam.sh"

dist_dir="${KOPT_DIST_DIR:-${root_dir}/build-mingw/dist}"
injector="${dist_dir}/kopt_injector.exe"
payload="${dist_dir}/kopt_payload.dll"

if [[ -z "${PROTON:-}" || -z "${STEAM_COMPAT_DATA_PATH:-}" ]]; then
  echo "[launch-proton] PROTON/STEAM_COMPAT_DATA_PATH not set, auto-detecting..." >&2
  if kopt_locate_ark; then
    : "${PROTON:="$KOPT_FOUND_PROTON"}"
    : "${STEAM_COMPAT_DATA_PATH:="$KOPT_FOUND_COMPATDATA"}"
  fi
fi

if [[ -z "${PROTON:-}" ]]; then
  echo "Could not auto-detect Proton. Set it explicitly, for example:" >&2
  echo "  export PROTON=\"\$HOME/.local/share/Steam/steamapps/common/Proton - Experimental/proton\"" >&2
  exit 2
fi
if [[ -z "${STEAM_COMPAT_DATA_PATH:-}" ]]; then
  echo "Could not auto-detect ARK's compatdata prefix. Set it explicitly, for example:" >&2
  echo "  export STEAM_COMPAT_DATA_PATH=\"\$HOME/.local/share/Steam/steamapps/compatdata/346110\"" >&2
  exit 2
fi
if [[ ! -f "${injector}" || ! -f "${payload}" ]]; then
  echo "Build artifacts were not found in ${dist_dir}. Run scripts/build-linux.sh first." >&2
  exit 3
fi

echo "[launch-proton] PROTON=${PROTON}" >&2
echo "[launch-proton] STEAM_COMPAT_DATA_PATH=${STEAM_COMPAT_DATA_PATH}" >&2
echo "[launch-proton] NOTE: this must run inside the same sandbox namespace as" >&2
echo "[launch-proton] ShooterGame.exe (Steam Linux Runtime); if it's already running," >&2
echo "[launch-proton] this needs 'nsenter -t <pid> -m -p' as root first, or use" >&2
echo "[launch-proton] scripts/install-linux-proxy.sh instead so it self-loads at launch." >&2

"${PROTON}" run "${injector}" --wait --timeout 300 --process ShooterGame.exe --dll "${payload}"
