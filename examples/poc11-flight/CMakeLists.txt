add_executable(poc11_flight main.cpp)
target_link_libraries(poc11_flight crema)

wut_create_rpx(poc11_flight)

# The bundle is assembled in the build tree instead of committing the same
# bytes twice: the ship and its hull are PoC 10's baked assets (the pipeline's
# output is content, not example code), the font atlas is this example's own.
set(POC11_CONTENT "${CMAKE_CURRENT_BINARY_DIR}/bundle")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/../poc10-mesh/content/"
     DESTINATION "${POC11_CONTENT}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/content/"
     DESTINATION "${POC11_CONTENT}")

wut_create_wuhb(poc11_flight
    NAME      "GX2 PoC11 - Flight"
    SHORTNAME "GX2PoC11"
    AUTHOR    "Samuele Voltan"
    CONTENT   "${POC11_CONTENT}"
)
