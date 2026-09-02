#!/usr/bin/env bash
# Installs the version.dll auto-load proxy into the Wine prefix's own
# system32 -- never into the Steam-tracked game directory. Dropping files
# there once made Steam's integrity check decide the depot had changed and
# queue a ~50k-chunk "update"; the prefix is unmanaged user data Steam's
# verifier never inspects. Matches ark_fun_tools' internal/platform/prefix
# approach (same version.dll target, same DllOverrides registry line) --
# keep the two in sync if that mechanism changes.
#
# See src/version_proxy.cpp / src/version.def for how the proxy itself
# works.
#
# Usage:
#   scripts/install-linux-proxy.sh              install (or update) the proxy
#   scripts/install-linux-proxy.sh --uninstall   restore the vanilla prefix
#   scripts/install-linux-proxy.sh --status      show what's currently installed
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/steam.sh
source "${root_dir}/scripts/lib/steam.sh"

dist_dir="${KOPT_DIST_DIR:-${root_dir}/build-mingw/dist}"
mode="install"
case "${1:-}" in
  --uninstall) mode="uninstall" ;;
  --status) mode="status" ;;
  "") ;;
  *) echo "Usage: $0 [--uninstall|--status]" >&2; exit 64 ;;
esac

if ! kopt_locate_ark; then
  echo "Could not find an ARK: Survival Evolved install in any Steam library." >&2
  echo "Set KOPT_STEAM_ROOT if Steam lives somewhere nonstandard." >&2
  exit 2
fi

system32="${KOPT_FOUND_COMPATDATA}/pfx/drive_c/windows/system32"
user_reg="${KOPT_FOUND_COMPATDATA}/pfx/user.reg"
target_version="${system32}/version.dll"
target_orig="${system32}/version_orig.dll"
target_payload="${system32}/kopt_payload.dll"
override_line='"version"="native,builtin"'
overrides_section='\[Software\\\\Wine\\\\DllOverrides\]'

echo "[install-linux-proxy] ARK: ${KOPT_FOUND_GAME_DIR} (appid ${KOPT_FOUND_APPID})" >&2
echo "[install-linux-proxy] Wine system32: ${system32}" >&2

if [[ "$mode" == "status" ]]; then
  if [[ -f "$target_version" && -f "$target_orig" && -f "$target_payload" ]] && grep -qF "$override_line" "$user_reg" 2>/dev/null; then
    echo "Installed: version.dll, version_orig.dll, kopt_payload.dll present; registry override set."
  else
    echo "Not installed (or partially installed):"
    for f in "$target_version" "$target_orig" "$target_payload"; do
      [[ -f "$f" ]] && echo "  present:  $f" || echo "  MISSING:  $f"
    done
    grep -qF "$override_line" "$user_reg" 2>/dev/null \
      && echo "  present:  registry override in user.reg" \
      || echo "  MISSING:  registry override in user.reg"
  fi
  exit 0
fi

if [[ "$mode" == "uninstall" ]]; then
  rm -f "$target_payload"
  if [[ -f "$target_orig" ]]; then
    mv -f "$target_orig" "$target_version"
    echo "[install-linux-proxy] Restored the real version.dll." >&2
  fi
  if [[ -f "$user_reg" ]] && grep -qF "$override_line" "$user_reg"; then
    grep -vF "$override_line" "$user_reg" > "${user_reg}.kopt-tmp"
    mv -f "${user_reg}.kopt-tmp" "$user_reg"
    echo "[install-linux-proxy] Removed the DllOverrides registry line." >&2
  fi
  echo "[install-linux-proxy] Uninstalled. Restart ARK for it to take effect." >&2
  exit 0
fi

# install
if [[ ! -f "$dist_dir/version.dll" || ! -f "$dist_dir/kopt_payload.dll" ]]; then
  echo "Missing build/dist/{version.dll,kopt_payload.dll} in ${dist_dir}." >&2
  echo "Run scripts/build-linux.sh first." >&2
  exit 3
fi
if [[ ! -d "$system32" ]]; then
  echo "No system32 in the Wine prefix (${system32})." >&2
  echo "Run ARK at least once first so Proton finishes setting it up." >&2
  exit 4
