#!/usr/bin/env bash
# Fetches ngtcp2 + ngtcp2_crypto_ossl + OpenSSL, cross-compiled for
# x86_64-w64-mingw32 by nixpkgs itself (pkgsCross.mingwW64.*) -- into
# third_party/quic-mingw64/. This is nghttp3-free on purpose: the Go relay
# side (backend/backend_go/internal/quicserver) is bare QUIC, not HTTP/3,
# so the client has no use for an HTTP/3 framing layer either.
#
# Not vendored into git: these are real binary blobs (~16MB with OpenSSL's
# libcrypto), and nixpkgs already gives a reproducible, pinned rebuild path
# -- re-running this script is the "vendoring", not a git commit. See
# .gitignore (third_party/quic-mingw64/).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$ROOT_DIR/third_party/quic-mingw64"

command -v nix >/dev/null 2>&1 || {
  echo "error: nix not found on PATH -- this script fetches prebuilt cross" \
       "packages from nixpkgs's binary cache, no local build required." >&2
  exit 1
}

echo "Resolving nixpkgs#pkgsCross.mingwW64.{openssl,ngtcp2} store paths..."
openssl_out="$(nix eval --raw 'nixpkgs#pkgsCross.mingwW64.openssl.out.outPath')"
openssl_dev="$(nix eval --raw 'nixpkgs#pkgsCross.mingwW64.openssl.dev.outPath')"
openssl_bin="$(nix eval --raw 'nixpkgs#pkgsCross.mingwW64.openssl.bin.outPath')"
ngtcp2_out="$(nix eval --raw 'nixpkgs#pkgsCross.mingwW64.ngtcp2.out.outPath')"
ngtcp2_dev="$(nix eval --raw 'nixpkgs#pkgsCross.mingwW64.ngtcp2.dev.outPath')"
# Header-only, platform-independent -- not cross-compiled, just the one
# amalgamated header. Used for both the outbound JSON building and parsing
# handshake/broadcast responses from ark_relay; writing a JSON parser by
# hand for the receive side wasn't worth it next to a widely-used,
# dependency-free single header.
nlohmann_json_out="$(nix eval --raw 'nixpkgs#nlohmann_json.outPath')"

echo "Building/fetching from binary cache (no source compilation expected)..."
nix build --no-link \
  "$openssl_out" "$openssl_dev" "$openssl_bin" \
  "$ngtcp2_out" "$ngtcp2_dev" \
  "$nlohmann_json_out"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/include" "$OUT_DIR/lib" "$OUT_DIR/bin"

cp -rL "$openssl_dev/include/openssl" "$OUT_DIR/include/"
cp -rL "$ngtcp2_dev/include/ngtcp2" "$OUT_DIR/include/"
mkdir -p "$OUT_DIR/include/nlohmann"
cp -rL "$nlohmann_json_out/include/nlohmann/"* "$OUT_DIR/include/nlohmann/"

cp -L "$openssl_out/lib/libssl.dll.a" "$OUT_DIR/lib/"
cp -L "$openssl_out/lib/libcrypto.dll.a" "$OUT_DIR/lib/"
cp -L "$ngtcp2_out/lib/libngtcp2.dll.a" "$OUT_DIR/lib/"
cp -L "$ngtcp2_out/lib/libngtcp2_crypto_ossl.dll.a" "$OUT_DIR/lib/"

# Runtime DLLs -- ngtcp2/openssl's mingw cross build is shared-library-only
# (no plain .a for ngtcp2 at all), unlike kopt_payload.dll's own -static
# link. These four ship alongside kopt_payload.dll in the loader bundle;
# Windows/Wine's DLL search order includes "the directory the calling
# module was loaded from", which covers a LoadLibraryW-loaded DLL like
# kopt_payload.dll itself.
cp -L "$openssl_bin/bin/libssl-3-x64.dll" "$OUT_DIR/bin/"
cp -L "$openssl_bin/bin/libcrypto-3-x64.dll" "$OUT_DIR/bin/"
cp -L "$ngtcp2_out/bin/libngtcp2.dll" "$OUT_DIR/bin/"
cp -L "$ngtcp2_out/bin/libngtcp2_crypto_ossl.dll" "$OUT_DIR/bin/"

echo "Done: $OUT_DIR"
du -sh "$OUT_DIR"
