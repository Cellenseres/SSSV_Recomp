cmake_minimum_required(VERSION 3.20)

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment version")
  enable_language(OBJC OBJCXX)
endif()

project(SSSVRecompiled LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# clang-cl: keep warnings manageable, but still MSVC-like
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-everything /W4")
endif()

# Avoid constexpr-mutex constructor issues with some VC runtime setups
if(WIN32)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
endif()

# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake >= 3.24
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
  cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(WIN32)
  set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/lib/")
endif()

# -----------------------------------------------------------------------------
# Third-party / subprojects
# -----------------------------------------------------------------------------
set(RT64_STATIC ON CACHE BOOL "" FORCE)
set(RT64_SDL_WINDOW_VULKAN ON CACHE BOOL "" FORCE)
add_compile_definitions(HLSL_CPU)

# Must be set before RecompFrontend/RT64 targets are configured so plume uses SDL window handles on Linux.
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  add_compile_definitions(PLUME_SDL_VULKAN_ENABLED RT64_SDL_WINDOW_VULKAN)
endif()

# rt64
add_subdirectory("${CMAKE_SOURCE_DIR}/lib/rt64" "${CMAKE_BINARY_DIR}/rt64")
target_include_directories(rt64 PRIVATE "${CMAKE_BINARY_DIR}/rt64/src")

# N64ModernRuntime + your SDK
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime")
add_subdirectory("${CMAKE_SOURCE_DIR}/lib/CellenseresSDK")

# -----------------------------------------------------------------------------
# SDL2 (MUST be configured before add_subdirectory(lib/RecompFrontend))
# -----------------------------------------------------------------------------
if(WIN32)
  include(FetchContent)

  if(DEFINED ENV{SDL2_VERSION})
    set(SDL2_VERSION $ENV{SDL2_VERSION})
  else()
    set(SDL2_VERSION "2.30.3")
  endif()

  FetchContent_Declare(
    sdl2
    URL "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip"
  )
  FetchContent_MakeAvailable(sdl2)

  # Provide vars in a way that subprojects can consume
  set(SDL2_INCLUDE_DIR "${sdl2_SOURCE_DIR}/include" CACHE PATH "" FORCE)
  set(SDL2_INCLUDE_DIRS "${sdl2_SOURCE_DIR}/include" CACHE PATH "" FORCE)

  # VS generator expects .lib
  set(SDL2_LIBRARY "${sdl2_SOURCE_DIR}/lib/x64/SDL2.lib" CACHE FILEPATH "" FORCE)
  set(SDL2_LIBRARIES "${sdl2_SOURCE_DIR}/lib/x64/SDL2.lib" CACHE FILEPATH "" FORCE)

  # Create an imported target if the Find-module did not
  if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 UNKNOWN IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
      IMPORTED_LOCATION "${sdl2_SOURCE_DIR}/lib/x64/SDL2.lib"
      INTERFACE_INCLUDE_DIRECTORIES "${sdl2_SOURCE_DIR}/include"
    )
  endif()
else()
  # Linux/macOS: rely on system SDL2
  find_package(SDL2 REQUIRED)
  # Normalize common vars for subprojects that use different naming
  if(DEFINED SDL2_INCLUDE_DIRS)
    set(SDL2_INCLUDE_DIRS "${SDL2_INCLUDE_DIRS}" CACHE PATH "" FORCE)
  elseif(DEFINED SDL2_INCLUDE_DIR)
    set(SDL2_INCLUDE_DIRS "${SDL2_INCLUDE_DIR}" CACHE PATH "" FORCE)
  endif()

  if(DEFINED SDL2_LIBRARIES)
    set(SDL2_LIBRARIES "${SDL2_LIBRARIES}" CACHE STRING "" FORCE)
  endif()
endif()

# -----------------------------------------------------------------------------
# DXC / SPIRV-Cross configuration (used by RecompFrontend/RT64 shader steps)
# NOTE: Must be set BEFORE add_subdirectory(lib/RecompFrontend)
# -----------------------------------------------------------------------------
set(DXC_COMMON_OPTS "-I${PROJECT_SOURCE_DIR}/src")
set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")

