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
# libssl (in addition to libcrypto) for the TLS transports (RTMPS/RTMPTS) via
# boost::asio::ssl. libcrypto alone -- used by the RTMPE/DH handshake -- is not enough.
pkg_check_modules(SSL REQUIRED libssl)

if(APPLE)
  find_library(CORE_AUDIO CoreAudio REQUIRED)
  find_library(CORE_SERVICES CoreServices REQUIRED)
  find_library(AUDIO_UNIT AudioUnit REQUIRED)
  find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
  find_library(FOUNDATION Foundation REQUIRED)
  find_library(APPKIT AppKit REQUIRED)
endif()

# -Wno-deprecated-* only silences Boost 1.76 / libc++ C++23 header internals.
# Optimization/-g come from CMAKE_BUILD_TYPE (see the default set above).
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wstrict-aliasing=2 -Wno-write-strings -Wno-deprecated-declarations")

# Warnings as errors, off by default so a toolchain with a new diagnostic does not
# break someone's build; CI and the modernization work should turn it on.
# Sanitizers: -DSANITIZE=address|thread|undefined (or a comma-separated list).
# The build-asan/ and build-tsan/ trees were hand-configured with raw
# -DCMAKE_CXX_FLAGS before this existed.
set(SANITIZE "" CACHE STRING "Sanitizers to enable (address, thread, undefined)")
if(SANITIZE)
  add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${SANITIZE})
endif()

option(WERROR "Treat warnings as errors" OFF)
if(WERROR)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # clang-only spellings; GCC doesn't recognise these and would warn.
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-builtins -Wno-#pragma-messages")
endif()

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/rtmfp)

# SYSTEM: third-party headers must not produce warnings we would then have to
# silence globally. Boost 1.76 alone emits ~12k -Wold-style-cast.
include_directories(SYSTEM
  ${Boost_INCLUDE_DIRS}
  ${SPEEX_INCLUDE_DIRS}
  ${CRYPTO_INCLUDE_DIRS}
  ${SSL_INCLUDE_DIRS})

link_directories(${Boost_LIBRARY_DIRS} ${SPEEX_LIBRARY_DIRS} ${CRYPTO_LIBRARY_DIRS} ${SSL_LIBRARY_DIRS})

set(sourceFiles
  admin_application.cpp
  amf0.cpp
  amf3.cpp
  authentication_manager.cpp
  av_delivery.cpp
  basic_rtmp_connection.cpp
  client_session.cpp
  config.cpp
  connect_router.cpp
  connection_registry.cpp
  crypto.cpp
  dh.cpp
  evp_dh.cpp
  flv_reader.cpp
  flv_writer.cpp
  http_connection.cpp
  io_context_pool.cpp
  logging.cpp
  main.cpp
  mixer.cpp
  random_string.cpp
  remote_relay.cpp
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
  netstream_stats_registry.cpp
  rtmp_app_manager.cpp
  rtmp_application.cpp
  rtmp_connection.cpp
  rtmp_handshake.cpp
  rtmp_handshaker.cpp
  rtmp_header.cpp
  rtmp_message.cpp
  rtmp_protocol.cpp
  rtmp_parser.cpp
  rtmp_so_message.cpp
  rtmpt_manager.cpp
  rtmpt_session.cpp
  rtmps_connection.cpp
  rtmpts_connection.cpp
  server.cpp
  so_manager.cpp
  speex_codec.cpp
  ssl_context.cpp
  qos_reporter.cpp
  stream_recorder.cpp
  stream_registry.cpp
  util.cpp
  vod_manager.cpp
  media_application.cpp
  video_call_application.cpp)

add_executable(${PROJECT_NAME} ${sourceFiles})

target_precompile_headers(${PROJECT_NAME} PRIVATE pch.h)

target_link_libraries(${PROJECT_NAME}
  ${Boost_LIBRARIES}
  ${SPEEX_LIBRARIES}
  ${CRYPTO_LIBRARIES}
  ${SSL_LIBRARIES})

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
# Testing. enable_testing() must be at top level: it is what generates
# CTestTestfile.cmake, and add_test() below lives under three different option
# blocks. Inside one of them (as it used to be) a default configure -- where that
# option is OFF -- generated no test file at all, so `ctest` found nothing and
# the tests registered under BUILD_CLIENT (ON by default) were unreachable.
# ---------------------------------------------------------------------------
enable_testing()

# macOS: Boost.Log resolves ICU at its old keg path. Tests that link Boost.Log
# need it on DYLD_FALLBACK_LIBRARY_PATH; override for a different install.
if(APPLE)
  set(FMS_TEST_DYLD_PATH "/opt/homebrew/opt/icu4c@74/lib" CACHE STRING
      "DYLD_FALLBACK_LIBRARY_PATH for tests that link Boost.Log")
endif()

