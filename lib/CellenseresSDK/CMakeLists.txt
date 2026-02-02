add_library(cellenseres_sdk STATIC
    src/launcher_music.cpp
)

target_include_directories(cellenseres_sdk
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
