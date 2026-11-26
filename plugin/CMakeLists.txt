cmake_minimum_required(VERSION 3.24)
project(SkinFlow VERSION 0.1.1 LANGUAGES CXX)

# C++23 + dynamic CRT to match CommonLibSSE-NG (vcpkg triplet x64-windows-static-md).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# CommonLibSSE-NG (alandtse ng, v6.7.1): the submodule in extern/, the same commit Community Shaders 1.9.1 builds with.
set(SKINFLOW_COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../extern/CommonLibSSE-NG" CACHE PATH "CommonLibSSE-NG checkout")
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)
add_subdirectory(${SKINFLOW_COMMONLIB_DIR} commonlib EXCLUDE_FROM_ALL)

find_package(unofficial-simpleini CONFIG QUIET)

add_library(SkinFlow SHARED
	src/main.cpp
	src/Api.cpp
	src/Settings.cpp
	src/SurfacePool.cpp
	src/D3DStateBackup.cpp
	src/ShaderFile.cpp
	src/GravityPass.cpp
	src/FlowPass.cpp
	src/SeamMap.cpp
	src/SeamBuilder.cpp
	src/SurfaceProbe.cpp
	src/SettingsMenu.cpp
	src/RenderHooks.cpp)
target_include_directories(SkinFlow PRIVATE src include)
target_include_directories(SkinFlow SYSTEM PRIVATE external)  # SKSEMenuFramework.h (header only, calls into the framework's DLL)
target_precompile_headers(SkinFlow PRIVATE src/PCH.h)
target_link_libraries(SkinFlow PRIVATE CommonLibSSE::CommonLibSSE d3d11 d3dcompiler dxguid)
target_compile_definitions(SkinFlow PRIVATE SKINFLOW_VERSION="${PROJECT_VERSION}" NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(SkinFlow PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8 /wd4100 /wd4324)
target_link_options(SkinFlow PRIVATE /DEBUG:FULL)
