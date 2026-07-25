# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Samuele Voltan
cmake_minimum_required(VERSION 3.13)
project(crema C CXX)

# Crema — a clean-room GX2 rendering framework for Wii U homebrew.
# Sits on top of Espresso and Latte, like any good crema should.
add_library(crema STATIC
    crema/crema_app.c
    crema/crema_audio.c
    crema/crema_bank.c
    crema/crema_blend.c
    crema/crema_buffer.c
    crema/crema_effect.c
    crema/crema_frame.c
    crema/crema_collide.c
    crema/crema_entity.c
    crema/crema_input.c
    crema/crema_mesh.c
    crema/crema_music.c
    crema/crema_pak.c
    crema/crema_shader.cpp
    crema/crema_texture.c
)
target_include_directories(crema PUBLIC crema external)

# Archives are assembled at build time from the committed baked assets, so the
# same bytes are not in the repository twice.
find_program(CREMA_PYTHON NAMES python3 python REQUIRED)

function(crema_bake kind output input)
    execute_process(COMMAND "${CREMA_PYTHON}"
                    "${CMAKE_SOURCE_DIR}/tools/crema_bake.py" ${kind}
                    "${output}" "${input}" ${ARGN}
                    RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "crema_bake.py ${kind} -> ${output} failed (${result})")
    endif()
endfunction()

function(crema_pak output)
    execute_process(COMMAND "${CREMA_PYTHON}"
                    "${CMAKE_SOURCE_DIR}/tools/crema_bake.py" pak "${output}"
                    ${ARGN}
                    RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "crema_bake.py pak -> ${output} failed (${result})")
    endif()
endfunction()

add_subdirectory(examples/poc1-triangle)
add_subdirectory(examples/poc2-wirecube)
add_subdirectory(examples/poc3-flatcube)
add_subdirectory(examples/poc4-gouraud)
add_subdirectory(examples/poc5-stress)
add_subdirectory(examples/poc6-engine)
add_subdirectory(examples/poc7-fillrate)
add_subdirectory(examples/poc8-fence)
add_subdirectory(examples/poc9-scene)
add_subdirectory(examples/poc10-mesh)
add_subdirectory(examples/poc11-flight)
add_subdirectory(examples/poc12-shmup)
