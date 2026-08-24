#!/bin/sh
# guibuxwm build script
# Builds wlroots 0.20.2 (if not installed) and the WM.
# Usage: ./build.sh [clean|debug|release|sanitize|verbose]

set -e

WLR_VERSION=0.20.2
WLR_URL="https://gitlab.freedesktop.org/wlroots/wlroots/-/archive/${WLR_VERSION}/wlroots-${WLR_VERSION}.tar.gz"
PREFIX="${WLR_PREFIX:-$HOME/.local}"
SRC_DIR="${WLR_SRC_DIR:-/tmp/opencode}"

export PKG_CONFIG_PATH="${PREFIX}/lib64/pkgconfig:${PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH}"

have_wlroots() {
	pkg-config --exists wlroots-0.20 || return 1
	# a wlroots built with -Dxwayland=disabled lacks the xwayland symbols;
	# the .pc file records that, so rebuild instead of failing at link
	[ "$(pkg-config --variable=have_xwayland wlroots-0.20)" = "true" ]
}

build_wlroots() {
	echo ">> wlroots ${WLR_VERSION} with xwayland not found, building to ${PREFIX}"
	mkdir -p "${SRC_DIR}"
	tarball="${SRC_DIR}/wlroots-${WLR_VERSION}.tar.gz"
	if [ ! -f "${tarball}" ]; then
		curl -sSL -o "${tarball}" "${WLR_URL}"
	fi
	rm -rf "${SRC_DIR}/wlroots-${WLR_VERSION}" "${SRC_DIR}/wlroots-build"
	tar xzf "${tarball}" -C "${SRC_DIR}"
	meson setup "${SRC_DIR}/wlroots-build" "${SRC_DIR}/wlroots-${WLR_VERSION}" \
		--prefix="${PREFIX}" -Dxwayland=enabled -Dexamples=false
	ninja -C "${SRC_DIR}/wlroots-build"
	ninja -C "${SRC_DIR}/wlroots-build" install
}

if ! have_wlroots; then
	build_wlroots
fi

cd "$(dirname "$0")"

if [ "${1:-}" = "clean" ]; then
	rm -rf build
	echo ">> cleaned: build/ removed (incl. ./build/guibuxwm)"
	exit 0
fi

MESON_ARGS=""
VERBOSE=""
case "${1:-}" in
	debug)
		MESON_ARGS="--buildtype=debug"
		;;
	release)
		MESON_ARGS="--buildtype=release -Doptimization=o2"
		;;
	sanitize)
		MESON_ARGS="--buildtype=debug -Db_sanitize=address,undefined"
		;;
	verbose)
		VERBOSE="-v"
		;;
esac

if [ -d build ]; then
	meson setup build --reconfigure ${MESON_ARGS}
else
	meson setup build ${MESON_ARGS}
fi
ninja ${VERBOSE} -C build

echo ">> done: ./build/guibuxwm"
