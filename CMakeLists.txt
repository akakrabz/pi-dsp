cmake_minimum_required(VERSION 3.20)
project(pifx VERSION 0.2.0 LANGUAGES C CXX)

# pifx: audio scope / router / effects box for the Raspberry Pi 5 (and any Linux box).
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j4
#   ./build/pifx
#
# Dependencies that are not in Debian/Pi OS are fetched from GitHub at configure time
# (pinned revisions below). SDL2 and OpenGL ES come from the system:
#   sudo apt install libsdl2-dev libgles-dev libegl-dev
# For offline builds point FETCHCONTENT_SOURCE_DIR_<NAME> at local checkouts.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(PIFX_GUI   "Build the ImGui front end (needs SDL2 + OpenGL ES 3)" ON)
option(PIFX_TESTS "Build the unit tests (ctest)" ON)
option(PIFX_NATIVE "Tune for the build machine (-mcpu=native)" OFF)

find_package(Threads REQUIRED)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)   # FetchContent_Populate(dep): we only want the sources
endif()
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ---------------------------------------------------------------- dependencies
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.92.9b-docking
  GIT_SHALLOW    TRUE)
FetchContent_Declare(implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG        09e2ba71766e25d88053a2173936c9d1043bae42)
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG        0.11.25
  GIT_SHALLOW    TRUE)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE)
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20)

# Only populate (no add_subdirectory): we compile the few files we need ourselves.
foreach(dep miniaudio nlohmann_json)
  FetchContent_GetProperties(${dep})
  if(NOT ${dep}_POPULATED)
    FetchContent_Populate(${dep})
  endif()
endforeach()

# ---------------------------------------------------------------- core library
# Everything that does not need a window: audio, DSP, analysis, HAT, Launchpad.
add_library(pifx_core STATIC
  src/core/Dsp.cpp
  src/core/Sources.cpp
  src/core/Devices.cpp
  src/core/Engine.cpp
  src/core/Hijack.cpp
  src/core/Analysis.cpp
  src/core/Hat.cpp
  src/core/PadMap.cpp
  src/core/Launchpad.cpp
  src/core/Rig.cpp
  src/core/Util.cpp
  src/core/miniaudio_impl.c)
target_include_directories(pifx_core PUBLIC src
  ${miniaudio_SOURCE_DIR}
  ${nlohmann_json_SOURCE_DIR}/single_include)
target_compile_definitions(pifx_core PUBLIC PIFX_VERSION="${PROJECT_VERSION}")
target_link_libraries(pifx_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
if(PIFX_NATIVE)
  target_compile_options(pifx_core PUBLIC -mcpu=native)
endif()
# miniaudio is big and warns a lot; keep our own code clean.
set_source_files_properties(src/core/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(pifx_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)

# ---------------------------------------------------------------- executable
set(PIFX_APP_SOURCES src/main.cpp)
if(PIFX_GUI)
  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
  endif()
  FetchContent_GetProperties(implot)
  if(NOT implot_POPULATED)
    FetchContent_Populate(implot)
  endif()
  FetchContent_GetProperties(stb)
  if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
  endif()

  find_package(SDL2 REQUIRED CONFIG)
  find_path(GLES3_INCLUDE_DIR GLES3/gl3.h HINTS ${PIFX_GLES_INCLUDE})
  find_library(GLES_LIBRARY NAMES GLESv2 libGLESv2.so.2)
  if(NOT GLES3_INCLUDE_DIR OR NOT GLES_LIBRARY)
    message(FATAL_ERROR "OpenGL ES 3 not found: sudo apt install libgles-dev libegl-dev")
  endif()

  add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp)
  target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${implot_SOURCE_DIR}
    ${GLES3_INCLUDE_DIR})
  # 32-bit draw indices: dense plots (spectra, heatmap overlays) exceed 65k vertices and
  # the GLES backend cannot split them (no glDrawElementsBaseVertex in ES 3.0).
  target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_OPENGL_ES3 IMGUI_DEFINE_MATH_OPERATORS "ImDrawIdx=unsigned int")
  target_link_libraries(imgui_lib PUBLIC SDL2::SDL2 ${GLES_LIBRARY})
  target_compile_options(imgui_lib PRIVATE -w)

  # The UI font, embedded so the binary runs from anywhere.
  set(FONT_TTF ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf)
  set(FONT_MONO_TTF ${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf)
  set(FONT_CPP ${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts.cpp)
  add_custom_command(OUTPUT ${FONT_CPP}
    COMMAND ${CMAKE_COMMAND} -DOUT=${FONT_CPP} -DSANS=${FONT_TTF} -DMONO=${FONT_MONO_TTF}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_fonts.cmake
    DEPENDS ${FONT_TTF} ${FONT_MONO_TTF} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_fonts.cmake)

  list(APPEND PIFX_APP_SOURCES
    src/ui/App.cpp
    src/ui/Gl.cpp
    src/ui/Theme.cpp
    src/ui/ScopeView.cpp
    src/ui/SignalPanel.cpp
    src/ui/FxPanel.cpp
    src/ui/PadPanel.cpp
    ${FONT_CPP})
endif()

add_executable(pifx ${PIFX_APP_SOURCES})
target_link_libraries(pifx PRIVATE pifx_core)
target_compile_options(pifx PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(PIFX_GUI)
  target_link_libraries(pifx PRIVATE imgui_lib)
  target_include_directories(pifx PRIVATE ${stb_SOURCE_DIR})
  target_compile_definitions(pifx PRIVATE PIFX_HAVE_GUI=1)
endif()

# ---------------------------------------------------------------- tests
if(PIFX_TESTS)
  enable_testing()
  add_executable(pifx_tests
    tests/test_main.cpp
    tests/test_dsp.cpp
    tests/test_analysis.cpp
    tests/test_hat.cpp
    tests/test_launchpad.cpp
    tests/test_rig.cpp)
  target_link_libraries(pifx_tests PRIVATE pifx_core)
  target_include_directories(pifx_tests PRIVATE tests)
  add_test(NAME pifx_tests COMMAND pifx_tests)
endif()

install(TARGETS pifx RUNTIME DESTINATION bin)