# ---------------------------------------------------------------------------
# Unit tests: -DBUILD_TESTS=ON. AMF, shared objects, RTMFP, av_delivery, the send
# queue policy, app_host, result handlers, and the byte_writer bench.
# ---------------------------------------------------------------------------
option(BUILD_TESTS "Build the unit tests" OFF)
# Old spelling, kept so existing build dirs and scripts keep working.
option(BUILD_AMF_TESTS "Deprecated alias for BUILD_TESTS" OFF)
if(BUILD_AMF_TESTS)
  set(BUILD_TESTS ON)
endif()
if(BUILD_TESTS)
  # The AMF serializers use Boost only for headers, so tests need Boost includes
  # but link no Boost libs — avoids dragging in Boost.Log/ICU.
  add_executable(amf_tests
    test/test_main.cpp test/amf0_test.cpp test/amf3_test.cpp test/media_path_test.cpp
    test/url_test.cpp
    amf0.cpp amf3.cpp util.cpp)
  add_test(NAME amf_tests COMMAND amf_tests)

  # so_manager unit tests: the shared-object state machine + fan-out, driven
  # through an injected recording delivery sink (no server / rtmp_application).
  add_executable(so_tests
    test/test_main.cpp test/so_test.cpp
    so_manager.cpp rtmp_so_message.cpp amf0.cpp amf3.cpp)
  add_test(NAME so_tests COMMAND so_tests)

  # RTMFP byte-level codec tests (option / option_list / header). Like the AMF
  # tests, these link Boost headers only — the codecs pull in no aes/crypto/session.
  add_executable(rtmfp_tests
    test/test_main.cpp test/rtmfp_test.cpp test/rtmfp_chunk_test.cpp test/rtmfp_cookie_test.cpp
    test/rtmfp_flow_test.cpp test/rtmfp_session_crypto_test.cpp test/rtmfp_congestion_test.cpp
    rtmfp/types.cpp rtmfp/header.cpp rtmfp/chunk.cpp rtmfp/flow.cpp
    rtmfp/parser.cpp rtmfp/aes.cpp)   # session-crypto test needs the checksum + aes helpers
  target_link_libraries(rtmfp_tests ${CRYPTO_LIBRARIES})   # cookie HMAC (rtmfp/cookie.h) + aes/HMAC
  add_test(NAME rtmfp_tests COMMAND rtmfp_tests)

  # av_delivery unit tests: the live A/V fan-out (keyframe join, config priming,
  # timestamp rebasing, metadata) driven through a recording media_host -- no
  # server, socket, or io_context (the payoff of injecting media_host).
  add_executable(av_delivery_test
    test/test_main.cpp test/av_delivery_test.cpp
    av_delivery.cpp stream_registry.cpp stream_recorder.cpp rtmp_message.cpp rtmp_header.cpp
    rtmp_protocol.cpp rtmp_so_message.cpp client_session.cpp amf0.cpp amf3.cpp flv_writer.cpp)
  add_test(NAME av_delivery_test COMMAND av_delivery_test)

  # Outbound-queue backpressure policy: message classification + the two-pass
  # shed that keeps a slow consumer from growing a send queue without bound.
  # Header-only policy, so only the message types come along.
  add_executable(send_queue_policy_test
    test/test_main.cpp test/send_queue_policy_test.cpp
    rtmp_message.cpp rtmp_header.cpp rtmp_protocol.cpp rtmp_so_message.cpp amf0.cpp amf3.cpp)
  add_test(NAME send_queue_policy_test COMMAND send_queue_policy_test)

  # rtmp_application driven through a fake app_host. Structural as much as
  # behavioural: this target links the application tier WITHOUT the transport
  # stack, which app_host is what makes possible. If a change reintroduces the
  # app -> rtmp_app_manager -> transport dependency, this stops linking.
  add_executable(app_host_test
    test/test_main.cpp test/app_host_test.cpp
    rtmp_application.cpp so_manager.cpp random_string.cpp client_session.cpp
    rtmp_message.cpp rtmp_header.cpp rtmp_protocol.cpp rtmp_so_message.cpp
    amf0.cpp amf3.cpp config.cpp logging.cpp)
  target_link_libraries(app_host_test ${Boost_LIBRARIES} ${CRYPTO_LIBRARIES})
  add_test(NAME app_host_test COMMAND app_host_test)
  if(APPLE)
    # Boost.Log wants icu4c@74 at its old keg path (as interop.sh does).
    set_tests_properties(app_host_test PROPERTIES
      ENVIRONMENT "DYLD_FALLBACK_LIBRARY_PATH=${FMS_TEST_DYLD_PATH}")
  endif()

  # result_handler_registry: the table of callbacks awaiting a peer _result, its
  # per-connection cap and its teardown purge. Templated on the handler pointer,
  # so it needs nothing but doctest.
  add_executable(result_handler_test
    test/test_main.cpp test/result_handler_test.cpp)
  target_include_directories(result_handler_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/test)
  add_test(NAME result_handler_test COMMAND result_handler_test)

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
  add_executable(fuzz_run test/fuzz_run.cpp amf0.cpp amf3.cpp)
  target_include_directories(fuzz_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(fuzz_run PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
  target_link_options(fuzz_run PRIVATE -fsanitize=address,undefined)
  add_test(NAME fuzz_run COMMAND fuzz_run)
  # 3M iterations under ASan+UBSan; ctest's default is 1500s.
  set_tests_properties(fuzz_run PROPERTIES TIMEOUT 3600)

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
    amf0.cpp amf3.cpp crypto.cpp flv_reader.cpp flv_writer.cpp rtmp_handshake.cpp rtmp_header.cpp
    rtmp_message.cpp rtmp_protocol.cpp rtmp_parser.cpp rtmp_so_message.cpp util.cpp)
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

  # RTMP pull-relay: the external tool fms-m spawns as --helper-app to bridge a
  # remote origin stream into the local server (see spawn_helper).
  add_executable(fms_helper client/helper_main.cpp)
  target_link_libraries(fms_helper PRIVATE rtmp_client_lib)

  # Back-to-back test: drives the fms-m server (spawned as a subprocess) with the
  # client library and asserts on the protocol events + frames it receives.
  add_executable(b2b_test test/b2b_test.cpp test/test_main.cpp)
  target_include_directories(b2b_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(b2b_test PRIVATE rtmp_client_lib)
  target_compile_definitions(b2b_test PRIVATE FMS_SERVER_BIN="$<TARGET_FILE:${PROJECT_NAME}>")
  add_dependencies(b2b_test ${PROJECT_NAME})
  # Built, wired to the server binary, and then never registered -- so the only
  # end-to-end protocol test in the tree never ran under ctest.
  add_test(NAME b2b_test COMMAND b2b_test)
  set_tests_properties(b2b_test PROPERTIES TIMEOUT 180)
  if(APPLE)
    set_tests_properties(b2b_test PROPERTIES
      ENVIRONMENT "DYLD_FALLBACK_LIBRARY_PATH=${FMS_TEST_DYLD_PATH}")
  endif()

  # Interop matrix vs the reference clients (rtmpdump + ffmpeg): drives the real
  # server over RTMP/RTMPT and asserts both the media (ffprobe) and the RTMP
  # user-control events the client receives (rtmpdump -z). Registered only when the
  # tools are on PATH -- it degrades to a skip elsewhere (e.g. CI without them).
  find_program(RTMPDUMP_EXE rtmpdump)
  find_program(FFMPEG_EXE ffmpeg)
  find_program(FFPROBE_EXE ffprobe)
  if(RTMPDUMP_EXE AND FFMPEG_EXE AND FFPROBE_EXE)
    add_test(NAME interop
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/interop/interop.sh $<TARGET_FILE:${PROJECT_NAME}>)
    set_tests_properties(interop PROPERTIES TIMEOUT 180)
  else()
    message(STATUS "interop test disabled (needs rtmpdump + ffmpeg + ffprobe on PATH)")
  endif()

  # End-to-end RTMP relay throughput benchmark (publisher -> server -> subscriber).
  # Not a ctest; drives the spawned server at max rate and reports GiB/s.
  add_executable(bench_rtmp_throughput test/bench_rtmp_throughput.cpp)
  target_link_libraries(bench_rtmp_throughput PRIVATE rtmp_client_lib)
  target_compile_definitions(bench_rtmp_throughput PRIVATE FMS_SERVER_BIN="$<TARGET_FILE:${PROJECT_NAME}>")
  add_dependencies(bench_rtmp_throughput ${PROJECT_NAME})

  # Characterization tests for the RTMP chunk parser (rtmp_parser::parse),
  # linked against the shared media lib. Bytes in, messages out.
  # FP9 handshake crypto: pure functions over an attacker-supplied 1536-byte C1.
  add_executable(handshake_test test/handshake_test.cpp test/test_main.cpp
    rtmp_handshake.cpp crypto.cpp)
  target_include_directories(handshake_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(handshake_test PRIVATE ${CRYPTO_LIBRARIES})
  add_test(NAME handshake_test COMMAND handshake_test)

  add_executable(chunk_parser_test test/chunk_parser_test.cpp test/test_main.cpp)
  target_include_directories(chunk_parser_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(chunk_parser_test PRIVATE fms_media_client ${Boost_LIBRARIES} ${CRYPTO_LIBRARIES})
  # Was built but never registered, so it only ran if someone invoked it by hand.
  add_test(NAME chunk_parser_test COMMAND chunk_parser_test)
  if(APPLE)
    # Links Boost.Log, which wants icu4c@74 at its old keg path (as interop.sh does).
    set_tests_properties(chunk_parser_test PROPERTIES
      ENVIRONMENT "DYLD_FALLBACK_LIBRARY_PATH=${FMS_TEST_DYLD_PATH}")
  endif()
  if(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(chunk_parser_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  endif()

  # Audio-mixer PCM framing (pcm_frame.h) -- header-only, no media/speex link needed.
  add_executable(mixer_tests test/mixer_test.cpp test/test_main.cpp)
  target_include_directories(mixer_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/test)
  add_test(NAME mixer_tests COMMAND mixer_tests)
endif()
