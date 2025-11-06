cmake_minimum_required(VERSION 3.20)
project(auto_aspect_plugin LANGUAGES CXX)

add_library(auto_aspect SHARED src/auto_aspect.cpp)

target_compile_features(auto_aspect PRIVATE cxx_std_17)

target_compile_definitions(auto_aspect PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_include_directories(auto_aspect PRIVATE include)

set_target_properties(auto_aspect PROPERTIES
    OUTPUT_NAME "auto_aspect"
    SUFFIX ".aux2"
)

if (MSVC)
    target_compile_options(auto_aspect PRIVATE /W4 /permissive-)
else()
    target_compile_options(auto_aspect PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(auto_aspect PRIVATE
    mfplat
    mfreadwrite
    mfuuid
    windowscodecs
)
