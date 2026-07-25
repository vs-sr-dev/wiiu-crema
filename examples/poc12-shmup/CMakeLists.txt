add_executable(poc12_shmup main.cpp)
target_link_libraries(poc12_shmup crema)

wut_create_rpx(poc12_shmup)

# Not one asset here is this example's own, and that is the point: the ship and
# its hull come from PoC 10's pipeline, the font and the sound bank from PoC 11.
# A second game built on the first game's content is what an asset pipeline is
# for — the bytes were baked once and neither example knows the other exists.
set(POC12_CONTENT "${CMAKE_CURRENT_BINARY_DIR}/bundle")
file(REMOVE_RECURSE "${POC12_CONTENT}")
crema_bake(bank "${CMAKE_CURRENT_BINARY_DIR}/audio.cbank"
                "${CMAKE_CURRENT_SOURCE_DIR}/../poc11-flight/assets/audio/bank.json")
crema_pak("${POC12_CONTENT}/assets.cpak"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc10-mesh/content/ship.cmesh"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc10-mesh/content/hull.ctex"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc11-flight/content/font.ctex"
          "${CMAKE_CURRENT_BINARY_DIR}/audio.cbank"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc11-flight/assets/audio/theme.csong")

wut_create_wuhb(poc12_shmup
    NAME      "GX2 PoC12 - Shmup"
    SHORTNAME "GX2PoC12"
    AUTHOR    "Samuele Voltan"
    CONTENT   "${POC12_CONTENT}"
)
