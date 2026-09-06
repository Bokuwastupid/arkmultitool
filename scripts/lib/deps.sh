# Shared Linux dependency detection for arkmultitool build scripts.
# Sourced by build-linux.sh and build-loader.sh so the "is X installed /
# how do I get it" logic lives in one place instead of drifting between
# two copies.
#
# Usage:
#   source "$(dirname "${BASH_SOURCE[0]}")/lib/deps.sh"
#   kopt_need cmd:cmake      pkg:apt=cmake      pkg:dnf=cmake      pkg:pacman=cmake      pkg:zypper=cmake      pkg:apk=cmake
#   kopt_need cmd:ninja      pkg:apt=ninja-build pkg:dnf=ninja-build pkg:pacman=ninja    pkg:zypper=ninja      pkg:apk=samurai
#   kopt_check_deps_or_exit [--install]

set -uo pipefail

KOPT_MISSING_CMDS=()
KOPT_MISSING_PKGS_APT=()
KOPT_MISSING_PKGS_DNF=()
KOPT_MISSING_PKGS_PACMAN=()
KOPT_MISSING_PKGS_ZYPPER=()
KOPT_MISSING_PKGS_APK=()

kopt_detect_distro_id() {
  if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    (. /etc/os-release && echo "${ID:-unknown}")
  else
    echo unknown
  fi
}

kopt_detect_pkg_manager() {
  local id
  id="$(kopt_detect_distro_id)"
  case "$id" in
    ubuntu|debian|pop|linuxmint|elementary|zorin|kali) echo apt ;;
    fedora|rhel|centos|rocky|almalinux|ol)             echo dnf ;;
    arch|manjaro|endeavouros|garuda)                   echo pacman ;;
    opensuse*|suse*|sles)                              echo zypper ;;
    alpine)                                             echo apk ;;
    nixos)                                              echo nix ;;
    *)
      if command -v apt-get >/dev/null 2>&1; then echo apt
      elif command -v dnf >/dev/null 2>&1; then echo dnf
      elif command -v pacman >/dev/null 2>&1; then echo pacman
      elif command -v zypper >/dev/null 2>&1; then echo zypper
      elif command -v apk >/dev/null 2>&1; then echo apk
      else echo unknown
      fi
      ;;
  esac
}

# kopt_need cmd:<binary> pkg:apt=<name> pkg:dnf=<name> pkg:pacman=<name> pkg:zypper=<name> pkg:apk=<name>
# Any pkg:<mgr>= arg may be omitted if the package name differs and is unknown for that manager.
kopt_need() {
  local bin="" apt="" dnf="" pacman="" zypper="" apk=""
  local arg
  for arg in "$@"; do
    case "$arg" in
      cmd:*)     bin="${arg#cmd:}" ;;
      pkg:apt=*) apt="${arg#pkg:apt=}" ;;
      pkg:dnf=*) dnf="${arg#pkg:dnf=}" ;;
      pkg:pacman=*) pacman="${arg#pkg:pacman=}" ;;
      pkg:zypper=*) zypper="${arg#pkg:zypper=}" ;;
      pkg:apk=*) apk="${arg#pkg:apk=}" ;;
    esac
  done
  if command -v "$bin" >/dev/null 2>&1; then
    return 0
  fi
  KOPT_MISSING_CMDS+=("$bin")
  [[ -n "$apt" ]] && KOPT_MISSING_PKGS_APT+=("$apt")
  [[ -n "$dnf" ]] && KOPT_MISSING_PKGS_DNF+=("$dnf")
  [[ -n "$pacman" ]] && KOPT_MISSING_PKGS_PACMAN+=("$pacman")
  [[ -n "$zypper" ]] && KOPT_MISSING_PKGS_ZYPPER+=("$zypper")
  [[ -n "$apk" ]] && KOPT_MISSING_PKGS_APK+=("$apk")
  return 1
}

