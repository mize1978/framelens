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
  -sEXPORTED_FUNCTIONS='["_main","_framelens_command"]' \
  -sEXPORTED_RUNTIME_METHODS='["ccall"]' \
  --preload-file house.json \
  --shell-file web/shell.html \
  -o web/framelens.html

# GitHub Pages/CDN で旧 JS/WASM が混在しないよう、生成アセットに同一バージョンを付ける。
asset_version="$(shasum -a 256 web/framelens.js | awk '{print substr($1,1,12)}')"
python3 -c 'import pathlib,sys; v=sys.argv[1]; h=pathlib.Path("web/framelens.html"); j=pathlib.Path("web/framelens.js"); h.write_text(h.read_text().replace("framelens.js", f"framelens.js?v={v}")); j.write_text(j.read_text().replace("framelens.wasm", f"framelens.wasm?v={v}").replace("framelens.data", f"framelens.data?v={v}"))' "$asset_version"

echo "✅ built web/framelens.html"
