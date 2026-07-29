// gl.hpp — GL ローダの切り替え。
//   ネイティブ: GLEW (OpenGL 3.3 Core)
//   Web(Emscripten): GLES3 / WebGL2（GLEW 不要、GL 関数は Emscripten が供給）
#pragma once
#ifdef __EMSCRIPTEN__
  #include <GLES3/gl3.h>
#else
  #include <GL/glew.h>
#endif
