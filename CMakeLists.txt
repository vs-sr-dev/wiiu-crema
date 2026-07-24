# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
cmake_minimum_required(VERSION 3.13)
project(crema C CXX)

# Crema — a clean-room GX2 rendering framework for Wii U homebrew.
# Sits on top of Espresso and Latte, like any good crema should.
add_library(crema STATIC
    crema/crema_app.c
    crema/crema_shader.cpp
)
target_include_directories(crema PUBLIC crema external)

add_subdirectory(examples/poc1-triangle)
add_subdirectory(examples/poc2-wirecube)
add_subdirectory(examples/poc3-flatcube)
add_subdirectory(examples/poc4-gouraud)
add_subdirectory(examples/poc5-stress)
add_subdirectory(examples/poc6-engine)
add_subdirectory(examples/poc7-fillrate)
add_subdirectory(examples/poc8-fence)
add_subdirectory(examples/poc9-scene)
