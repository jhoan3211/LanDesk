include(FetchContent)

# Dear ImGui fijado a una versión exacta; se compila dentro del ejecutable (sin DLL).
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS UNICODE _UNICODE)
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)

add_executable(lancast WIN32
    src/main.cpp
    src/app.cpp
    src/app_tray.cpp
    src/app_client.cpp
    src/app_input.cpp
    src/ui.cpp
    src/ui_pages.cpp
    src/ui_stream.cpp
    src/renderer.cpp
    src/live_stats.cpp
    src/second_window.cpp
    src/app_service.cpp
    src/agent.cpp
    res/lancast.rc
    ${PROJECT_SOURCE_DIR}/cmake/lancast.manifest)
set_target_properties(lancast PROPERTIES OUTPUT_NAME LanCast)
target_include_directories(lancast PRIVATE src)
target_link_libraries(lancast PRIVATE lancast::host lancast::client imgui d3d11 dxgi dwmapi shcore winmm shell32)
lancast_set_warnings(lancast)
