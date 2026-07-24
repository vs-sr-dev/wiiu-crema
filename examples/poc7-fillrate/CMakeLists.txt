add_executable(poc7_fillrate main.cpp)
target_link_libraries(poc7_fillrate crema)

wut_create_rpx(poc7_fillrate)
wut_create_wuhb(poc7_fillrate
    NAME      "GX2 PoC7 - Fill Rate"
    SHORTNAME "GX2PoC7"
    AUTHOR    "Samuele Voltan"
)
