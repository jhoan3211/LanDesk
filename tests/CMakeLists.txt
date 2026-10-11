include(FetchContent)

# GoogleTest fijado a una versión exacta. gtest_force_shared_crt alinea el runtime con el resto del proyecto.
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(lancast-core-tests
    core/bitrate_test.cpp
    core/types_test.cpp
    core/json_writer_test.cpp
    core/fec_test.cpp
    core/media_test.cpp
    core/protocol_test.cpp
    core/settings_test.cpp
    core/audio_resample_test.cpp
    core/json_reader_test.cpp)
target_link_libraries(lancast-core-tests PRIVATE lancast::core GTest::gtest_main)
lancast_set_warnings(lancast-core-tests)
gtest_discover_tests(lancast-core-tests)

# Pruebas contra el hardware real de la máquina (requieren al menos una GPU y un monitor).
add_executable(lancast-platform-tests
    platform/enumeration_test.cpp
    platform/pipeline_test.cpp
    ${PROJECT_SOURCE_DIR}/cmake/lancast.manifest)
target_link_libraries(lancast-platform-tests PRIVATE lancast::platform GTest::gtest_main d3d11 dxgi)
lancast_set_warnings(lancast-platform-tests)
gtest_discover_tests(lancast-platform-tests PROPERTIES LABELS hardware)

# Extremo a extremo: host + cliente reales sobre loopback.
add_executable(lancast-e2e-tests
    e2e/session_test.cpp
    ${PROJECT_SOURCE_DIR}/cmake/lancast.manifest)
target_link_libraries(lancast-e2e-tests PRIVATE lancast::host lancast::client GTest::gtest_main d3d11 dxgi)
lancast_set_warnings(lancast-e2e-tests)
gtest_discover_tests(lancast-e2e-tests PROPERTIES LABELS hardware DISCOVERY_TIMEOUT 30)
