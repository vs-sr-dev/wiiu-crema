add_executable(poc10_mesh main.cpp)
target_link_libraries(poc10_mesh crema)

wut_create_rpx(poc10_mesh)

# This example ships the assets BOTH ways on purpose — loose files, and the
# same two assets packed into one .cpak — because the difference between them
# is what it measures.
set(POC10_CONTENT "${CMAKE_CURRENT_BINARY_DIR}/bundle")
file(REMOVE_RECURSE "${POC10_CONTENT}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/content/" DESTINATION "${POC10_CONTENT}")
crema_pak("${POC10_CONTENT}/assets.cpak"
          "${CMAKE_CURRENT_SOURCE_DIR}/content/ship.cmesh"
          "${CMAKE_CURRENT_SOURCE_DIR}/content/hull.ctex")

wut_create_wuhb(poc10_mesh
    NAME      "GX2 PoC10 - Squadron"
    SHORTNAME "GX2PoC10"
    AUTHOR    "Samuele Voltan"
    CONTENT   "${POC10_CONTENT}"
)
