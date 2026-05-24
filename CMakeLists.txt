cmake_minimum_required(VERSION 3.16)
project(fms-m CXX)

# Keep using the classic FindBoost module (Homebrew boost@1.76 ships no CMake config).
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

# Have FindBoost do its own library/header search instead of delegating to Boost's
# CMake config. Homebrew's boost@1.76 ships no config at all, and newer Boost's
# config (e.g. 1.90 on Ubuntu 26.04 under CMake 4.x) fails the per-component
# lookup; the direct search works consistently across all our toolchains.
set(Boost_NO_BOOST_CMAKE ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to an optimized build with debug info. Without a build type CMake adds
# no -O flag at all, so the server would otherwise build unoptimized. Applies to
# single-config generators only; multi-config ones (Xcode/VS) choose per-build.
# Pick Debug explicitly (-O0, asserts on) when you want the byte_writer guards.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
      "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()

# Code now uses std::chrono / std::filesystem; Boost.Log still needs date_time.
# Boost.System is header-only since 1.69, so it is not a linked component (newer
# distros, e.g. Ubuntu 26.04 / Boost 1.90, no longer ship the stub library).
set(BOOST_COMPONENTS date_time log log_setup program_options thread)

if(APPLE)
  # Homebrew boost@1.76 is keg-only and ships no CMake config, so point the
  # classic FindBoost module at the keg and pin the version.
  set(BOOST_ROOT /opt/homebrew/opt/boost@1.76)
  set(Boost_NO_SYSTEM_PATHS ON)
  find_package(Boost 1.76 EXACT COMPONENTS ${BOOST_COMPONENTS} REQUIRED)
else()
  # Linux and other systems: use whatever Boost the distro provides.
  find_package(Boost REQUIRED COMPONENTS ${BOOST_COMPONENTS})
endif()
message(STATUS "Boost Include: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost Libraries: ${Boost_LIBRARIES}")

add_definitions(-DBOOST_LOG_DYN_LINK)
# Silence Boost header noise under modern compilers (global bind placeholders).
add_definitions(-DBOOST_BIND_GLOBAL_PLACEHOLDERS)
# Boost.Asio <= 1.76 predates C++20's removal of std::result_of; harmless on newer.
add_definitions(-DBOOST_ASIO_HAS_STD_INVOKE_RESULT=1)
# Boost.Asio/date_time internally include deprecated Boost headers (e.g.
# boost/detail/iterator.hpp) that emit #pragma message notes GCC can't silence
# with a warning flag; this is Boost's own off-switch for those messages.
add_definitions(-DBOOST_ALLOW_DEPRECATED_HEADERS)

# speex (audio codec) and OpenSSL libcrypto (RTMP handshake / DH) via pkg-config.
if(APPLE)
  # Homebrew keg-only packages aren't on the default pkg-config search path.
  set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/opt/speex/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig:/opt/homebrew/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(SPEEX REQUIRED speex)
pkg_check_modules(CRYPTO REQUIRED libcrypto)

if(APPLE)
  find_library(CORE_AUDIO CoreAudio REQUIRED)
  find_library(CORE_SERVICES CoreServices REQUIRED)
  find_library(AUDIO_UNIT AudioUnit REQUIRED)
  find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
  find_library(FOUNDATION Foundation REQUIRED)
  find_library(APPKIT AppKit REQUIRED)
endif()

# -fno-strict-aliasing was dropped: the amf0 double type-pun (the only real
# aliasing UB) is now a std::memcpy, and the build is clean under -Wstrict-aliasing=2.
# -Wno-deprecated-* only silences Boost 1.76 / libc++ C++23 header internals.
# Optimization/-g come from CMAKE_BUILD_TYPE (see the default set above).
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe -Wall -Wno-write-strings -Wno-deprecated-declarations")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # clang-only spellings; GCC doesn't recognise these and would warn.
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-builtins -Wno-#pragma-messages")
endif()

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/rtmfp
  ${Boost_INCLUDE_DIRS}
  ${SPEEX_INCLUDE_DIRS}
  ${CRYPTO_INCLUDE_DIRS})

link_directories(${Boost_LIBRARY_DIRS} ${SPEEX_LIBRARY_DIRS} ${CRYPTO_LIBRARY_DIRS})

