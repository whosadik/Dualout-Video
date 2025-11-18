# БЫЛО (как выше)

# СТАЛО
file(GLOB SRC_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# исключаем enum_hwnd.cpp из основного exe
list(FILTER SRC_FILES EXCLUDE REGEX "enum_hwnd\\.cpp$")

# основной сервис dualout-video
add_executable(dualout-video ${SRC_FILES})

# отдельный маленький exe для перечисления HWND
add_executable(enum_hwnd
    "${CMAKE_CURRENT_SOURCE_DIR}/src/enum_hwnd.cpp"
)

target_include_directories(dualout-video PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/dualout-core
)

target_link_libraries(dualout-video PRIVATE
    dualout-core
    mf mfplat mfreadwrite mfuuid evr avrt ole32 uuid
)

# --- НОВОЕ: положить dualout-video.exe в общую bin-папку ---
set(BA_BIN "C:/Users/Legion/brainAudio/third_party/bin")

add_custom_command(TARGET dualout-video POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${BA_BIN}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:dualout-video>
          "${BA_BIN}/dualout-video.exe"
  VERBATIM
)
