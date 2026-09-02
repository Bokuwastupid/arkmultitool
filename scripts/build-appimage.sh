#!/usr/bin/env bash
# Wraps the linux-x64 Kopt.Loader publish output into a portable AppImage.
#
# AppImage is a Linux-only distribution format (a squashfs-backed ELF that
# self-mounts via FUSE); it cannot run on Windows under any circumstances.
# Windows users get the separate win-x64 bundle from build-loader.sh/.ps1 --
# this script only produces the Linux artifact.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PUBLISH_DIR="${1:-$ROOT_DIR/loader/dist/linux-x64}"
OUT_DIR="$ROOT_DIR/loader/dist/appimage"
APPDIR="$OUT_DIR/Kopt.Loader.AppDir"
APP_VERSION="${KOPT_LOADER_VERSION:-0.0.0-dev}"
ARCH="x86_64"

if [[ ! -x "$PUBLISH_DIR/Kopt.Loader" ]]; then
  echo "error: $PUBLISH_DIR/Kopt.Loader not found or not executable." >&2
  echo "       run ./scripts/build-loader.sh --runtime linux-x64 first." >&2
  exit 1
fi

command -v appimagetool >/dev/null 2>&1 || {
  echo "error: appimagetool not found on PATH." >&2
  echo "       download: https://github.com/AppImage/appimagetool/releases" >&2
  exit 1
}

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

# Full self-contained .NET publish payload (dlls, runtimeconfig, native libs).
cp -a "$PUBLISH_DIR/." "$APPDIR/usr/bin/"
chmod +x "$APPDIR/usr/bin/Kopt.Loader"

ICON_PNG="$ROOT_DIR/loader/Kopt.Loader/Assets/icon.png"
ICON_ICO="$ROOT_DIR/loader/Kopt.Loader/Assets/avalonia-logo.ico"
ICON_DST="$APPDIR/usr/share/icons/hicolor/256x256/apps/kopt-loader.png"
if [[ -f "$ICON_PNG" ]]; then
  cp "$ICON_PNG" "$ICON_DST"
elif [[ -f "$ICON_ICO" ]] && command -v magick >/dev/null 2>&1; then
  # .ico holds multiple sizes; pick the largest frame and re-encode as PNG.
  # [0]: .ico holds multiple frames; magick without an index writes one file
  # per frame (icon-0.png, icon-1.png, ...) instead of a single output.
  magick "$ICON_ICO[0]" -resize 256x256 "$ICON_DST"
else
  echo "warning: no usable icon source ($ICON_PNG, $ICON_ICO); using a 1x1 placeholder." >&2
  base64 -d > "$ICON_DST" <<< \
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="
fi
ln -sf "usr/share/icons/hicolor/256x256/apps/kopt-loader.png" "$APPDIR/kopt-loader.png"

cat > "$APPDIR/usr/share/applications/kopt-loader.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=KOPT Loader
Exec=Kopt.Loader
Icon=kopt-loader
Categories=Game;Utility;
Terminal=false
EOF
ln -sf "usr/share/applications/kopt-loader.desktop" "$APPDIR/kopt-loader.desktop"

cat > "$APPDIR/AppRun" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
export DOTNET_ROOT="$HERE/usr/bin"
export LD_LIBRARY_PATH="$HERE/usr/bin:${LD_LIBRARY_PATH:-}"
exec "$HERE/usr/bin/Kopt.Loader" "$@"
EOF
chmod +x "$APPDIR/AppRun"

mkdir -p "$OUT_DIR"
VERSION="$APP_VERSION" ARCH="$ARCH" appimagetool "$APPDIR" "$OUT_DIR/Kopt.Loader-${APP_VERSION}-${ARCH}.AppImage"

echo "Built: $OUT_DIR/Kopt.Loader-${APP_VERSION}-${ARCH}.AppImage"
