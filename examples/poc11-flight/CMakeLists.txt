add_executable(poc11_flight main.cpp)
target_link_libraries(poc11_flight crema)

wut_create_rpx(poc11_flight)

# One archive, assembled in the build tree: the ship and its hull are PoC 10's
# baked assets (the pipeline's output is content, not example code), the font
# atlas is this example's own. Three files went in, one goes into the .wuhb —
# which is the whole point, since what an asset costs on this console is
# dominated by touching its file at all.
set(POC11_CONTENT "${CMAKE_CURRENT_BINARY_DIR}/bundle")
# emptied first: the bundle is generated, and a file left behind by a previous
# configure would be shipped forever without ever appearing in this list
file(REMOVE_RECURSE "${POC11_CONTENT}")
crema_pak("${POC11_CONTENT}/assets.cpak"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc10-mesh/content/ship.cmesh"
          "${CMAKE_CURRENT_SOURCE_DIR}/../poc10-mesh/content/hull.ctex"
          "${CMAKE_CURRENT_SOURCE_DIR}/content/font.ctex")

wut_create_wuhb(poc11_flight
    NAME      "GX2 PoC11 - Flight"
    SHORTNAME "GX2PoC11"
    AUTHOR    "Samuele Voltan"
    CONTENT   "${POC11_CONTENT}"
)
