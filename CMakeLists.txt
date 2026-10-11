cmake_minimum_required(VERSION 3.25)

project(LanCast
    VERSION 1.2.0
    DESCRIPTION "Escritorio remoto y streaming de pantalla de baja latencia para LAN"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "LanCast solo compila en Windows 10/11 (D3D11, DXGI, Media Foundation).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(LANCAST_BUILD_TESTS "Compilar las pruebas unitarias" ON)
option(LANCAST_WARNINGS_AS_ERRORS "Tratar advertencias como errores" ON)

include(cmake/CompilerWarnings.cmake)

add_subdirectory(lancast-core)
add_subdirectory(lancast-platform)
add_subdirectory(lancast-host)
add_subdirectory(lancast-client)
add_subdirectory(lancast-app)

if(LANCAST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
