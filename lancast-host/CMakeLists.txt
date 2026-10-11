add_library(lancast_host STATIC
    src/host_service.cpp
    src/streamer.cpp)
add_library(lancast::host ALIAS lancast_host)

target_include_directories(lancast_host PUBLIC include PRIVATE src)
target_link_libraries(lancast_host PUBLIC lancast::platform PRIVATE d3d11 dxgi)
lancast_set_warnings(lancast_host)
