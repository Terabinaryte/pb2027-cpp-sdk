cmake_minimum_required(VERSION 3.21)

project(pb2027 LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PB2027_BUILD_TESTS "Build unit tests" ON)
option(PB2027_BUILD_EXAMPLES "Build examples" ON)
option(PB2027_ENABLE_NETWORK_TESTS "Enable tests that contact pb.ika.zip" OFF)

find_package(zstd CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ---------------------------------------------------------------------------
# codec: pure protocol layer (types + BATCH encoding + message decoding)
# ---------------------------------------------------------------------------
add_library(pb2027_codec STATIC src/codec.cpp)
add_library(pb2027::codec ALIAS pb2027_codec)
set_target_properties(pb2027_codec PROPERTIES EXPORT_NAME codec)

target_include_directories(pb2027_codec
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pb2027_codec PUBLIC zstd::libzstd)
target_compile_features(pb2027_codec PUBLIC cxx_std_23)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pb2027_codec PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(pb2027_client STATIC src/client.cpp src/ws_transport.cpp)
add_library(pb2027::client ALIAS pb2027_client)
set_target_properties(pb2027_client PROPERTIES EXPORT_NAME client)

target_include_directories(pb2027_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/websocketclient
)
target_link_libraries(pb2027_client
    PUBLIC pb2027_codec
    PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads
)
target_compile_features(pb2027_client PUBLIC cxx_std_23)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pb2027_client PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(pb2027_api STATIC src/api.cpp src/http.cpp)
add_library(pb2027::api ALIAS pb2027_api)
set_target_properties(pb2027_api PROPERTIES EXPORT_NAME api)
target_link_libraries(pb2027_api
    PUBLIC pb2027_codec
    PRIVATE OpenSSL::SSL OpenSSL::Crypto zstd::libzstd Threads::Threads
)
target_include_directories(pb2027_api PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_features(pb2027_api PUBLIC cxx_std_23)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pb2027_api PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(pb2027_viewer STATIC src/board.cpp)
add_library(pb2027::viewer ALIAS pb2027_viewer)
set_target_properties(pb2027_viewer PROPERTIES EXPORT_NAME viewer)
target_link_libraries(pb2027_viewer PUBLIC pb2027_codec)
target_compile_features(pb2027_viewer PUBLIC cxx_std_23)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pb2027_viewer PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(PB2027_BUILD_TESTS)
    enable_testing()

    add_executable(pb2027_tests tests/test_codec.cpp)
    target_link_libraries(pb2027_tests PRIVATE pb2027_codec)
    target_include_directories(pb2027_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    add_test(NAME pb2027_codec_tests COMMAND pb2027_tests)

    add_executable(pb2027_client_tests tests/test_client.cpp)
    target_link_libraries(pb2027_client_tests PRIVATE pb2027_client)
    target_include_directories(pb2027_client_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    add_executable(pb2027_board_tests tests/test_board.cpp)
    target_link_libraries(pb2027_board_tests PRIVATE pb2027_viewer)
    target_include_directories(pb2027_board_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    add_test(NAME pb2027_board_tests COMMAND pb2027_board_tests)

    add_executable(pb2027_api_tests tests/test_api.cpp)
    target_link_libraries(pb2027_api_tests PRIVATE pb2027_api)
    target_include_directories(pb2027_api_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    if(PB2027_ENABLE_NETWORK_TESTS)
        add_test(NAME pb2027_api_tests COMMAND pb2027_api_tests)
    endif()

    add_test(NAME pb2027_client_tests
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_client_test.py $<TARGET_FILE:pb2027_client_tests>
    )
    set_tests_properties(pb2027_client_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

if(PB2027_BUILD_EXAMPLES)
    add_executable(pb2027_ws_smoke tests/ws_smoke.cpp)
    target_include_directories(pb2027_ws_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/websocketclient
    )
    target_link_libraries(pb2027_ws_smoke PRIVATE
        OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads
    )
    target_compile_features(pb2027_ws_smoke PRIVATE cxx_std_23)

    add_executable(pb2027_real_probe examples/real_probe.cpp)
    target_link_libraries(pb2027_real_probe PRIVATE pb2027_client)
    target_compile_features(pb2027_real_probe PRIVATE cxx_std_23)

    add_executable(pb2027_paint_rect examples/paint_rect.cpp)
    target_link_libraries(pb2027_paint_rect PRIVATE pb2027_api pb2027_client)
    target_compile_features(pb2027_paint_rect PRIVATE cxx_std_23)

    add_executable(pb2027_paint_probe examples/paint_probe.cpp)
    target_link_libraries(pb2027_paint_probe PRIVATE pb2027_api pb2027_viewer pb2027_client)
    target_compile_features(pb2027_paint_probe PRIVATE cxx_std_23)

    add_executable(pb2027_viewer_demo examples/viewer.cpp)
    target_link_libraries(pb2027_viewer_demo PRIVATE pb2027_viewer pb2027_client)
    target_compile_features(pb2027_viewer_demo PRIVATE cxx_std_23)
endif()

# ---------------------------------------------------------------------------
# install/export (M6); kept here so the package name is stable from day one
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS pb2027_codec pb2027_client pb2027_viewer pb2027_api EXPORT pb2027Targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT pb2027Targets
    FILE pb2027Targets.cmake
    NAMESPACE pb2027::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pb2027
)

configure_package_config_file(
    cmake/pb2027Config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/pb2027Config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pb2027
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/pb2027ConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/pb2027Config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/pb2027ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pb2027
)
