cmake_minimum_required(VERSION 3.16)
project(XYZOptimizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CRT estatico: o .exe roda em qualquer Windows sem instalar o Visual C++ Redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(XYZOptimizer WIN32
    src/main.cpp
    src/tweaks.cpp
    src/app.manifest
)

target_compile_definitions(XYZOptimizer PRIVATE
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

target_link_libraries(XYZOptimizer PRIVATE
    gdiplus dwmapi shell32 ole32 advapi32 user32 gdi32
)

if(MSVC)
    target_compile_options(XYZOptimizer PRIVATE /utf-8 /EHsc /W3)
else()
    target_compile_options(XYZOptimizer PRIVATE -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_link_options(XYZOptimizer PRIVATE -municode -mwindows -static)
endif()
