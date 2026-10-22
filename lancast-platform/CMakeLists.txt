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

# SpeexDSP (BSD, versión fija): cancelación de eco del micrófono del cliente. Resta del micrófono el audio del host
# que suena por los parlantes, para que no vuelva al host. Solo el cancelador (mdf) y el preprocesador.
FetchContent_Declare(speexdsp
    URL https://github.com/xiph/speexdsp/archive/refs/tags/SpeexDSP-1.2.1.zip
    URL_HASH SHA256=be16e16bcbc26875916a8eedd3b5b46cb93bef4714c49895a28f8c86b81437cf
    SOURCE_SUBDIR no-cmake
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(speexdsp)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/speexdsp/speex/speexdsp_config_types.h
    "#ifndef __SPEEX_TYPES_H__\n#define __SPEEX_TYPES_H__\n#include <stdint.h>\ntypedef int16_t spx_int16_t;\ntypedef uint16_t spx_uint16_t;\ntypedef int32_t spx_int32_t;\ntypedef uint32_t spx_uint32_t;\n#endif\n")
add_library(speexdsp STATIC
    ${speexdsp_SOURCE_DIR}/libspeexdsp/mdf.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/preprocess.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/filterbank.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/fftwrap.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/smallft.c)
target_include_directories(speexdsp SYSTEM PUBLIC ${speexdsp_SOURCE_DIR}/include ${CMAKE_CURRENT_BINARY_DIR}/speexdsp)
target_compile_definitions(speexdsp PRIVATE FLOATING_POINT USE_SMALLFT EXPORT=)
target_compile_options(speexdsp PRIVATE /W0)

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
    src/gamepad.cpp
    src/calibration.cpp
    src/video_keep_awake.cpp
    src/amf_upscaler.cpp)
add_library(lancast::platform ALIAS lancast_platform)

target_include_directories(lancast_platform PUBLIC include PRIVATE src)
# SDK públicos (MIT) de AMD AMF y NVIDIA NVENC: solo encabezados; los runtimes vienen con los drivers.
target_include_directories(lancast_platform SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/amf
    ${PROJECT_SOURCE_DIR}/third_party/nvenc)
target_link_libraries(lancast_platform
    PUBLIC lancast::core
    PRIVATE dxgi d3d11 dxguid mfplat mfuuid mf shcore user32 ole32 oleaut32 bcrypt crypt32 ws2_32 iphlpapi
            ole32 uuid shell32 advapi32 gdi32 userenv wtsapi32 wintrust d3dcompiler vigem_client speexdsp)
lancast_set_warnings(lancast_platform)