set(sourceFiles
  admin_application.cpp
  amf0.cpp
  amf3.cpp
  authentication_manager.cpp
  basic_rtmp_connection.cpp
  client_session.cpp
  config.cpp
  crypto.cpp
  dh.cpp
  evp_dh.cpp
  flv_reader.cpp
  flv_writer.cpp
  g711_codec.cpp
  http_connection.cpp
  io_context_pool.cpp
  logging.cpp
  main.cpp
  mixer.cpp
  random_string.cpp
  rtmfp/aes.cpp
  rtmfp/chunk.cpp
  rtmfp/dh2.cpp
  rtmfp/flow.cpp
  rtmfp/header.cpp
  rtmfp/parser.cpp
  rtmfp/serializer.cpp
  rtmfp/service.cpp
  rtmfp/session.cpp
  rtmfp/types.cpp
  rtmp_app_manager.cpp
  rtmp_application.cpp
  rtmp_connection.cpp
  rtmp_header.cpp
  rtmp_message.cpp
  rtmp_protocol.cpp
  rtmp_raw_data.cpp
  rtmp_so_message.cpp
  rtmpt_manager.cpp
  rtmpt_session.cpp
  server.cpp
  so_manager.cpp
  speex_codec.cpp
  util.cpp
  video_bcast_application.cpp
  video_call_application.cpp)

add_executable(${PROJECT_NAME} ${sourceFiles})

target_precompile_headers(${PROJECT_NAME} PRIVATE pch.h)

target_link_libraries(${PROJECT_NAME}
  ${Boost_LIBRARIES}
  ${SPEEX_LIBRARIES}
  ${CRYPTO_LIBRARIES})

if(APPLE)
  target_link_libraries(${PROJECT_NAME}
    ${CORE_AUDIO}
    ${CORE_SERVICES}
    ${AUDIO_UNIT}
    ${AUDIO_TOOLBOX}
    ${FOUNDATION}
    ${APPKIT})
