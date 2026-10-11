add_library(lancast_core STATIC
    src/types.cpp
    src/bitrate.cpp
    src/json_writer.cpp
    src/report.cpp)
add_library(lancast::core ALIAS lancast_core)

target_include_directories(lancast_core PUBLIC include)
lancast_set_warnings(lancast_core)
