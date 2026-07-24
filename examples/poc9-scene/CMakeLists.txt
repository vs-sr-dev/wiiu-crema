add_executable(poc9_scene main.cpp)
target_link_libraries(poc9_scene crema)

wut_create_rpx(poc9_scene)
wut_create_wuhb(poc9_scene
    NAME      "GX2 PoC9 - Scene"
    SHORTNAME "GX2PoC9"
    AUTHOR    "Samuele Voltan"
)