# Call after every kopt_need call has run. Exits 2 with a per-distro
# install command if anything is missing; with --install, attempts the
# install itself via sudo for the detected package manager first.
kopt_check_deps_or_exit() {
  local do_install=0
  [[ "${1:-}" == "--install" ]] && do_install=1

  if [[ "${#KOPT_MISSING_CMDS[@]}" -eq 0 ]]; then
    return 0
  fi

  local mgr
  mgr="$(kopt_detect_pkg_manager)"
  echo "Missing required tools: ${KOPT_MISSING_CMDS[*]}" >&2

  local install_cmd=""
  case "$mgr" in
    apt)
      [[ "${#KOPT_MISSING_PKGS_APT[@]}" -gt 0 ]] && install_cmd="sudo apt-get update && sudo apt-get install -y ${KOPT_MISSING_PKGS_APT[*]}" ;;
    dnf)
      [[ "${#KOPT_MISSING_PKGS_DNF[@]}" -gt 0 ]] && install_cmd="sudo dnf install -y ${KOPT_MISSING_PKGS_DNF[*]}" ;;
    pacman)
      [[ "${#KOPT_MISSING_PKGS_PACMAN[@]}" -gt 0 ]] && install_cmd="sudo pacman -S --needed ${KOPT_MISSING_PKGS_PACMAN[*]}" ;;
    zypper)
      [[ "${#KOPT_MISSING_PKGS_ZYPPER[@]}" -gt 0 ]] && install_cmd="sudo zypper install -y ${KOPT_MISSING_PKGS_ZYPPER[*]}" ;;
    apk)
      [[ "${#KOPT_MISSING_PKGS_APK[@]}" -gt 0 ]] && install_cmd="sudo apk add ${KOPT_MISSING_PKGS_APK[*]}" ;;
    nix)
      echo "  NixOS/nix profile detected: nix profile install nixpkgs#cmake nixpkgs#ninja nixpkgs#pkgsCross.mingwW64.buildPackages.gcc" >&2
      ;;
    *)
      echo "  Unrecognized package manager; see docs/LINUX_BUILD.md for the full per-distro table." >&2
      ;;
  esac

  if [[ -n "$install_cmd" ]]; then
    if [[ "$do_install" -eq 1 ]]; then
      echo "[deps] Installing via: $install_cmd" >&2
      eval "$install_cmd"
      # Re-verify: an eval'd install can partially fail without a nonzero
      # exit (e.g. one bad package name in the list), so recheck commands
      # rather than trusting the install's exit code alone.
      local still_missing=0
      local c
      for c in "${KOPT_MISSING_CMDS[@]}"; do
        command -v "$c" >/dev/null 2>&1 || still_missing=1
      done
      if [[ "$still_missing" -eq 0 ]]; then
        return 0
      fi
      echo "[deps] Install ran but some tools are still missing; see docs/LINUX_BUILD.md." >&2
      exit 2
    else
      echo "  Install with:" >&2
      echo "    $install_cmd" >&2
      echo "  (or re-run this script with --install to do it automatically)" >&2
    fi
  fi

  echo "  Full per-distro package table: docs/LINUX_BUILD.md" >&2
  exit 2
}

# .NET SDK package names/availability for net10.0 vary and lag across
# distro repos (Debian stable, Alpine, older Ubuntu LTS in particular).
# Microsoft's dotnet-install.sh is the one method that works identically
# on every distro, so it is the primary path; the distro package is only
# offered as a secondary hint.
kopt_ensure_dotnet() {
  local required_major="${1:-10}"
  if command -v dotnet >/dev/null 2>&1; then
    local installed_major
    installed_major="$(dotnet --list-sdks 2>/dev/null | awk -F. '{print $1}' | sort -u | tail -n1)"
    if [[ -n "$installed_major" && "$installed_major" -ge "$required_major" ]]; then
      return 0
    fi
  fi
  echo "Missing .NET ${required_major}.0+ SDK." >&2
  echo "  Recommended (works on every distro, installs to \$HOME/.dotnet, no root needed):" >&2
  echo "    curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel ${required_major}.0" >&2
  echo "    export PATH=\"\$HOME/.dotnet:\$PATH\"" >&2
  local mgr
  mgr="$(kopt_detect_pkg_manager)"
  case "$mgr" in
    apt)    echo "  Distro package (may lag behind ${required_major}.0): sudo apt-get install -y dotnet-sdk-${required_major}.0" >&2 ;;
    dnf)    echo "  Distro package (may lag behind ${required_major}.0): sudo dnf install -y dotnet-sdk-${required_major}.0" >&2 ;;
    pacman) echo "  Distro package (may lag behind ${required_major}.0): sudo pacman -S --needed dotnet-sdk" >&2 ;;
    zypper) echo "  Distro package (may lag behind ${required_major}.0): sudo zypper install -y dotnet-sdk-${required_major}" >&2 ;;
    apk)    echo "  Distro package (may lag behind ${required_major}.0): sudo apk add dotnet${required_major}-sdk" >&2 ;;
    nix)    echo "  nix profile install nixpkgs#dotnet-sdk_${required_major}" >&2 ;;
  esac
  echo "  Full table: docs/LINUX_BUILD.md" >&2
  exit 2
}
