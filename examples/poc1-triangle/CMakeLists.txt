add_executable(poc1_triangle main.cpp)
target_link_libraries(poc1_triangle crema)

wut_create_rpx(poc1_triangle)
wut_create_wuhb(poc1_triangle
    NAME      "GX2 PoC1 - Triangle"
    SHORTNAME "GX2PoC1"
    AUTHOR    "Samuele Voltan"
)
