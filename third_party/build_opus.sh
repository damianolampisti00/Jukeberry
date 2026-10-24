#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds libopus 1.5.2 (fixed point, plain C) as a static library for BB10
# armv7 with the BB10 NDK: third_party/opus/lib/armv7/libopus.a and
# third_party/opus/include. The source tarball is downloaded from Xiph and
# checked against the SHA-256 below (from Xiph's SHA256SUMS.txt).
#
# Usage:  bash third_party/build_opus.sh   (with the BB10 NDK environment set,
#         e.g. after sourcing bbndk-env_10_3_1_995.sh: qcc in PATH)
set -e
VER=1.5.2
SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
HERE=$(cd "$(dirname "$0")" && pwd)
command -v qcc >/dev/null || { echo "qcc not in PATH: set up the BB10 NDK environment first"; exit 1; }
WORK=${TMPDIR:-/tmp}/opus-build
SRC=$WORK/opus-$VER
OUT=$HERE/opus

mkdir -p "$WORK"
if [ ! -d "$SRC" ]; then
    curl -sSL -o "$WORK/opus.tar.gz" https://downloads.xiph.org/releases/opus/opus-$VER.tar.gz
    echo "$SHA256  $WORK/opus.tar.gz" | sha256sum -c -
    tar xzf "$WORK/opus.tar.gz" -C "$WORK"
fi

# file lists from the package's own makefile fragments (awk: Git Bash turns
# sed arguments starting with / into Windows paths)
list() {
    awk -v name="$1" '$1 == name && $2 == "=" { on = 1 }
        on { for (i = 1; i <= NF; i++) if ($i ~ /\.c$/) print $i; if ($NF != "\\") on = 0 }' "$SRC/$2"
}
FILES=$( (list CELT_SOURCES celt_sources.mk; list SILK_SOURCES silk_sources.mk;
          list SILK_SOURCES_FIXED silk_sources.mk; list OPUS_SOURCES opus_sources.mk) )

OBJ=$WORK/obj
rm -rf "$OBJ" "$OUT" && mkdir -p "$OBJ" "$OUT/lib/armv7" "$OUT/include/opus"
for f in $FILES; do
    qcc -Vgcc_ntoarmv7le -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp \
        -DOPUS_BUILD -DFIXED_POINT -DDISABLE_FLOAT_API -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF \
        -I"$SRC/include" -I"$SRC/celt" -I"$SRC/silk" -I"$SRC/silk/fixed" -I"$SRC" \
        -c "$SRC/$f" -o "$OBJ/$(echo "$f" | tr / _ | sed 's/\.c$/.o/')"
done
ntoarmv7-ar rcs "$OUT/lib/armv7/libopus.a" "$OBJ"/*.o
cp "$SRC"/include/*.h "$OUT/include/opus/"
cp "$SRC/COPYING" "$OUT/COPYING"
rm -rf "$OBJ"
echo "Built $OUT/lib/armv7/libopus.a ($(echo "$FILES" | wc -l) files)"
