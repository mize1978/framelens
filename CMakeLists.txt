cmake_minimum_required(VERSION 3.16)
project(FrameLens LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(framelens
    src/main.cpp
    src/framing.cpp
)

# --- 依存: OpenGL / GLFW / GLEW ---
find_package(OpenGL REQUIRED)

# GLFW: パッケージ設定 (glfw3Config) を優先。無ければ pkg-config へフォールバック。
find_package(glfw3 QUIET)
if(NOT glfw3_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW REQUIRED glfw3)
endif()

find_package(GLEW REQUIRED)

target_link_libraries(framelens PRIVATE OpenGL::GL GLEW::GLEW)

if(glfw3_FOUND)
    target_link_libraries(framelens PRIVATE glfw)
else()
    target_include_directories(framelens PRIVATE ${GLFW_INCLUDE_DIRS})
    target_link_libraries(framelens PRIVATE ${GLFW_LIBRARIES})
endif()

if(MSVC)
    target_compile_options(framelens PRIVATE /W4 /utf-8)
else()
    target_compile_options(framelens PRIVATE -Wall -Wextra)
endif()
