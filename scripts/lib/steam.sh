# Shared Steam library / ARK / Proton discovery for Linux scripts.
# Extracted from launch-proton.sh so install-linux-proxy.sh doesn't
# reimplement the same "find ARK, find its Proton, find its prefix" logic.
set -uo pipefail

kopt_steam_roots() {
  if [[ -n "${KOPT_STEAM_ROOT:-}" ]]; then
    echo "$KOPT_STEAM_ROOT"
    return
  fi
  local candidates=(
    "$HOME/.local/share/Steam"
    "$HOME/.steam/steam"
    "$HOME/.steam/root"
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
    "$HOME/snap/steam/common/.local/share/Steam"
  )
  local c
  for c in "${candidates[@]}"; do
    [[ -d "$c/steamapps" ]] && echo "$c"
  done
}

kopt_library_paths() {
  local root="$1"
  local vdf="$root/steamapps/libraryfolders.vdf"
  echo "$root"
  if [[ -f "$vdf" ]]; then
    grep -oE '"path"[[:space:]]*"[^"]+"' "$vdf" | sed -E 's/.*"path"[[:space:]]*"([^"]+)".*/\1/'
  fi
}

kopt_find_game_dir() {
  local lib="$1"
  find "$lib/steamapps/common" -maxdepth 1 -iname 'ARK*' -type d 2>/dev/null | while read -r dir; do
    [[ -f "$dir/ShooterGame/Binaries/Win64/ShooterGame.exe" ]] && echo "$dir"
  done | head -n1
}

kopt_find_appid() {
  local lib="$1" install_dir_name="$2"
  grep -lE "\"installdir\"[[:space:]]*\"${install_dir_name}\"" "$lib"/steamapps/appmanifest_*.acf 2>/dev/null \
    | head -n1 \
    | sed -E 's/.*appmanifest_([0-9]+)\.acf/\1/'
}

kopt_find_proton() {
  local lib="$1"
  local exp="$lib/steamapps/common/Proton - Experimental/proton"
  [[ -f "$exp" ]] && { echo "$exp"; return; }
  find "$lib/steamapps/common" -maxdepth 1 -iname 'Proton *' -type d 2>/dev/null \
    | sort -V | tail -n1 | while read -r dir; do
      [[ -f "$dir/proton" ]] && echo "$dir/proton"
    done
}

# The authoritative "real" version.dll doesn't reliably live in a per-game
# prefix's own system32 -- Wine only puts it there if something already
# copied it in; a prefix can run the game fine on Wine's internal builtin
# with no file on disk at all (confirmed on a real prefix: deleting it
# didn't make Wine recreate it, and the game kept running). The Proton
# installation itself always ships it as a real file. Matches
# ark_fun_tools' internal/install/deploy.protonSystemDLL.
kopt_find_proton_real_version_dll() {
  local lib="$1"
  find "$lib/steamapps/common" -iname 'Proton*' -maxdepth 1 -type d 2>/dev/null | while read -r dir; do
    local candidate="$dir/files/lib/wine/x86_64-windows/version.dll"
    [[ -f "$candidate" ]] && stat -c '%Y %n' "$candidate" 2>/dev/null
  done | sort -rn | head -n1 | cut -d' ' -f2-
}

# Populates KOPT_FOUND_LIB, KOPT_FOUND_GAME_DIR, KOPT_FOUND_APPID,
# KOPT_FOUND_COMPATDATA, KOPT_FOUND_PROTON for the first Steam
# library/install that has ARK. Returns 1 if nothing was found.
kopt_locate_ark() {
  KOPT_FOUND_LIB="" KOPT_FOUND_GAME_DIR="" KOPT_FOUND_APPID="" \
    KOPT_FOUND_COMPATDATA="" KOPT_FOUND_PROTON="" KOPT_FOUND_PROTON_REAL_VERSION_DLL=""
  while IFS= read -r root; do
    while IFS= read -r lib; do
      [[ -d "$lib/steamapps" ]] || continue
      local game_dir appid
      game_dir="$(kopt_find_game_dir "$lib")"
      [[ -n "$game_dir" ]] || continue
      appid="$(kopt_find_appid "$lib" "$(basename "$game_dir")")"
      [[ -n "$appid" ]] || continue
      KOPT_FOUND_LIB="$lib"
      KOPT_FOUND_GAME_DIR="$game_dir"
      KOPT_FOUND_APPID="$appid"
      KOPT_FOUND_COMPATDATA="$lib/steamapps/compatdata/$appid"
      KOPT_FOUND_PROTON="$(kopt_find_proton "$lib")"
      KOPT_FOUND_PROTON_REAL_VERSION_DLL="$(kopt_find_proton_real_version_dll "$lib")"
      return 0
    done < <(kopt_library_paths "$root")
  done < <(kopt_steam_roots)
  return 1
}
