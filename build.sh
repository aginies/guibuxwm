#!/bin/sh
# guibuxwm build script
# Builds wlroots 0.20.2 (if not installed) and the WM.

set -e

WLR_VERSION=0.20.2
WLR_URL="https://gitlab.freedesktop.org/wlroots/wlroots/-/archive/${WLR_VERSION}/wlroots-${WLR_VERSION}.tar.gz"
PREFIX="${WLR_PREFIX:-$HOME/.local}"
SRC_DIR="${WLR_SRC_DIR:-/tmp/opencode}"

export PKG_CONFIG_PATH="${PREFIX}/lib64/pkgconfig:${PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH}"

have_wlroots() {
	pkg-config --exists wlroots-0.20
}

build_wlroots() {
	echo ">> wlroots ${WLR_VERSION} not found, building to ${PREFIX}"
	mkdir -p "${SRC_DIR}"
	tarball="${SRC_DIR}/wlroots-${WLR_VERSION}.tar.gz"
	if [ ! -f "${tarball}" ]; then
		curl -sSL -o "${tarball}" "${WLR_URL}"
	fi
	rm -rf "${SRC_DIR}/wlroots-${WLR_VERSION}" "${SRC_DIR}/wlroots-build"
	tar xzf "${tarball}" -C "${SRC_DIR}"
	meson setup "${SRC_DIR}/wlroots-build" "${SRC_DIR}/wlroots-${WLR_VERSION}" \
		--prefix="${PREFIX}" -Dxwayland=disabled -Dexamples=false
	ninja -C "${SRC_DIR}/wlroots-build"
	ninja -C "${SRC_DIR}/wlroots-build" install
}

if ! have_wlroots; then
	build_wlroots
fi

cd "$(dirname "$0")"

if [ -d build ]; then
	meson setup build --reconfigure
else
	meson setup build
fi
ninja -C build

echo ">> done: ./build/guibuxwm"
