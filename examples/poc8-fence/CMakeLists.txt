add_executable(poc8_fence main.cpp)
target_link_libraries(poc8_fence crema)

wut_create_rpx(poc8_fence)
wut_create_wuhb(poc8_fence
    NAME      "GX2 PoC8 - Fenced Pipeline"
    SHORTNAME "GX2PoC8"
    AUTHOR    "Samuele Voltan"
)
