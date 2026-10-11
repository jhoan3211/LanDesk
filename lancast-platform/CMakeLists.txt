# Capa de plataforma Windows compartida por host y cliente (DXGI, D3D11, Media Foundation, DisplayConfig).
# Separada de lancast-core para que el protocolo y los tipos sigan siendo portables.
add_library(lancast_platform STATIC
    src/com.cpp
    src/strings.cpp
    src/dpi.cpp
    src/gpu.cpp
    src/monitors.cpp
    src/encoders.cpp
    src/decoders.cpp
    src/system_report.cpp)
add_library(lancast::platform ALIAS lancast_platform)

target_include_directories(lancast_platform PUBLIC include PRIVATE src)
target_link_libraries(lancast_platform
    PUBLIC lancast::core
    PRIVATE dxgi d3d11 dxguid mfplat mfuuid shcore user32 ole32)
lancast_set_warnings(lancast_platform)
