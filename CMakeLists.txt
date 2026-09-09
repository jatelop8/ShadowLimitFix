cmake_minimum_required(VERSION 3.24)
project(ShadowLimitFix)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 独立 CommonLibSSE-NG（源码构建——CS fork，含 SKSE/ContextHook.h 的
# install_context_hook，需要 SKSE_SUPPORT_XBYAK + vcpkg xbyak）
if(NOT DEFINED COMMONLIB_DIR)
    set(COMMONLIB_DIR "D:/Modding/open-shaders/extern/CommonLibSSE-NG")
endif()

if(NOT DEFINED VCPKG_PREFIX)
    set(VCPKG_PREFIX "E:/vcpkg-full/installed/x64-windows-static-md")
endif()

set(CommonLibPath "${COMMONLIB_DIR}")
set(CommonLibName "CommonLibSSE")

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/ShadowLimitFix.cpp
    src/P1_hooks.cpp
    src/Scheduler.cpp
    src/ShaderReplace.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN SKYRIM)

target_compile_options(${PROJECT_NAME} PUBLIC
    "$<$<CONFIG:DEBUG>:/fp:strict;/ZI;/Od;/Gy>"
    "$<$<CONFIG:RELEASE>:/Zi;/fp:fast;/GL;/Gy-;/O2;/Ob2;/Oi;/Ot;/Oy>"
)

# CommonLibSSE-NG 源码（跳过 IPO 检查——某些环境 MSBuild 崩溃）
set(_CMAKE_CXX_IPO_MAY_BE_SUPPORTED_BY_COMPILER FALSE CACHE INTERNAL "skip IPO check")
set(_CMAKE_C_IPO_MAY_BE_SUPPORTED_BY_COMPILER FALSE CACHE INTERNAL "skip IPO check")
add_subdirectory(${CommonLibPath} ${CommonLibName} EXCLUDE_FROM_ALL)

# 依赖：spdlog / directxtk（CommonLibSSE-NG 需要），从 vcpkg 预装库查找
find_package(spdlog CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
# Detours（成熟 hook 库——自动处理寄存器保存/trampoline/2GB 跳转，
# 手写 hook 的 rel32 溢出+寄存器污染两个崩溃教训证明必须用它）
find_library(DETOURS_LIBRARY detours REQUIRED)

target_include_directories(${PROJECT_NAME} PRIVATE "${VCPKG_PREFIX}/include" src)

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE spdlog::spdlog "${DETOURS_LIBRARY}")

# DX 库（P1 起 shader 替换用）
target_link_libraries(${PROJECT_NAME} PRIVATE d3d11 dxgi d3dcompiler)

# 生成后复制到游戏 mod 目录（-DPLUGIN_DEPLOY_DIR 指定）
if(DEFINED PLUGIN_DEPLOY_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "${PLUGIN_DEPLOY_DIR}/SKSE/Plugins/${PROJECT_NAME}.dll"
    )
endif()