set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T" "ps_6_0" "-D" "DYNAMIC_RENDER_PARAMS")
set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T" "vs_6_0" "-D" "DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
set(DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T" "cs_6_0")
set(DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T" "gs_6_0")
set(DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3"
  "-fspv-target-env=vulkan1.1spirv1.4"
  "-fspv-extension=SPV_KHR_ray_tracing"
  "-fspv-extension=SPV_EXT_descriptor_indexing"
)

if(WIN32)
  set(DXC "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc.exe")
  add_compile_definitions(NOMINMAX)
else()
  if(APPLE)
    set(DXC
      ${CMAKE_COMMAND} -E env
      "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64"
      "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-macos"
    )

    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
      set(SPIRVCROSS
        ${CMAKE_COMMAND} -E env
        "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/spirv-cross/lib/x64"
        "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/spirv-cross/bin/x64/spirv-cross"
      )
    else()
      set(SPIRVCROSS
        ${CMAKE_COMMAND} -E env
        "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/spirv-cross/lib/arm64"
        "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/spirv-cross/bin/arm64/spirv-cross"
      )
    endif()
  else()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
      set(DXC
        ${CMAKE_COMMAND} -E env
        "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/x64"
        "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc-linux"
      )
    else()
      set(DXC
        ${CMAKE_COMMAND} -E env
        "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64"
        "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-linux"
      )
    endif()
  endif()
endif()

message(STATUS "DXC = ${DXC}")
if(DEFINED SPIRVCROSS)
  message(STATUS "SPIRVCROSS = ${SPIRVCROSS}")
endif()

# -----------------------------------------------------------------------------
# RecompFrontend (after SDL2 + DXC are set)
# -----------------------------------------------------------------------------
set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime" CACHE PATH "" FORCE)
set(RECOMP_FRONTEND_RT64_PATH "${CMAKE_SOURCE_DIR}/lib/rt64" CACHE PATH "" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/lib/RecompFrontend")

# Make sure RecompFrontend targets see SDL2 headers/libs (fixes SDL.h not found)
if(TARGET recompui)
  if(DEFINED SDL2_INCLUDE_DIRS)
    target_include_directories(recompui PRIVATE ${SDL2_INCLUDE_DIRS})
  elseif(DEFINED SDL2_INCLUDE_DIR)
    target_include_directories(recompui PRIVATE ${SDL2_INCLUDE_DIR})
  endif()

  if(TARGET SDL2::SDL2)
    target_link_libraries(recompui PRIVATE SDL2::SDL2)
  elseif(DEFINED SDL2_LIBRARIES)
    target_link_libraries(recompui PRIVATE ${SDL2_LIBRARIES})
  endif()
endif()

if(TARGET recompinput)
  if(DEFINED SDL2_INCLUDE_DIRS)
    target_include_directories(recompinput PRIVATE ${SDL2_INCLUDE_DIRS})
  elseif(DEFINED SDL2_INCLUDE_DIR)
    target_include_directories(recompinput PRIVATE ${SDL2_INCLUDE_DIR})
  endif()

  if(TARGET SDL2::SDL2)
    target_link_libraries(recompinput PRIVATE SDL2::SDL2)
  elseif(DEFINED SDL2_LIBRARIES)
    target_link_libraries(recompinput PRIVATE ${SDL2_LIBRARIES})
  endif()
endif()

# -----------------------------------------------------------------------------
# Recompiled funcs library
# -----------------------------------------------------------------------------
add_library(RecompiledFuncs STATIC)

target_compile_options(RecompiledFuncs PRIVATE
  -fno-strict-aliasing
  -Wno-unused-variable
  -Wno-implicit-function-declaration
)

target_include_directories(RecompiledFuncs PRIVATE
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include"
  "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include"
  "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include"
)

file(GLOB FUNC_C_SOURCES "${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c")
file(GLOB FUNC_CXX_SOURCES "${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp")
target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})

# -----------------------------------------------------------------------------
# Main executable
# -----------------------------------------------------------------------------
add_executable(SSSVRecompiled)

target_sources(SSSVRecompiled PRIVATE
  "${CMAKE_SOURCE_DIR}/src/main/main.cpp"
  "${CMAKE_SOURCE_DIR}/src/main/register_overlays.cpp"
  "${CMAKE_SOURCE_DIR}/src/main/theme.cpp"
  "${CMAKE_SOURCE_DIR}/src/main/launcher_animation.cpp"
  "${CMAKE_SOURCE_DIR}/src/game/config.cpp"
  "${CMAKE_SOURCE_DIR}/src/game/recomp_api.cpp"
  "${CMAKE_SOURCE_DIR}/src/game/sssv_billboard_rewrite.cpp"
  "${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp"
)

target_include_directories(SSSVRecompiled PRIVATE
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/src/main"
  "${CMAKE_SOURCE_DIR}/RecompiledFuncs"
  "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include"
  "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/sse2neon"
  "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib"
  "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include"
  "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc"
  "${CMAKE_SOURCE_DIR}/lib/rt64/src"
  "${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi"
  "${CMAKE_SOURCE_DIR}/lib/rt64/src/render"
  "${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompinput/include"
  "${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompui/include"
  "${CMAKE_SOURCE_DIR}/lib/RecompFrontend/recompui/src"
  "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include"
  "${CMAKE_CURRENT_BINARY_DIR}"
)

# Slightly better defaults for x64 builds
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  target_compile_options(SSSVRecompiled PRIVATE
    -march=nehalem
    -fno-strict-aliasing
    -fms-extensions
  )
else()
  target_compile_options(SSSVRecompiled PRIVATE
    -fno-strict-aliasing
    -fms-extensions
  )
endif()

if(MSVC)
  target_link_options(SSSVRecompiled PRIVATE /OPT:NOICF)
elseif(APPLE)
  target_link_options(SSSVRecompiled PRIVATE "-fuse-ld=${CMAKE_SOURCE_DIR}/.github/macos/ld64")
endif()

# -----------------------------------------------------------------------------
# Platform deps (your exe)
# -----------------------------------------------------------------------------
if(WIN32)
  # SDL2 already fetched above
  target_include_directories(SSSVRecompiled PRIVATE "${SDL2_INCLUDE_DIRS}")
  target_link_directories(SSSVRecompiled PRIVATE "${sdl2_SOURCE_DIR}/lib/x64")

  add_custom_command(TARGET SSSVRecompiled POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
      "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
      "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
      $<TARGET_FILE_DIR:SSSVRecompiled>
  )

  set_target_properties(SSSVRecompiled PROPERTIES
    LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
    LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
  )

  target_link_libraries(SSSVRecompiled PRIVATE SDL2 Winmm.lib Dbghelp.lib)
  target_sources(SSSVRecompiled PRIVATE "${CMAKE_SOURCE_DIR}/icons/app.rc")
endif()

if(APPLE)
  # SDL2 already found above
  if(DEFINED SDL2_INCLUDE_DIRS)
    target_include_directories(SSSVRecompiled PRIVATE ${SDL2_INCLUDE_DIRS})
  endif()

  set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
  set(THREADS_PREFER_PTHREAD_FLAG TRUE)
  find_package(Threads REQUIRED)

  if(TARGET SDL2::SDL2)
    target_link_libraries(SSSVRecompiled PRIVATE ${CMAKE_DL_LIBS} Threads::Threads SDL2::SDL2)
  else()
    target_link_libraries(SSSVRecompiled PRIVATE ${CMAKE_DL_LIBS} Threads::Threads ${SDL2_LIBRARIES})
  endif()

  include(${CMAKE_SOURCE_DIR}/.github/macos/apple_bundle.cmake)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  # SDL2 already found above
  find_package(X11 REQUIRED)
  find_package(Freetype REQUIRED)

  if(DEFINED SDL2_INCLUDE_DIRS)
    target_include_directories(SSSVRecompiled PRIVATE ${SDL2_INCLUDE_DIRS})
  endif()

  target_include_directories(SSSVRecompiled PRIVATE
    ${X11_INCLUDE_DIR}
    ${X11_Xrandr_INCLUDE_PATH}
  )

  target_link_libraries(SSSVRecompiled PRIVATE
    ${X11_LIBRARIES}
    ${X11_Xrandr_LIB}
    ${FREETYPE_LIBRARIES}
  )

  if(TARGET SDL2::SDL2)
    target_link_libraries(SSSVRecompiled PRIVATE SDL2::SDL2)
  else()
    target_link_libraries(SSSVRecompiled PRIVATE ${SDL2_LIBRARIES})
  endif()

  set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
  set(THREADS_PREFER_PTHREAD_FLAG TRUE)
  find_package(Threads REQUIRED)

  target_link_libraries(SSSVRecompiled PRIVATE "-latomic -static-libstdc++" ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# -----------------------------------------------------------------------------
# Link everything
# -----------------------------------------------------------------------------
target_link_libraries(SSSVRecompiled PRIVATE
  RecompiledFuncs
  cellenseres_sdk
  recompui
  recompinput
  librecomp
  ultramodern
  rt64
  nfd
)

# Make running from build output consistent with packaged builds.
add_custom_command(TARGET SSSVRecompiled POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:SSSVRecompiled>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/recompcontrollerdb.txt"
    "$<TARGET_FILE_DIR:SSSVRecompiled>/recompcontrollerdb.txt"
)

set_property(TARGET SSSVRecompiled PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
