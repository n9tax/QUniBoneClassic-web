#!/bin/bash
# build-toolchain.sh - build a pdp11-aout cross compiler for net11:
# GNU binutils 2.42 and GCC 13.3.0 (C only) with the PDP-11 code generation
# fixes in gcc-13-pdp11-fixes.patch.
#
#   ./build-toolchain.sh [install prefix]      default: ./pdp11-tools
#
# Needs a host C/C++ compiler, make, wget, and the GMP, MPFR and MPC
# development files (Debian: libgmp-dev libmpfr-dev libmpc-dev).
# Takes 10-20 minutes on a PC. Not meant for the BeagleBone: build net11 on
# a PC and copy net11.lda over (a built net11.lda is in 10.02_devices/5_boot).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX=$(realpath -m "${1:-$HERE/pdp11-tools}")
WORK=$HERE/work
JOBS=$(nproc 2>/dev/null || echo 4)
mkdir -p "$WORK" && cd "$WORK"

[ -f binutils-2.42.tar.xz ] || wget https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz
[ -f gcc-13.3.0.tar.xz ] || wget https://ftp.gnu.org/gnu/gcc/gcc-13.3.0/gcc-13.3.0.tar.xz
[ -d binutils-2.42 ] || tar xf binutils-2.42.tar.xz
if [ ! -d gcc-13.3.0 ]; then
    tar xf gcc-13.3.0.tar.xz
    (cd gcc-13.3.0 && patch -p1 < "$HERE/gcc-13-pdp11-fixes.patch")
fi

mkdir -p b-binutils && cd b-binutils
../binutils-2.42/configure --target=pdp11-aout --prefix="$PREFIX" --disable-nls --disable-werror
make -j"$JOBS" && make install
cd ..

export PATH="$PREFIX/bin:$PATH"
# GCC 13 does not build with the C++ default of newer host compilers (char8_t)
export CXX="${CXX:-g++} -std=gnu++17" CC="${CC:-gcc} -std=gnu17"
mkdir -p b-gcc && cd b-gcc
../gcc-13.3.0/configure --target=pdp11-aout --prefix="$PREFIX" --enable-languages=c --disable-nls \
    --without-headers --with-newlib --disable-libssp --disable-libgomp --disable-libquadmath \
    --disable-libatomic --disable-shared --disable-threads --disable-multilib --disable-werror
make -j"$JOBS" all-gcc all-target-libgcc
make install-gcc install-target-libgcc
echo
echo "Done. Add to PATH: export PATH=$PREFIX/bin:\$PATH"
