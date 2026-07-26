add_executable(poc14_kickoff main.cpp)
target_link_libraries(poc14_kickoff crema)

wut_create_rpx(poc14_kickoff)

# The first example since PoC 10 to ship art of its own, and it is three
# meshes totalling 368 triangles: a box, a sphere and a wedge. A team is not
# modelled, it is assembled — see tools/gen_bricks.py.
set(POC14_CONTENT "${CMAKE_CURRENT_BINARY_DIR}/bundle")
file(REMOVE_RECURSE "${POC14_CONTENT}")
crema_bake(bank "${CMAKE_CURRENT_BINARY_DIR}/match.cbank"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/bank.json")
crema_pak("${POC14_CONTENT}/assets.cpak"
          "${CMAKE_CURRENT_BINARY_DIR}/match.cbank"
          "${CMAKE_CURRENT_SOURCE_DIR}/content/box.cmesh"
          "${CMAKE_CURRENT_SOURCE_DIR}/content/sphere.cmesh"
          "${CMAKE_CURRENT_SOURCE_DIR}/content/wedge.cmesh"
          "${CMAKE_CURRENT_SOURCE_DIR}/content/grass.ctex"
          "${CMAKE_CURRENT_SOURCE_DIR}/content/net.ctex"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc11-flight/content/font.ctex")

wut_create_wuhb(poc14_kickoff
    NAME      "GX2 PoC14 - Kickoff"
    SHORTNAME "GX2PoC14"
    AUTHOR    "Samuele Voltan"
    CONTENT   "${POC14_CONTENT}"
)