fi
if [[ ! -f "$user_reg" ]]; then
  echo "No user.reg in the Wine prefix (${user_reg})." >&2
  exit 4
fi

if pgrep -x ShooterGame.exe >/dev/null 2>&1; then
  echo "[install-linux-proxy] WARNING: ShooterGame.exe is currently running." >&2
  echo "[install-linux-proxy] The proxy only loads at process start, and Wine rewrites" >&2
  echo "[install-linux-proxy] user.reg wholesale on exit -- restart ARK after this finishes." >&2
fi

if [[ -f "$target_orig" ]]; then
  echo "[install-linux-proxy] version_orig.dll already present, leaving it (already installed once)." >&2
elif [[ -f "$target_version" ]]; then
  cp -n "$target_version" "$target_orig"
  echo "[install-linux-proxy] Saved real version.dll -> version_orig.dll (from the prefix)" >&2
elif [[ -n "$KOPT_FOUND_PROTON_REAL_VERSION_DLL" ]]; then
  cp -n "$KOPT_FOUND_PROTON_REAL_VERSION_DLL" "$target_orig"
  echo "[install-linux-proxy] Saved real version.dll -> version_orig.dll (from Proton's own files, none was in the prefix)" >&2
else
  echo "Real version.dll not found in the prefix (${target_version}) or in any installed Proton's" >&2
  echo "files/lib/wine/x86_64-windows/version.dll." >&2
  exit 4
fi

# Files Proton stages into system32 can come out without a write bit
# (confirmed: a real prefix's version.dll was -r-xr-xr-x) -- chmod before
# overwrite so this doesn't fail with "Permission denied" on a second install.
chmod -f u+w "$target_version" "$target_payload" 2>/dev/null || true
cp -f "$dist_dir/version.dll" "$target_version"
cp -f "$dist_dir/kopt_payload.dll" "$target_payload"

if grep -qF "$override_line" "$user_reg"; then
  echo "[install-linux-proxy] Registry override already set." >&2
else
  if ! grep -qE "$overrides_section" "$user_reg"; then
    echo "No ${overrides_section} section in ${user_reg}." >&2
    exit 5
  fi
  # Wine writes the section header with a trailing timestamp on the same
  # line -- "[Software\\Wine\\DllOverrides] 1785139151" -- so this has to
  # be a prefix match, not equality against the bare header (that silently
  # never matched, which is exactly how this shipped broken the first
  # time: the script printed "Added" unconditionally without checking).
  # Order within the section doesn't matter to Wine's registry parser --
  # just needs to land somewhere between this header and the next `[`.
  #
  # The section string needs FOUR backslashes here, not two: awk's -v
  # assignment applies the same backslash-escape processing a string
  # literal in the program text would get, so "\\\\" is what survives as
  # two literal backslashes -- matching what's actually in the file. Two
  # backslashes in the -v value collapse to one and never match (confirmed
  # by hand: that was the actual first-round bug, not a typo risk).
  if ! awk -v section='[Software\\\\Wine\\\\DllOverrides]' -v line="$override_line" '
    { print }
    !done && substr($0, 1, length(section)) == section { print line; done = 1 }
    END { if (!done) { print "no DllOverrides section header found" > "/dev/stderr"; exit 1 } }
  ' "$user_reg" > "${user_reg}.kopt-tmp"; then
    rm -f "${user_reg}.kopt-tmp"
    echo "[install-linux-proxy] Failed to find the DllOverrides section header to insert after -- registry NOT modified." >&2
    exit 6
  fi
  mv -f "${user_reg}.kopt-tmp" "$user_reg"
  echo "[install-linux-proxy] Added Wine DllOverrides registry entry." >&2
fi

echo "[install-linux-proxy] Installed version.dll (proxy) + kopt_payload.dll into ${system32}" >&2
echo "[install-linux-proxy] Steam-tracked game directory was not touched." >&2
echo "[install-linux-proxy] Launch ARK normally through Steam/Proton -- it self-loads. No script to run per session." >&2
