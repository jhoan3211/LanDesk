add_library(lancast_core STATIC
    src/types.cpp
    src/bitrate.cpp
    src/json_writer.cpp
    src/report.cpp
    src/log.cpp
    src/encoding.cpp
    src/fec.cpp
    src/media.cpp
    src/toml.cpp
    src/settings.cpp
    src/geometry.cpp
    src/protocol.cpp)
add_library(lancast::core ALIAS lancast_core)

target_include_directories(lancast_core PUBLIC include)
lancast_set_warnings(lancast_core)
