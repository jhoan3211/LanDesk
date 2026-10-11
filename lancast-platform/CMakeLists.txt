# Capa de plataforma Windows compartida por host y cliente.
add_library(lancast_platform STATIC
    src/com.cpp
    src/strings.cpp
    src/dpi.cpp
    src/gpu.cpp
    src/monitors.cpp
    src/encoders.cpp
    src/decoders.cpp
    src/system_report.cpp
    src/crypto.cpp
    src/net.cpp
    src/secure_channel.cpp
    src/d3d.cpp
    src/capture.cpp
    src/video_processor.cpp
    src/mf_encoder.cpp
    src/mf_decoder.cpp
    src/amf_encoder.cpp
    src/nvenc_encoder.cpp
    src/input.cpp
    src/audio.cpp
    src/system.cpp
    src/session_crypto.cpp
    src/files.cpp
    src/marker_window.cpp
    src/updater.cpp)
add_library(lancast::platform ALIAS lancast_platform)

target_include_directories(lancast_platform PUBLIC include PRIVATE src)
# SDK públicos (MIT) de AMD AMF y NVIDIA NVENC: solo encabezados; los runtimes vienen con los drivers.
target_include_directories(lancast_platform SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/amf
    ${PROJECT_SOURCE_DIR}/third_party/nvenc)
target_link_libraries(lancast_platform
    PUBLIC lancast::core
    PRIVATE dxgi d3d11 dxguid mfplat mfuuid mf shcore user32 ole32 oleaut32 bcrypt crypt32 ws2_32 iphlpapi
            ole32 uuid shell32 advapi32 gdi32)
lancast_set_warnings(lancast_platform)
