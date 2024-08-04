
cmake_minimum_required(VERSION 3.6)
project(OHook CXX)
enable_language(C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/MP /MD)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

#MinHook
file(GLOB MINHOOK_SOURCES minhook/*.c minhook/hde/*.c)
file(GLOB MINHOOK_HEADERS minhook/*.h minhook/hde/*.h)
add_library(minhook STATIC ${MINHOOK_SOURCES} ${MINHOOK_HEADERS})
target_include_directories(minhook PUBLIC minhook)

# ImGUI
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/thirdparty/imgui)
file(GLOB IMGUI_SOURCES 
		${IMGUI_DIR}/*.cpp 
		${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
		${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
		${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
file(GLOB IMGUI_HEADERS 
		${IMGUI_DIR}/*.h 
		${IMGUI_DIR}/backends/imgui_impl_dx11.h
		${IMGUI_DIR}/backends/imgui_impl_dx12.h
		${IMGUI_DIR}/backends/imgui_impl_win32.h)

add_library(imgui STATIC ${IMGUI_SOURCES} ${IMGUI_HEADERS})
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

# UniversalHookX
file(GLOB_RECURSE UNIVERSALHOOK_SOURCES UniversalHookX/*.cpp)
file(GLOB_RECURSE UNIVERSALHOOK_HEADERS UniversalHookX/*.hpp UniversalHookX/*.h)
add_library(UniversalHookX STATIC ${UNIVERSALHOOK_SOURCES} ${UNIVERSALHOOK_HEADERS})
target_include_directories(UniversalHookX PUBLIC UniversalHookX)
target_link_libraries(UniversalHookX PUBLIC imgui minhook)

# PaliaSDK
file(GLOB_RECURSE PALIASDK_SOURCES PaliaSDK/*.cpp)
file(GLOB_RECURSE PALIASDK_HEADERS PaliaSDK/*.hpp)
add_library(PaliaSDK STATIC ${PALIASDK_SOURCES} ${PALIASDK_HEADERS})
target_include_directories(PaliaSDK PUBLIC PaliaSDK)
target_precompile_headers(PaliaSDK PRIVATE PaliaSDK/SDK.hpp)

# JsonCPP
file(GLOB_RECURSE JSONCPP_SOURCES jsoncpp/dist/*.cpp)
file(GLOB_RECURSE JSONCPP_HEADERS jsoncpp/include/json/*.h)
add_library(jsoncpp STATIC ${JSONCPP_SOURCES} ${JSONCPP_HEADERS})
target_include_directories(jsoncpp PUBLIC jsoncpp/include)

# OHook DLL
if(EXISTS "${CMAKE_BINARY_DIR}/OHook.pdb")
   file(REMOVE ${CMAKE_BINARY_DIR}/OHook.pdb)
endif()
file(GLOB_RECURSE OHook_SOURCES OHook/*.cpp)
file(GLOB_RECURSE OHook_HEADERS OHook/*.h)
add_library(OHook SHARED ${OHook_SOURCES} ${OHook_HEADERS})
target_include_directories(OHook PUBLIC OHook)
target_link_libraries(OHook PRIVATE PaliaSDK jsoncpp UniversalHookX minhook $<$<BOOL:${WIN32}>:ws2_32> $<$<BOOL:${WIN32}>:Winmm.lib>)