cmake_minimum_required(VERSION 3.24)
project(ShadowLimitFix)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CommonLibSSE-NG source build. This fork carries SKSE/ContextHook.h
# (install_context_hook) and needs SKSE_SUPPORT_XBYAK + vcpkg xbyak.
# Machine-specific paths must come from -DCOMMONLIB_DIR / -DVCPKG_PREFIX
# (e.g. via CMakeUserPresets.json), NOT from hard-coded defaults.
if(NOT DEFINED COMMONLIB_DIR)
    message(FATAL_ERROR "COMMONLIB_DIR must be set (path to CommonLibSSE-NG sources)")
endif()
if(NOT DEFINED VCPKG_PREFIX)
    message(FATAL_ERROR "VCPKG_PREFIX must be set (vcpkg installed/x64-windows-static-md)")
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

# CommonLibSSE-NG sources (IPO check skipped - MSBuild crashes in some envs)
set(_CMAKE_CXX_IPO_MAY_BE_SUPPORTED_BY_COMPILER FALSE CACHE INTERNAL "skip IPO check")
set(_CMAKE_C_IPO_MAY_BE_SUPPORTED_BY_COMPILER FALSE CACHE INTERNAL "skip IPO check")
add_subdirectory(${CommonLibPath} ${CommonLibName} EXCLUDE_FROM_ALL)

# Dependencies from the vcpkg prebuilt tree
find_package(spdlog CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)
# Detours - the only sane hook base (auto register save/trampoline/2GB rel32;
# hand-rolled hooks crashed on rel32 overflow and register clobbering).
find_library(DETOURS_LIBRARY detours REQUIRED)

target_include_directories(${PROJECT_NAME} PRIVATE "${VCPKG_PREFIX}/include" src)

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE spdlog::spdlog "${DETOURS_LIBRARY}")

target_link_libraries(${PROJECT_NAME} PRIVATE d3d11 dxgi d3dcompiler)

# Optional post-build deploy to a game mod folder (-DPLUGIN_DEPLOY_DIR=...)
if(DEFINED PLUGIN_DEPLOY_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "${PLUGIN_DEPLOY_DIR}/SKSE/Plugins/${PROJECT_NAME}.dll"
    )
endif()
