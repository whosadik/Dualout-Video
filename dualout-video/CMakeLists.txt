# dualout-video/CMakeLists.txt
file(GLOB SRC_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_executable(dualout-video ${SRC_FILES})

target_include_directories(dualout-video PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/dualout-core
)

target_link_libraries(dualout-video PRIVATE
    dualout-core
    mf mfplat mfreadwrite mfuuid evr avrt ole32 uuid
)
