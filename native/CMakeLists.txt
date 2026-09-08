cmake_minimum_required(VERSION 3.28)
project(wastatushd LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Dependencies are cloned by CI into native/third_party to keep the repository small.
set(LSPLANT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/Dobby dobby-build)
add_subdirectory(third_party/LSPlant/lsplant/src/main/jni lsplant-build)

add_library(wastatushd SHARED
    jni/module.cpp
    elf_util.cpp
)

target_include_directories(wastatushd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/Dobby/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/LSPlant/lsplant/src/main/jni/include
)

target_compile_options(wastatushd PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fvisibility=hidden
)

target_link_libraries(wastatushd PRIVATE
    lsplant_static
    dobby_static
    log
    dl
    android
)
