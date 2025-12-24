#!/bin/bash

mkdir out

export ARCH=arm64

export CROSS_COMPILE=$(pwd)/toolchain/bin/aarch64-linux-android-
make -C $(pwd) O=$(pwd)/out KCFLAGS=-mno-android m11q_open_defconfig
make -j16 -C $(pwd) O=$(pwd)/out KCFLAGS=-mno-android DTC_EXT=$(pwd)/tools/dtc CONFIG_BUILD_ARM64_DT_OVERLAY=y
 
cp out/arch/arm64/boot/Image $(pwd)/arch/arm64/boot/Image