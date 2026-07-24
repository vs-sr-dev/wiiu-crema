# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
#
# Wii U / devkitPPC + WUT toolchain wrapper: layer on top of the official
# devkitPro CMake support (shipped in the devkitpro/devkitppc Docker image)
# to get powerpc-eabi-gcc, .rpx output rules and WUT search paths.

include($ENV{DEVKITPRO}/cmake/WiiU.cmake)

set(PLATFORM_WIIU TRUE)

add_definitions(-D__WIIU__ -D__WUT__)

# C11/C++17, keep FP ops individually rounded (fmadd contraction on PPC
# changes rounding vs the x86 reference — bit us on the LBA2 port).
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -ffp-contract=off")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffp-contract=off")
