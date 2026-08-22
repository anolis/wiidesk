#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

source_dir=${1:-/tmp/libvncserver-0.9.15}
prefix=${2:-/tmp/wii-libvncserver}
tag=LibVNCServer-0.9.15
commit=9b54b1ec32731bd23158ca014dc18014db4194c3
build_dir=$source_dir/build-wii
toolchain=$build_dir/wii-powerpc-toolchain.cmake

if [[ ! -d $source_dir/.git ]]; then
	git clone --depth 1 --branch "$tag" \
		https://github.com/LibVNC/libvncserver.git "$source_dir"
fi

if [[ $(git -C "$source_dir" rev-parse HEAD) != "$commit" ]]; then
	echo "$source_dir is not pinned LibVNCServer commit $commit" >&2
	exit 1
fi

mkdir -p "$build_dir"
cat >"$toolchain" <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR powerpc)
set(CMAKE_C_COMPILER powerpc-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER powerpc-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/powerpc-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

cmake -S "$source_dir" -B "$build_dir" \
	-DCMAKE_TOOLCHAIN_FILE="$toolchain" \
	-DCMAKE_BUILD_TYPE=MinSizeRel \
	-DCMAKE_INSTALL_PREFIX="$prefix" \
	-DBUILD_SHARED_LIBS=OFF \
	-DWITH_ZLIB=OFF \
	-DWITH_LZO=OFF \
	-DWITH_JPEG=OFF \
	-DWITH_PNG=OFF \
	-DWITH_SDL=OFF \
	-DWITH_GTK=OFF \
	-DWITH_QT=OFF \
	-DWITH_LIBSSHTUNNEL=OFF \
	-DWITH_THREADS=OFF \
	-DWITH_GNUTLS=OFF \
	-DWITH_OPENSSL=OFF \
	-DWITH_SYSTEMD=OFF \
	-DWITH_GCRYPT=OFF \
	-DWITH_FFMPEG=OFF \
	-DWITH_TIGHTVNC_FILETRANSFER=OFF \
	-DWITH_IPv6=OFF \
	-DWITH_WEBSOCKETS=OFF \
	-DWITH_SASL=OFF \
	-DWITH_XCB=OFF \
	-DWITH_EXAMPLES=OFF \
	-DWITH_TESTS=OFF
cmake --build "$build_dir" -j16
cmake --install "$build_dir"

sha256sum "$prefix/lib/libvncserver.a"
