add_executable(poc6_engine main.cpp)
target_link_libraries(poc6_engine crema)

wut_create_rpx(poc6_engine)
wut_create_wuhb(poc6_engine
    NAME      "GX2 PoC6 - Engine Style"
    SHORTNAME "GX2PoC6"
    AUTHOR    "Samuele Voltan"
)
