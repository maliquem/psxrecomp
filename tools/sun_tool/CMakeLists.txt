cmake_minimum_required(VERSION 3.20)
# Sun tool: a small SDL3 + Dear ImGui program to tune the sun and the moon of
# each area while a game runs (TCP sun_area of a build with the debug tools;
# sun_areas.txt next to the game's exe, read by the runtime at start). Any
# title with the GI shadow passes ([video.shadow]). Builds on its own:
#   cmake -S psxrecomp/tools/sun_tool -B build-suntool -G Ninja -DCMAKE_BUILD_TYPE=Release
# Dear ImGui comes from the game's recomp-ui, as the runtime's launcher does
# (RECOMP_UI_ROOT; empty = <game>/recomp-ui, the framework being <game>/psxrecomp).
project(PsxSunTool LANGUAGES C CXX)

set(RECOMP_UI_ROOT "" CACHE PATH "Path to recomp-ui; empty = <game>/recomp-ui")
if(NOT RECOMP_UI_ROOT)
    get_filename_component(RECOMP_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../recomp-ui" ABSOLUTE)
endif()
set(_sun_imgui "${RECOMP_UI_ROOT}/src/third_party/imgui")
set(_sun_stb "${RECOMP_UI_ROOT}/src/third_party")   # stb_image_write.h (public domain / MIT)
if(NOT EXISTS "${_sun_imgui}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui not found in ${_sun_imgui} (set RECOMP_UI_ROOT)")
endif()

get_filename_component(_sun_tc "${CMAKE_C_COMPILER}" DIRECTORY)
find_package(SDL3 3.2 CONFIG REQUIRED COMPONENTS SDL3
    HINTS "${_sun_tc}/../deps" PATH_SUFFIXES lib/cmake/SDL3)
if(TARGET SDL3::SDL3-static)
    set(_sun_sdl SDL3::SDL3-static)
else()
    set(_sun_sdl SDL3::SDL3)
endif()

add_executable(psx-sun-tool WIN32
    main.cpp
    "${_sun_imgui}/imgui.cpp"
    "${_sun_imgui}/imgui_draw.cpp"
    "${_sun_imgui}/imgui_tables.cpp"
    "${_sun_imgui}/imgui_widgets.cpp"
    "${_sun_imgui}/backends/imgui_impl_sdl3.cpp"
    "${_sun_imgui}/backends/imgui_impl_opengl3.cpp")
set_target_properties(psx-sun-tool PROPERTIES
    OUTPUT_NAME "psx_sun_tool"
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(psx-sun-tool PRIVATE "${_sun_imgui}" "${_sun_imgui}/backends" "${_sun_stb}")
target_compile_definitions(psx-sun-tool PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(psx-sun-tool PRIVATE ${_sun_sdl} opengl32 ws2_32)
if(MINGW)
    target_link_options(psx-sun-tool PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
