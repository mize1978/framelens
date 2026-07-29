#!/usr/bin/env bash
# FrameLens — Web (WebAssembly / WebGL2) ビルド
# 必要: Emscripten (emcc)。ネイティブは従来どおり CMake でビルド。
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p web
emcc -std=c++17 -O2 -I src \
  src/main.cpp src/framing.cpp \
  -sUSE_GLFW=3 \
  -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2 -sFULL_ES3=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sGL_ASSERTIONS=1 -sASSERTIONS=1 \
  --preload-file house.json \
  --shell-file web/shell.html \
  -o web/framelens.html

echo "✅ built web/framelens.html"
