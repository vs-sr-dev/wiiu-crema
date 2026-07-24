add_executable(poc3_flatcube main.cpp)
target_link_libraries(poc3_flatcube crema)

wut_create_rpx(poc3_flatcube)
wut_create_wuhb(poc3_flatcube
    NAME      "GX2 PoC3 - Flat Cube"
    SHORTNAME "GX2PoC3"
    AUTHOR    "Samuele Voltan"
)
