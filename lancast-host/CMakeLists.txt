add_executable(lancast-host
    src/main.cpp
    ${PROJECT_SOURCE_DIR}/cmake/lancast.manifest)

target_link_libraries(lancast-host PRIVATE lancast::core lancast::platform)
lancast_set_warnings(lancast-host)
