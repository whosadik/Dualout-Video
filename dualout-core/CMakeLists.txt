add_library(dualout-core STATIC
    DualOutEngine.cpp
    DualOutEngine.h
    miniaudio.h
)

target_include_directories(dualout-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
