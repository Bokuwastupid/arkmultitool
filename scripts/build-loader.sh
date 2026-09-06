#!/usr/bin/env bash
# Linux-native equivalent of scripts/build-loader.ps1 (which still is the
# canonical path on Windows). Publishes the Avalonia loader without
# requiring PowerShell, using only `dotnet`, which is genuinely
# cross-platform.
#
# Usage: scripts/build-loader.sh [--runtime linux-x64|win-x64] [--configuration Release|Debug] [--install]
#
#   --runtime win-x64  cross-publishes the Windows bundle from Linux and
#                       bundles the mingw-built native payload/injector
#                       from build-mingw/dist (run scripts/build-linux.sh
#                       first). This does NOT run validate-release.ps1 --
#                       that script does PE/RVA analysis of the compiled
#                       Windows binary and only runs under pwsh; run it on
#                       Windows (or under `pwsh` if installed here) before
#                       shipping a win-x64 bundle built this way.
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/deps.sh
source "${root_dir}/scripts/lib/deps.sh"

runtime="linux-x64"
configuration="Release"
do_install=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --runtime) shift; runtime="$1" ;;
    --configuration) shift; configuration="$1" ;;
    --install) do_install=1 ;;
    *) echo "Unknown argument: $1" >&2; exit 64 ;;
  esac
  shift
done

if [[ "$runtime" != "linux-x64" && "$runtime" != "win-x64" ]]; then
  echo "Unsupported --runtime '$runtime' (expected linux-x64 or win-x64)" >&2
  exit 64
fi

kopt_ensure_dotnet 10

project="${root_dir}/loader/Kopt.Loader/Kopt.Loader.csproj"
output="${root_dir}/loader/dist/${runtime}"
native_dist="${root_dir}/build-mingw/dist"
payload="${native_dist}/kopt_payload.dll"
injector="${native_dist}/kopt_injector.exe"
version_proxy="${native_dist}/version.dll"

if [[ "$runtime" == "win-x64" ]]; then
  if [[ ! -f "$payload" || ! -f "$injector" ]]; then
    echo "win-x64 bundle needs native artifacts in ${native_dist}." >&2
    echo "Run scripts/build-linux.sh first (or --install here won't help; it's a different toolchain)." >&2
    if [[ "$do_install" -eq 1 ]]; then
      : # nothing to auto-install for a missing build step, not a missing tool
    fi
    exit 3
  fi
fi
if [[ "$runtime" == "linux-x64" && ( ! -f "$payload" || ! -f "$version_proxy" ) ]]; then
  echo "[build-loader] NOTE: version.dll/kopt_payload.dll not found in ${native_dist} yet." >&2
  echo "[build-loader] Run scripts/build-linux.sh first if you want the dashboard's Linux" >&2
  echo "[build-loader] Launch/Inject button (installs the Proton auto-load proxy) to work." >&2
fi

dotnet publish "$project" --configuration "$configuration" --runtime "$runtime" --self-contained false \
  --output "$output"

if [[ "$runtime" == "win-x64" ]]; then
  cp -f "$payload" "${output}/kopt_payload_candidate.dll"
  cp -f "$injector" "${output}/kopt_injector.exe"
elif [[ -f "$payload" && -f "$version_proxy" ]]; then
  cp -f "$payload" "${output}/kopt_payload.dll"
  cp -f "$version_proxy" "${output}/version.dll"
fi

manifest="${output}/release-manifest.local.json"
{
  echo '{'
  echo '  "schema": 1,'
  echo "  \"runtime\": \"${runtime}\","
  echo "  \"configuration\": \"${configuration}\","
  echo "  \"generatedAt\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
  echo '  "signed": false,'
  echo '  "channel": "local-development",'
  echo '  "artifacts": ['
  first=1
  while IFS= read -r -d '' file; do
    name="$(basename "$file")"
    size="$(stat -c%s "$file")"
    sha256="$(sha256sum "$file" | awk '{print $1}')"
    [[ "$first" -eq 0 ]] && echo ','
    first=0
    printf '    { "name": "%s", "size": %s, "sha256": "%s" }' "$name" "$size" "$sha256"
  done < <(find "$output" -maxdepth 1 -type f -not -name 'release-manifest.local.json' -print0 | sort -z)
  echo
  echo '  ]'
  echo '}'
} > "$manifest"

if [[ "$runtime" == "linux-x64" ]]; then
  dotnet "${output}/Kopt.Loader.dll" --self-test
else
  echo "[KOPT] Skipping self-test: win-x64 native assets can't run under Linux's dotnet host." >&2
  echo "[KOPT] Self-test this bundle on Windows (or under Wine/Proton) before shipping." >&2
fi

echo "[KOPT] Loader bundle built: ${output}"
