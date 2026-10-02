cmake_minimum_required(VERSION 3.21)

project(NightCityWinds VERSION 0.7.0 LANGUAGES CXX)

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
target_link_libraries(NightCityWinds PRIVATE RED4ext::SDK)
target_compile_definitions(NightCityWinds PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(NightCityWinds PRIVATE /W4 /permissive- /Zc:preprocessor)
