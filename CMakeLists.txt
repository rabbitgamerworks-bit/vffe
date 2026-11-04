cmake_minimum_required(VERSION 3.16)
project(JJFFE64 C CXX)

# ---------------------------------------------------------------------------
# JJFFE 64-bit / Vulkan
#
#   Windows x64 (MSVC or MinGW-w64):  native Win32 + Vulkan  -> ffewin64.exe
#   Linux / others:                   SDL2 + Vulkan          -> ffesdl2
#   Headless test build:              -DFFE_HEADLESS_TEST=ON  -> ffetest
#
# The 32-bit x86 game core (ffe.asm, ffedat.asm, ffebmp.asm) is translated to
# portable C by tools/asm2c.py.  The generated sources are committed in gen/,
# so Python is only needed with -DFFE_REGENERATE=ON (after editing the asm).
# ---------------------------------------------------------------------------

option(FFE_REGENERATE "Regenerate gen/ from the assembly sources (needs Python 3)" OFF)
option(FFE_HEADLESS_TEST "Also build the headless test executable" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(WARNING "This port targets 64-bit hosts; a 32-bit build works but is not the goal.")
endif()

set(GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/gen)
set(GEN_SOURCES
  ${GEN_DIR}/ffe_data.c
  ${GEN_DIR}/ffe_dispatch.c)
foreach(i RANGE 0 15)
  if(i LESS 10)
    list(APPEND GEN_SOURCES ${GEN_DIR}/ffe_code0${i}.c)
  else()
    list(APPEND GEN_SOURCES ${GEN_DIR}/ffe_code${i}.c)
  endif()
endforeach()

if(FFE_REGENERATE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_custom_command(
    OUTPUT ${GEN_SOURCES} ${GEN_DIR}/ffe_syms.h
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm2c.py
            ${CMAKE_CURRENT_SOURCE_DIR} ${GEN_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/asm2c.py
            ${CMAKE_CURRENT_SOURCE_DIR}/ffe.asm
            ${CMAKE_CURRENT_SOURCE_DIR}/ffedat.asm
            ${CMAKE_CURRENT_SOURCE_DIR}/ffebmp.asm
    COMMENT "Translating x86 assembly to C (tools/asm2c.py)")
endif()

# --- recompiled game core --------------------------------------------------
add_library(ffecore STATIC
  ${GEN_SOURCES}
  runtime/ffert.c
  ffecfg.c)
target_include_directories(ffecore PUBLIC runtime gen ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
  # generated code: huge functions full of labels; silence warnings
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0;/bigobj")
  target_compile_definitions(ffecore PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
  if(MINGW)
    set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-Wa,-mbig-obj")
  endif()
endif()
if(NOT MSVC)
  target_link_libraries(ffecore PUBLIC m)
endif()

# --- Vulkan (headers + volk loader, vendored in third_party) ----------------
add_library(ffevulkan STATIC
  third_party/volk/volk.c
  platform/vulkan/vkpresent.c)
target_include_directories(ffevulkan PUBLIC
  third_party/vulkan/include
  third_party/volk
  platform/vulkan)
target_link_libraries(ffevulkan PUBLIC ${CMAKE_DL_LIBS})

set(COMMON_SOURCES
  platform/common/fbvideo.c
  platform/common/keyb.c
  platform/common/mixer.c)

if(WIN32)
  # ---- Windows x64: native Win32 + Vulkan ----------------------------------
  target_compile_definitions(ffecore PUBLIC __FFEWIN64__)
  target_compile_definitions(ffevulkan PUBLIC VK_USE_PLATFORM_WIN32_KHR WIN32_LEAN_AND_MEAN NOMINMAX)
  add_executable(ffewin64 WIN32
    ${COMMON_SOURCES}
    platform/win64/win64main.c
    platform/win64/w64dir.cpp
    platform/win64/ffewin64.rc
    hmp.cpp)
  target_link_libraries(ffewin64 PRIVATE ffecore ffevulkan winmm user32 gdi32)
  if(MSVC)
    target_compile_definitions(ffewin64 PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
  else()
    target_link_options(ffewin64 PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
  set(FFE_MAIN_TARGET ffewin64)
else()
  # ---- SDL2 + Vulkan (Linux etc.) -------------------------------------------
  find_package(SDL2 REQUIRED)
  target_compile_definitions(ffecore PUBLIC __FFESDL2__)
  add_executable(ffesdl2
    ${COMMON_SOURCES}
    platform/sdl2/sdl2main.c
    linux/linuxdir.c)
  if(TARGET SDL2::SDL2)
    target_link_libraries(ffesdl2 PRIVATE SDL2::SDL2)
  else()
    target_include_directories(ffesdl2 PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(ffesdl2 PRIVATE ${SDL2_LIBRARIES})
  endif()
  target_link_libraries(ffesdl2 PRIVATE ffecore ffevulkan)
  set(FFE_MAIN_TARGET ffesdl2)
endif()

# Copy the config file next to the executable
add_custom_command(TARGET ${FFE_MAIN_TARGET} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/$<IF:$<BOOL:${WIN32}>,ffewin64.cfg,ffesdl2.cfg>
          $<TARGET_FILE_DIR:${FFE_MAIN_TARGET}>)

if(FFE_HEADLESS_TEST AND NOT WIN32)
  add_executable(ffetest tests/nullsys.c linux/linuxdir.c)
  target_link_libraries(ffetest PRIVATE ffecore)
endif()
