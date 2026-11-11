#!/bin/bash
# build.sh - build fuzix.rl02.gz from source: Fuzix with the PDP-11 port of
# PR #1205 (codeberg.org/EtchedPixels/FUZIX), plus the patches in patches/.
#
#   ./build.sh                 work tree in ./work, result ./fuzix.rl02.gz
#
# Needs git, a host C compiler, python3, and the patched pdp11-aout cross
# compiler from ../10.06_net11/toolchain (build-toolchain.sh). Set
# PDP11_TOOLS to its install prefix if it is somewhere else.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=${PDP11_TOOLS:-$HERE/../10.06_net11/toolchain/pdp11-tools}
REPO=https://codeberg.org/migraineman/FUZIX.git
BRANCH=pdp11-port
BASE=aa5e80f0b          # last commit of the PDP-11/40 port the patches apply to
WORK=$HERE/work

if ! [ -x "$TOOLS/bin/pdp11-aout-gcc" ]; then
    echo "No pdp11-aout-gcc in $TOOLS/bin: build it with ../10.06_net11/toolchain/build-toolchain.sh"
    exit 1
fi
export PATH="$TOOLS/bin:$PATH"

mkdir -p "$WORK"
if [ ! -d "$WORK/FUZIX" ]; then
    git clone --single-branch --branch "$BRANCH" "$REPO" "$WORK/FUZIX"
    cd "$WORK/FUZIX"
    git checkout -q -b pdp11-qbone "$BASE"
    git -c user.name=build -c user.email=build@localhost am -q "$HERE"/patches/*.patch
fi
cd "$WORK/FUZIX"

echo "== host tools"
make -C Standalone > "$WORK/standalone.log" 2>&1

echo "== C library"
(cd Library && make tools/syscall_pdp11) > "$WORK/library.log" 2>&1
make -C Library/libs -f Makefile.pdp11 >> "$WORK/library.log" 2>&1

echo "== kernel"
make -C Kernel TARGET=pdp11dec VERSION=0.5 SUBVERSION='""' > "$WORK/kernel.log" 2>&1

echo "== programs and disk image"
(cd Kernel/platform/platform-pdp11dec && ./build_fuzix_disk.sh) > "$WORK/disk.log" 2>&1
grep "^Kernel:" "$WORK/disk.log"

gzip -9 -c Kernel/platform/platform-pdp11dec/fuzix_rl02.dsk > "$HERE/fuzix.rl02.gz"
echo "Done: $HERE/fuzix.rl02.gz"
