add_executable(poc5_stress main.cpp)
target_link_libraries(poc5_stress crema)

wut_create_rpx(poc5_stress)
wut_create_wuhb(poc5_stress
    NAME      "GX2 PoC5 - Stress Test"
    SHORTNAME "GX2PoC5"
    AUTHOR    "Samuele Voltan"
)
