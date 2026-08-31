#!/usr/bin/env bash
# One command to actually open the loader window -- wires up the native
# lib paths Avalonia/Skia/X11 need on NixOS (see docs/LINUX_BUILD.md) so
# nobody has to remember LD_LIBRARY_PATH by hand every time.
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dll="${root_dir}/loader/dist/linux-x64/Kopt.Loader.dll"

if [[ ! -f "$dll" ]]; then
  echo "Loader not built yet. Run: scripts/build-loader.sh --runtime linux-x64" >&2
  exit 1
fi

export KOPT_API_URL="${KOPT_API_URL:-http://127.0.0.1:5087}"
export LD_LIBRARY_PATH="${HOME}/.nix-profile/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

exec dotnet "$dll" "$@"
