add_executable(poc2_wirecube main.cpp)
target_link_libraries(poc2_wirecube crema)

wut_create_rpx(poc2_wirecube)
wut_create_wuhb(poc2_wirecube
    NAME      "GX2 PoC2 - Wireframe Cube"
    SHORTNAME "GX2PoC2"
    AUTHOR    "Samuele Voltan"
)
