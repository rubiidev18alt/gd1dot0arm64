# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(gd102_arm64_host CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# Dynarmic A32 -> AArch64 JIT frontend
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(Boost_INCLUDE_DIR ${TP}/boost CACHE PATH "" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
add_subdirectory(${TP}/dynarmic dynarmic EXCLUDE_FROM_ALL)

add_library(game SHARED
    src/abi.h
    src/common.h
    src/cpu.cpp
    src/cpu.h
    src/host_main.cpp
    src/jni_bridge.cpp
    src/jni_bridge.h
    src/loader.cpp
    src/loader.h
    src/local_gdps.cpp
    src/local_gdps.h
    src/mem.cpp
    src/natives.h
    src/paths.cpp
    src/paths.h
    src/thunks_gl.cpp
    src/thunks_gl.h
    src/thunks_libc.cpp
    src/thunks_pthread.cpp
)

set_target_properties(game PROPERTIES OUTPUT_NAME "game")
target_include_directories(game PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TP}/misc
)
target_compile_options(game PRIVATE -Wall -Wno-unused-function -fexceptions)
target_link_options(game PRIVATE
    -Wl,--gc-sections
    -Wl,-z,max-page-size=16384
    -Wl,-z,common-page-size=16384
)

if(ANDROID)
    target_compile_options(game PRIVATE -fvisibility=hidden)
    target_link_libraries(game PRIVATE dynarmic log android EGL GLESv2 z)
else()
    find_package(JNI REQUIRED)
    find_package(ZLIB REQUIRED)
    target_include_directories(game PRIVATE ${JNI_INCLUDE_DIRS})
    target_link_libraries(game PRIVATE dynarmic EGL GLESv2 ZLIB::ZLIB pthread dl m)

    option(GD102_BUILD_VERIFY_RUNNER "Build headless Linux ARM64 verification runner" ON)
    if(GD102_BUILD_VERIFY_RUNNER)
        add_executable(gd102_verify_runner
            ${CMAKE_CURRENT_SOURCE_DIR}/../tests/gd102_verify_runner.cpp
        )
        target_include_directories(gd102_verify_runner PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${JNI_INCLUDE_DIRS}
        )
        target_compile_options(gd102_verify_runner PRIVATE -Wall -fexceptions)
        target_link_libraries(gd102_verify_runner PRIVATE game EGL GLESv2 pthread dl)
    endif()
endif()
