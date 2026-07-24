add_executable(poc11_flight main.cpp)
target_link_libraries(poc11_flight crema)

wut_create_rpx(poc11_flight)
# shares PoC 10's baked assets: the pipeline output is content, not example code
wut_create_wuhb(poc11_flight
    NAME      "GX2 PoC11 - Flight"
    SHORTNAME "GX2PoC11"
    AUTHOR    "Samuele Voltan"
    CONTENT   "${CMAKE_CURRENT_SOURCE_DIR}/../poc10-mesh/content"
)
