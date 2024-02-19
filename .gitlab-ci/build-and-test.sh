#!/bin/bash

set -e
set -o xtrace

if [[ -z "$MESON_BUILDDIR" ]]; then
    MESON_BUILDDIR=build
fi

check_piglit_results ()
{
    local EXPECTED_RESULTS="$MESON_BUILDDIR"/test/piglit-results/$1
    local DEPENDENCY="$MESON_BUILDDIR"/$2

    if ! test -e $DEPENDENCY; then
	return
    fi

    if test -e $EXPECTED_RESULTS; then
	return
    fi

    echo Expected $EXPECTED_RESULTS does not exist
    exit 1
}

meson -Dc_args="-fno-common" -Dprefix=/usr -Dxephyr=true -Dwerror=true $MESON_EXTRA_OPTIONS "$MESON_BUILDDIR/"

export PIGLIT_DIR=/root/piglit XTEST_DIR=/root/xts LP_NUM_THREADS=0
ninja -j${FDO_CI_CONCURRENT:-4} -C "$MESON_BUILDDIR/"
meson test --num-processes ${FDO_CI_CONCURRENT:-4} --print-errorlogs -C "$MESON_BUILDDIR/"

check_piglit_results xephyr-glamor hw/kdrive/ephyr/Xephyr.p/ephyr_glamor.c.o
check_piglit_results xvfb hw/vfb/Xvfb
check_piglit_results xwayland hw/xwayland/Xwayland
