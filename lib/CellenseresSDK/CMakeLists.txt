add_library(cellenseres_sdk STATIC
    src/launcher_music.cpp
    src/launcher_model3d.cpp
    src/ui_bridge.cpp
)

# `build_*_shader` helpers come from rt64 CMake functions and rely on DXC* vars.
# CellenseresSDK is added before the top-level DXC vars are configured, so provide
# local fallbacks here to keep shader generation deterministic.
if(NOT DEFINED DXC_COMMON_OPTS)
    set(DXC_COMMON_OPTS "-I${PROJECT_SOURCE_DIR}/src")
endif()
if(NOT DEFINED DXC_DXIL_OPTS)
    set(DXC_DXIL_OPTS "-Wno-ignored-attributes")
endif()
if(NOT DEFINED DXC_SPV_OPTS)
    set(DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
endif()
if(NOT DEFINED DXC_PS_OPTS)
    set(DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T" "ps_6_0" "-D" "DYNAMIC_RENDER_PARAMS")
endif()
if(NOT DEFINED DXC_VS_OPTS)
    set(DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T" "vs_6_0" "-D" "DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
endif()
if(WIN32 AND NOT DEFINED DXC)
    set(DXC "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc.exe")
elseif(APPLE AND NOT DEFINED DXC)
    set(DXC
        ${CMAKE_COMMAND} -E env
        "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64"
        "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-macos"
    )
elseif(NOT WIN32 AND NOT APPLE AND NOT DEFINED DXC)
    set(DXC
        ${CMAKE_COMMAND} -E env
        "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/x64"
        "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc-linux"
    )
endif()

target_compile_definitions(cellenseres_sdk
    PRIVATE
        RMLUI_STATIC_LIB
        NOMINMAX
)

target_include_directories(cellenseres_sdk
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf
        ${CMAKE_BINARY_DIR}/shaders
        ${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib
        ${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/plume
        ${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/plume/contrib/volk
        ${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/plume/contrib/VulkanMemoryAllocator/include
        ${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/plume/contrib/Vulkan-Headers/include
        ${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/plume/contrib/D3D12MemoryAllocator/include
        ${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
        ${PROJECT_SOURCE_DIR}/lib/rt64/src
        ${PROJECT_SOURCE_DIR}/lib/rt64/src/rhi
        ${PROJECT_SOURCE_DIR}/lib/rt64/src/render
        ${PROJECT_SOURCE_DIR}/lib/RecompFrontend/recompui/include
        ${PROJECT_SOURCE_DIR}/lib/RecompFrontend/recompui/lib/RmlUi/Include
        ${PROJECT_SOURCE_DIR}/lib/RecompFrontend/recompui/src
        ${PROJECT_SOURCE_DIR}/lib/RecompFrontend/recompinput/include
        ${PROJECT_SOURCE_DIR}/lib/RecompFrontend/lib/GamepadMotionHelpers
        ${PROJECT_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
        ${PROJECT_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
        ${PROJECT_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
        ${PROJECT_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/concurrentqueue
)

if(WIN32)
    target_include_directories(cellenseres_sdk PRIVATE ${SDL2_INCLUDE_DIRS})
elseif(APPLE OR CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_include_directories(cellenseres_sdk PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

build_vertex_shader(cellenseres_sdk "lib/CellenseresSDK/shaders/LauncherModelVS.hlsl" "shaders/LauncherModelVS.hlsl")
build_pixel_shader(cellenseres_sdk "lib/CellenseresSDK/shaders/LauncherModelPS.hlsl" "shaders/LauncherModelPS.hlsl")
