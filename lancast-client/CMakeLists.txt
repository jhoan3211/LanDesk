add_library(lancast_client STATIC
    src/client_session.cpp)
add_library(lancast::client ALIAS lancast_client)

target_include_directories(lancast_client PUBLIC include PRIVATE src)
target_link_libraries(lancast_client PUBLIC lancast::platform)
lancast_set_warnings(lancast_client)