else()
  # Linux: dlopen for auth plugins, pthread for Boost.Thread / std::thread.
  find_package(Threads REQUIRED)
  target_link_libraries(${PROJECT_NAME} ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Unit tests (AMF): -DBUILD_AMF_TESTS=ON.  The AMF (de)serializers are the only
# TUs pulled in, so tests build/run without the audio/RTMP/RTMFP stack.
# ---------------------------------------------------------------------------
option(BUILD_AMF_TESTS "Build AMF serialization unit tests" OFF)
if(BUILD_AMF_TESTS)
  enable_testing()
  # The AMF serializers use Boost only for headers, so tests need Boost includes
  # but link no Boost libs — avoids dragging in Boost.Log/ICU.
  add_executable(amf_tests
    test/test_main.cpp test/amf0_test.cpp test/amf3_test.cpp test/media_path_test.cpp
    amf0.cpp amf3.cpp)
  if(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(amf_tests Threads::Threads)
  endif()
  add_test(NAME amf_tests COMMAND amf_tests)

  # RTMFP byte-level codec tests (option / option_list / header). Like the AMF
  # tests, these link Boost headers only — the codecs pull in no aes/crypto/session.
  add_executable(rtmfp_tests
    test/test_main.cpp test/rtmfp_test.cpp test/rtmfp_chunk_test.cpp
    rtmfp/types.cpp rtmfp/header.cpp rtmfp/chunk.cpp)
  if(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(rtmfp_tests Threads::Threads)
  endif()
  add_test(NAME rtmfp_tests COMMAND rtmfp_tests)

  # Manual throughput benchmark for byte_writer's input-buffer role (not a test).
  # Always built optimized (with asserts off) so the numbers are meaningful even
  # in an otherwise-Debug configure.
  add_executable(bench_byte_buffer test/bench_byte_buffer.cpp)
  target_include_directories(bench_byte_buffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(bench_byte_buffer PRIVATE -O2 -DNDEBUG)
endif()

# ---------------------------------------------------------------------------
# AMF fuzzers: -DBUILD_FUZZERS=ON.  The AMF decoders parse untrusted network
# input, so they get continuous fuzzing under ASan/UBSan.  Two harnesses:
#   fuzz_run  - portable, self-driving (own main + seed corpus); also registered
#               as a ctest so a normal `ctest` run exercises the decoders.
#   fuzz_amf3 - libFuzzer entry point (clang only); coverage-guided, run by hand.
# ---------------------------------------------------------------------------
option(BUILD_FUZZERS "Build the AMF fuzzers (ASan/UBSan)" OFF)
if(BUILD_FUZZERS)
  enable_testing()
  add_executable(fuzz_run test/fuzz_run.cpp amf0.cpp amf3.cpp)
  target_include_directories(fuzz_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(fuzz_run PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
  target_link_options(fuzz_run PRIVATE -fsanitize=address,undefined)
  if(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(fuzz_run Threads::Threads)
  endif()
  add_test(NAME fuzz_run COMMAND fuzz_run)

  # libFuzzer needs LLVM clang's -fsanitize=fuzzer runtime; GCC has no equivalent
  # driver and Apple's Xcode clang ("AppleClang") ships no libFuzzer runtime, so
  # gate on a real LLVM clang only (STREQUAL, not MATCHES).
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_executable(fuzz_amf3 test/fuzz_amf3.cpp amf3.cpp)
    target_include_directories(fuzz_amf3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(fuzz_amf3 PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address)
    target_link_options(fuzz_amf3 PRIVATE -fsanitize=fuzzer,address)
  endif()
endif()

# ---------------------------------------------------------------------------
# RTMP test/helper client (client/). Builds against the project's shared media
# modules via a small static lib so the two pch.h files don't collide.
# ---------------------------------------------------------------------------
option(BUILD_CLIENT "Build the RTMP client tool" ON)
if(BUILD_CLIENT)
  add_library(fms_media_client STATIC
    amf0.cpp amf3.cpp crypto.cpp flv_reader.cpp flv_writer.cpp rtmp_header.cpp
    rtmp_message.cpp rtmp_protocol.cpp rtmp_raw_data.cpp rtmp_so_message.cpp util.cpp)
  target_include_directories(fms_media_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/rtmfp)
  target_precompile_headers(fms_media_client PRIVATE pch.h)

  # Reusable RTMP client library: everything under client/ except main.cpp.
  add_library(rtmp_client_lib STATIC
    client/config.cpp client/flv_sink.cpp client/nc_event_handler.cpp
    client/net_client.cpp client/net_client_tunnel.cpp client/net_connection.cpp
    client/net_stream.cpp client/ns_event_handler.cpp)
  # client/ first so the client's own pch.h/headers win, then the project root.
  # PUBLIC so consumers (the executable) inherit the include paths.
  target_include_directories(rtmp_client_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/client ${CMAKE_CURRENT_SOURCE_DIR})
  target_precompile_headers(rtmp_client_lib PRIVATE client/pch.h)
  target_link_libraries(rtmp_client_lib PUBLIC fms_media_client ${Boost_LIBRARIES} ${CRYPTO_LIBRARIES})
  if(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(rtmp_client_lib PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  endif()

  # Thin command-line tool on top of the client library.
  add_executable(rtmp_client client/main.cpp)
  target_link_libraries(rtmp_client PRIVATE rtmp_client_lib)

  # Back-to-back test: drives the fms-m server (spawned as a subprocess) with the
  # client library and asserts on the protocol events + frames it receives.
  add_executable(b2b_test test/b2b_test.cpp test/test_main.cpp)
  target_include_directories(b2b_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(b2b_test PRIVATE rtmp_client_lib)
  target_compile_definitions(b2b_test PRIVATE FMS_SERVER_BIN="$<TARGET_FILE:${PROJECT_NAME}>")
  add_dependencies(b2b_test ${PROJECT_NAME})

  # End-to-end RTMP relay throughput benchmark (publisher -> server -> subscriber).
  # Not a ctest; drives the spawned server at max rate and reports GiB/s.
  add_executable(bench_rtmp_throughput test/bench_rtmp_throughput.cpp)
  target_link_libraries(bench_rtmp_throughput PRIVATE rtmp_client_lib)
  target_compile_definitions(bench_rtmp_throughput PRIVATE FMS_SERVER_BIN="$<TARGET_FILE:${PROJECT_NAME}>")
  add_dependencies(bench_rtmp_throughput ${PROJECT_NAME})

  # Characterization tests for the RTMP chunk parser (rtmp_raw_data::parse_data),
  # linked against the shared media lib. Bytes in, messages out.
  add_executable(chunk_parser_test test/chunk_parser_test.cpp test/test_main.cpp)
  target_include_directories(chunk_parser_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(chunk_parser_test PRIVATE fms_media_client ${Boost_LIBRARIES} ${CRYPTO_LIBRARIES})
  if(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(chunk_parser_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  endif()
endif()
