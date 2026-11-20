cmake_minimum_required(VERSION 3.20)
project(DemoTools2 CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # no VC++ redistributable needed
endif()

set(IMGUI_SRC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp third_party/imgui/backends/imgui_impl_opengl2.cpp)

add_library(DemoTools2 SHARED
  src/main.cpp src/game.cpp src/skins.cpp src/menu.cpp src/render.cpp src/util.cpp src/calc.cpp src/console.cpp src/ui.cpp src/campath.cpp src/pov.cpp src/demoscan.cpp src/record.cpp src/hotkeys.cpp
  ${IMGUI_SRC})
target_include_directories(DemoTools2 PRIVATE src third_party/imgui)
target_compile_definitions(DemoTools2 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX NDEBUG _CRT_SECURE_NO_WARNINGS)
target_link_libraries(DemoTools2 PRIVATE opengl32 dwmapi imm32 shell32 comdlg32)
