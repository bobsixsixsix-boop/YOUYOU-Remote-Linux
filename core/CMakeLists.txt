# SPDX-License-Identifier: GPL-3.0-or-later
# libyouyou_core.so = upstream's protocol stack (core/upstream, a git subtree of
# doke69/YOUYOU-Remote-ns) + the Linux media layer + the C ABI in bridge.h.
# Dependency options mirror core/upstream/CMakeLists.txt; core/upstream/external
# is filled by upstream's scripts/fetch_deps.py + patch_deps.py (hash-pinned).
cmake_minimum_required(VERSION 3.21)
project(youyou_core VERSION 0.2.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # the static dependencies end up in a shared library
set(UP "${CMAKE_CURRENT_SOURCE_DIR}/upstream")
if(NOT EXISTS "${UP}/external/libdatachannel/CMakeLists.txt")
    message(FATAL_ERROR "Run core/upstream/scripts/fetch_deps.py and patch_deps.py first")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(flag ENABLE_TESTING ENABLE_PROGRAMS MBEDTLS_FATAL_WARNINGS JSON_BuildTests JSON_Install
        ENABLE_OPENSSL ENABLE_WARNINGS_AS_ERRORS LIBSRTP_TEST_APPS INSTALL_DEPS_LIBS)
    set(${flag} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(flag NO_WEBSOCKET NO_TESTS NO_EXAMPLES USE_SYSTEM_JSON USE_MBEDTLS)
    set(${flag} ON CACHE BOOL "" FORCE)
endforeach()
# Upstream's off-Switch default: libsrtp's own crypto (60 Mbps is ~8 MB/s).
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
add_compile_definitions(MBEDTLS_USER_CONFIG_FILE="uu_mbedtls_config.h")
include_directories("${UP}/include")
add_subdirectory("${UP}/external/mbedtls" mbedtls EXCLUDE_FROM_ALL)
add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
set_target_properties(MbedTLS::MbedTLS PROPERTIES INTERFACE_LINK_LIBRARIES mbedtls
                      INTERFACE_INCLUDE_DIRECTORIES "${UP}/external/mbedtls/include")
add_library(MbedTLS::MbedCrypto ALIAS mbedcrypto)
add_library(MbedTLS::MbedX509 ALIAS mbedx509)
add_subdirectory("${UP}/external/json" json EXCLUDE_FROM_ALL)
set(nlohmann_json_DIR "${CMAKE_BINARY_DIR}/json" CACHE PATH "" FORCE)
add_subdirectory("${UP}/external/libdatachannel" libdatachannel EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYS REQUIRED IMPORTED_TARGET sdl2 opus libcurl)

# Everything but the C ABI, so the self-test can drive uu::Media directly.
add_library(yy_core OBJECT media_linux.cpp)
foreach(source protocol auth quality_rpc cpu_diagnostics background diagnostics rtc_diagnostics
        client stream_stats video_assembler strict_video_receiver transport turn_stream
        live_telemetry config_store runtime_profile)
    target_sources(yy_core PRIVATE "${UP}/source/${source}.cpp")
endforeach()
target_include_directories(yy_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(yy_core PUBLIC UU_VERSION="linux-${PROJECT_VERSION}")
target_link_libraries(yy_core PUBLIC datachannel-static nlohmann_json::nlohmann_json mbedcrypto
                      PkgConfig::SYS Threads::Threads)

add_library(youyou_core SHARED bridge.cpp)
target_link_libraries(youyou_core PRIVATE yy_core)
# Export only the C ABI; the static dependencies stay private to the library.
target_link_options(youyou_core PRIVATE -Wl,--no-undefined
                    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/youyou_core.map")
set_target_properties(youyou_core PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/youyou_core.map")

option(YY_TESTS "Build the self-tests (the decode test needs libavformat)" OFF)
if(YY_TESTS)
    pkg_check_modules(AV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
    add_executable(decode_test tests/decode_test.cpp)
    target_link_libraries(decode_test PRIVATE yy_core PkgConfig::AV)
    add_executable(protocol_test tests/protocol_test.cpp)
    target_link_libraries(protocol_test PRIVATE yy_core)
endif()
