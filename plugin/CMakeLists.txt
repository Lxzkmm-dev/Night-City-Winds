cmake_minimum_required(VERSION 3.21)

project(NightCityWinds VERSION 0.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(RED4EXT_HEADER_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../deps/RED4ext.SDK-master" RED4ext.SDK EXCLUDE_FROM_ALL)

file(GLOB SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(NightCityWinds SHARED ${SOURCES})
target_include_directories(NightCityWinds PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# PhysX 3.4 public headers (BSD-3), only for the virtual interfaces of the game's own PhysX DLLs;
# nothing is linked, PxGetPhysics is looked up at runtime.
target_include_directories(NightCityWinds SYSTEM PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../deps/physx34/PhysX_3.4/Include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../deps/physx34/PxShared/include")
# REDGraph's client header (MIT, plain C; nothing is linked - the host is found at runtime) and the
# foliage-wind patch build_shaders.py generates from the installed game (build.ps1 runs it)
target_include_directories(NightCityWinds PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../deps/REDGraph/include"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(NightCityWinds PRIVATE RED4ext::SDK)
target_compile_definitions(NightCityWinds PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(NightCityWinds PRIVATE /W4 /permissive- /Zc:preprocessor)
