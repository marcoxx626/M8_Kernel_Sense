#!/bin/bash

export TOP=/home/marcoxx/android/kernel/toolchain

export PATH=$TOP/arm-eabi-4.6/bin:$PATH

export ARCH=arm

export SUBARCH=arm

export CROSS_COMPILE=arm-eabi-

make m8_defconfig

make clean

make -j5


