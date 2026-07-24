add_executable(poc10_mesh main.cpp)
target_link_libraries(poc10_mesh crema)

wut_create_rpx(poc10_mesh)
wut_create_wuhb(poc10_mesh
    NAME      "GX2 PoC10 - Squadron"
    SHORTNAME "GX2PoC10"
    AUTHOR    "Samuele Voltan"
    CONTENT   "${CMAKE_CURRENT_SOURCE_DIR}/content"
)
