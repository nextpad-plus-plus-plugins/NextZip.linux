#!/bin/bash
# Build the 7-Zip engine (Format7zF: all formats + codecs) as a shared library
# -> deps/7z.so. The NextZip plugin dlopen()s this at runtime and calls
# CreateObject / IInArchive / IOutArchive.
#
# The 7-Zip source (deps/7zip/) is NOT vendored in git — it is fetched on first
# build from 7-zip.org and sha256-verified (same version + hash as the macOS
# port). The gcc makefiles (cmpl_gcc*.mak, var_gcc*.mak) are part of upstream
# 7-Zip, so no patching is needed. aarch64 uses the in-tree native GAS assembly
# (7zAsm.S / LzmaDecOpt.S); other arches fall back to the generic pure-C build.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/7zip"
BUNDLE="$SRC/CPP/7zip/Bundles/Format7zF"
JOBS="$(nproc)"

# ── bootstrap: fetch + verify + extract upstream 7-Zip if not present ──────────
SZ_VER="2601"                       # 7-Zip 26.01 (2026-04-27)
SZ_TARBALL="7z${SZ_VER}-src.tar.xz"
SZ_URL="https://www.7-zip.org/a/${SZ_TARBALL}"
SZ_SHA256="b2389e0e930b2f9a348cf0fe7d9870a46482a8ec044ee0bdf42e2136db31c3d6"
if [ ! -f "$BUNDLE/makefile" ]; then
	echo "[7z] source not present — fetching 7-Zip ${SZ_VER} from 7-zip.org…"
	TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
	curl -fL --retry 3 -o "$TMP/$SZ_TARBALL" "$SZ_URL"
	echo "${SZ_SHA256}  ${TMP}/${SZ_TARBALL}" | sha256sum -c -
	mkdir -p "$SRC"
	tar -xf "$TMP/$SZ_TARBALL" -C "$SRC"
	echo "[7z] extracted to $SRC"
fi

ARCH="$(uname -m)"
case "$ARCH" in
	aarch64|arm64) MAK="../../cmpl_gcc_arm64.mak"; OUTDIR="b/g_arm64" ;;
	x86_64)        MAK="../../cmpl_gcc_x64.mak";   OUTDIR="b/g_x64"   ;;
	*)             MAK="../../cmpl_gcc.mak";       OUTDIR="b/g"       ;;
esac

# x86_64 assembly: upstream's var_gcc_x64.mak hardcodes USE_ASM=1 and drives
# `asmc` (or jwasm with USE_JWASM=1) over MASM-dialect .asm sources. Neither
# assembler is packaged by mainstream distros, and nasm/yasm cannot stand in —
# the makefile passes MASM flags (-nologo -Fo). Without one the build dies with
# "asmc: No such file or directory".
#
# So do what the macOS port does: keep the x64 makefile (IS_X64=1, correct
# output dir and flags) and override USE_ASM to empty, which GNU make's `ifdef`
# treats as undefined. That drops only the hand-written CRC/SHA/LZMA-decode
# fast paths; every format and codec is still built, in pure C.
#
# aarch64 needs no such treatment: its assembly is GNU-syntax .S assembled by
# the system gcc, so USE_ASM=1 works out of the box.
ASM_OVERRIDE=""
if [ "$OUTDIR" = "b/g_x64" ]; then
	if command -v asmc >/dev/null 2>&1; then
		echo "[7z] asmc found — building x86_64 with optimized assembly"
	elif command -v jwasm >/dev/null 2>&1; then
		echo "[7z] jwasm found — building x86_64 with optimized assembly"
		ASM_OVERRIDE="USE_JWASM=1"
	else
		echo "[7z] no asmc/jwasm — building x86_64 pure C (USE_ASM=)"
		ASM_OVERRIDE="USE_ASM="
	fi
fi

echo "[7z] building $ARCH via $MAK $ASM_OVERRIDE…"
( cd "$BUNDLE" && make -s -j"$JOBS" -f "$MAK" $ASM_OVERRIDE )

SO="$BUNDLE/$OUTDIR/7z.so"
[ -f "$SO" ] || { echo "[7z] ERROR: $SO not produced"; exit 1; }
cp "$SO" "$HERE/7z.so"
echo "[7z] -> $HERE/7z.so ($(du -h "$HERE/7z.so" | cut -f1))"
echo "[7z] done."
