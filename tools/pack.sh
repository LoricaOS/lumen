#!/bin/sh
# pack.sh — build the signed lumen herald system package (lumen$SUF.hpkg).
#
# A .hpkg is a manifest-first uncompressed POSIX ustar + a detached
# ECDSA-P256/SHA-256 signature. lumen is class=system (it needs the POWER cap),
# so herald installs its whole payload tree verbatim. Payload:
#   bin/lumen                       the compositor (stripped)
#   etc/aegis/caps.d/lumen          its cap policy (service FB THREAD_CREATE PROC_READ POWER)
#   usr/share/fonts/*.ttf           toolkit fonts (every GUI app needs these)
#   usr/share/{logo,claude,wallpaper}.raw  logo + desktop wallpaper
set -eu
cd "$(dirname "$0")/.."

VER="$(cat VERSION)"
KEY="${HERALD_KEY:-}"
STRIP="${STRIP:-strip}"
ARCH="${ARCH:-x86_64}"                    # target arch (arm64 → arch=arm64 + -arm64.hpkg)
SUF=""; [ "$ARCH" = x86_64 ] || SUF="-$ARCH"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/bin" "$STAGE/etc/aegis/caps.d" "$STAGE/usr/share/fonts"

if ! "$STRIP" -o "$STAGE/bin/lumen" lumen.elf 2>/dev/null; then
    cp lumen.elf "$STAGE/bin/lumen"
fi
chmod 0755 "$STAGE/bin/lumen"
cp pkg/caps.d/lumen "$STAGE/etc/aegis/caps.d/lumen"
cp assets/*.ttf "$STAGE/usr/share/fonts/"
for raw in logo claude wallpaper; do
    [ -f "assets/$raw.raw" ] && cp "assets/$raw.raw" "$STAGE/usr/share/$raw.raw"
done
printf 'id=lumen\nname=Lumen Compositor\nversion=%s\nclass=system\narch=%s\n' "$VER" "$ARCH" > "$STAGE/manifest"

# manifest first so herald reads it without scanning; uncompressed ustar.
tar --format=ustar -C "$STAGE" -cf lumen$SUF.hpkg manifest bin etc usr
if [ -n "$KEY" ]; then openssl dgst -sha256 -sign "$KEY" -out lumen$SUF.hpkg.sig lumen$SUF.hpkg; else rm -f lumen$SUF.hpkg.sig; echo "[lumen] unsigned (no HERALD_KEY set)"; fi
echo "[lumen] lumen$SUF.hpkg $VER ($(wc -c < lumen$SUF.hpkg) bytes) + .sig"
