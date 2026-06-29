#!/bin/sh
# pack.sh — build the signed lumen herald system package (lumen.hpkg).
#
# A .hpkg is a manifest-first uncompressed POSIX ustar + a detached
# ECDSA-P256/SHA-256 signature. lumen is class=system (it needs the POWER cap),
# so herald installs its whole payload tree verbatim. Payload:
#   bin/lumen                       the compositor (stripped)
#   etc/aegis/caps.d/lumen          its cap policy (service FB THREAD_CREATE PROC_READ POWER)
#   usr/share/fonts/*.ttf           toolkit fonts (every GUI app needs these)
#   usr/share/{logo,claude}.raw     desktop logo assets
set -eu
cd "$(dirname "$0")/.."

VER="$(cat VERSION)"
KEY="${HERALD_KEY:-}"
STRIP="${STRIP:-strip}"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/bin" "$STAGE/etc/aegis/caps.d" "$STAGE/usr/share/fonts"

if ! "$STRIP" -o "$STAGE/bin/lumen" lumen.elf 2>/dev/null; then
    cp lumen.elf "$STAGE/bin/lumen"
fi
chmod 0755 "$STAGE/bin/lumen"
cp pkg/caps.d/lumen "$STAGE/etc/aegis/caps.d/lumen"
cp assets/*.ttf "$STAGE/usr/share/fonts/"
for raw in logo claude; do
    [ -f "assets/$raw.raw" ] && cp "assets/$raw.raw" "$STAGE/usr/share/$raw.raw"
done
printf 'id=lumen\nname=Lumen Compositor\nversion=%s\nclass=system\n' "$VER" > "$STAGE/manifest"

# manifest first so herald reads it without scanning; uncompressed ustar.
tar --format=ustar -C "$STAGE" -cf lumen.hpkg manifest bin etc usr
if [ -n "$KEY" ]; then openssl dgst -sha256 -sign "$KEY" -out lumen.hpkg.sig lumen.hpkg; else rm -f lumen.hpkg.sig; echo "[lumen] unsigned (no HERALD_KEY set)"; fi
echo "[lumen] lumen.hpkg $VER ($(wc -c < lumen.hpkg) bytes) + .sig"
