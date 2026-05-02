cmake_minimum_required(VERSION 3.16)
project(fms-m CXX)

# Keep using the classic FindBoost module (Homebrew boost@1.76 ships no CMake config).
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Code now uses std::chrono / std::filesystem; Boost.Log still needs date_time.
set(BOOST_COMPONENTS date_time log log_setup program_options system thread)

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
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe -g -Wall -Wno-write-strings -Wno-deprecated-declarations")
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
  io_service_pool.cpp
  logging.cpp
  main.cpp
  mixer.cpp
  multiplexer_application.cpp
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
