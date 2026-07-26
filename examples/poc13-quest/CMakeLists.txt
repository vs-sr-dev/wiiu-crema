add_executable(poc13_quest main.cpp)
target_link_libraries(poc13_quest crema)

wut_create_rpx(poc13_quest)

# The third example in a row that ships no art of its own. The hull, the font
# and the sound bank were baked once for PoC 10 and PoC 11, and the only thing
# this one generates is a ground texture — at runtime, on purpose, because the
# cost of a scene owning a texture is the measurement this PoC exists to take.
set(POC13_CONTENT "${CMAKE_CURRENT_BINARY_DIR}/bundle")
file(REMOVE_RECURSE "${POC13_CONTENT}")
crema_bake(bank "${CMAKE_CURRENT_BINARY_DIR}/audio.cbank"
                "${CMAKE_CURRENT_SOURCE_DIR}/../poc11-flight/assets/audio/bank.json")
crema_pak("${POC13_CONTENT}/assets.cpak"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc10-mesh/content/ship.cmesh"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc10-mesh/content/hull.ctex"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc11-flight/content/font.ctex"
          "${CMAKE_CURRENT_BINARY_DIR}/audio.cbank"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc11-flight/assets/audio/theme.csong")

wut_create_wuhb(poc13_quest
    NAME      "GX2 PoC13 - Quest"
    SHORTNAME "GX2PoC13"
    AUTHOR    "Samuele Voltan"
    CONTENT   "${POC13_CONTENT}"
)
