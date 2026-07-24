add_executable(poc4_gouraud main.cpp)
target_link_libraries(poc4_gouraud crema)

wut_create_rpx(poc4_gouraud)
wut_create_wuhb(poc4_gouraud
    NAME      "GX2 PoC4 - Gouraud Torus"
    SHORTNAME "GX2PoC4"
    AUTHOR    "Samuele Voltan"
)
