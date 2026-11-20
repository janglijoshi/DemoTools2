#!/bin/sh
set -e
cd "$(dirname "$0")/.."
CXX=${CXX:-x86_64-w64-mingw32-clang++}   # llvm-mingw, on PATH or set CXX
rm -rf build/obj && mkdir -p build/obj
for f in src/*.cpp third_party/imgui/imgui*.cpp third_party/imgui/backends/imgui_impl_win32.cpp third_party/imgui/backends/imgui_impl_opengl2.cpp; do
  $CXX -std=c++20 -O2 -Wall -Wno-unused-function -c "$f" -Isrc -Ithird_party/imgui -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNDEBUG -o build/obj/$(basename "$f" .cpp).o
done
$CXX -shared -static -O2 -o build/DemoTools2.dll build/obj/*.o -lopengl32 -ldwmapi -limm32 -lgdi32 -luser32 -lshell32 -lcomdlg32 -Wl,--strip-all
ls -la build/DemoTools2.dll
