include(FetchContent)

# ViGEmClient (MIT, versión fija): mandos virtuales de Xbox en el host. Solo funciona con el driver ViGEmBus
# instalado; sin él, LanCast lo detecta y lo explica. Se compila solo el .cpp (sin su recurso de DLL).
FetchContent_Declare(vigemclient
    URL https://github.com/nefarius/ViGEmClient/archive/refs/tags/v1.21.222.0.zip
    URL_HASH SHA256=4c61f0a82ea1153e271fe760712a97f4edaf64ad5f4244aca78eead74a3749b9
    SOURCE_SUBDIR no-cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(vigemclient)
add_library(vigem_client STATIC ${vigemclient_SOURCE_DIR}/src/ViGEmClient.cpp)
target_include_directories(vigem_client SYSTEM PUBLIC ${vigemclient_SOURCE_DIR}/include)
target_link_libraries(vigem_client PRIVATE setupapi)
target_compile_options(vigem_client PRIVATE /W0)

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
    src/updater.cpp
    src/desktop.cpp
    src/service.cpp
    src/crash.cpp
    src/signature.cpp
    src/color.cpp
    src/gamepad.cpp)
add_library(lancast::platform ALIAS lancast_platform)

target_include_directories(lancast_platform PUBLIC include PRIVATE src)
# SDK públicos (MIT) de AMD AMF y NVIDIA NVENC: solo encabezados; los runtimes vienen con los drivers.
target_include_directories(lancast_platform SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/amf
    ${PROJECT_SOURCE_DIR}/third_party/nvenc)
target_link_libraries(lancast_platform
    PUBLIC lancast::core
    PRIVATE dxgi d3d11 dxguid mfplat mfuuid mf shcore user32 ole32 oleaut32 bcrypt crypt32 ws2_32 iphlpapi
            ole32 uuid shell32 advapi32 gdi32 userenv wtsapi32 wintrust d3dcompiler vigem_client)
lancast_set_warnings(lancast_platform)
