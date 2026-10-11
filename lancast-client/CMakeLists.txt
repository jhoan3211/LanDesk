add_executable(lancast-client
    src/main.cpp
    ${PROJECT_SOURCE_DIR}/cmake/lancast.manifest)

target_link_libraries(lancast-client PRIVATE lancast::core lancast::platform)
lancast_set_warnings(lancast-client)
