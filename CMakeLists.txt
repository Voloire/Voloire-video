cmake_minimum_required(VERSION 3.20)
project(voloire_video VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT MSVC)
  message(FATAL_ERROR "Voloire-video targets Windows with MSVC")
endif()

add_compile_options(/W3 /utf-8 /permissive- /EHsc /Zc:__cplusplus /MP /Zi)
add_link_options(/DEBUG /OPT:REF /OPT:ICF)  # pdb next to each binary: the crash handler symbolises with it
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00
                        VV_VERSION="${PROJECT_VERSION}")

set(VV_FFMPEG_ROOT "" CACHE PATH "Root of an FFmpeg shared dev build (include, lib, bin)")
option(VV_BUILD_UI "Build the Qt tray application" ON)

if(NOT VV_FFMPEG_ROOT)
  message(FATAL_ERROR "Set -DVV_FFMPEG_ROOT=<ffmpeg shared dev root>")
endif()

add_library(vv_ffmpeg INTERFACE)
target_include_directories(vv_ffmpeg SYSTEM INTERFACE ${VV_FFMPEG_ROOT}/include)
foreach(lib avformat avcodec avutil swscale swresample)
  find_library(VV_FF_${lib} ${lib} PATHS ${VV_FFMPEG_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
  target_link_libraries(vv_ffmpeg INTERFACE ${VV_FF_${lib}})
endforeach()

add_subdirectory(src/core)
add_subdirectory(src/cli)
add_subdirectory(tools/testsubject)
if(VV_BUILD_UI)
  add_subdirectory(src/ui)
endif()

enable_testing()
add_subdirectory(tests)

# Runtime DLLs next to the binaries so every target runs straight from the build tree.
file(GLOB VV_FF_DLLS ${VV_FFMPEG_ROOT}/bin/*.dll)
add_custom_target(vv_runtime ALL
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VV_FF_DLLS} ${CMAKE_BINARY_DIR}/bin)
