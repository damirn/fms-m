cmake_minimum_required(VERSION 3.16)
project(fms-m CXX)

# Keep using the classic FindBoost module (Homebrew boost@1.76 ships no CMake config).
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Boost 1.76 from Homebrew (keg-only)
set(BOOST_ROOT /opt/homebrew/opt/boost@1.76)
set(Boost_NO_SYSTEM_PATHS ON)
# Code now uses std::chrono / std::filesystem; Boost.Log still needs date_time.
find_package(Boost 1.76 EXACT COMPONENTS
  date_time log log_setup program_options system thread
  REQUIRED)
message(STATUS "Boost Include: ${Boost_INCLUDE_DIRS}")
message(STATUS "Boost Libraries: ${Boost_LIBRARIES}")

add_definitions(-DBOOST_LOG_DYN_LINK)
# Silence Boost 1.76 header noise under modern clang (global bind placeholders).
add_definitions(-DBOOST_BIND_GLOBAL_PLACEHOLDERS)
# Boost 1.76 asio predates C++20's removal of std::result_of; use invoke_result.
add_definitions(-DBOOST_ASIO_HAS_STD_INVOKE_RESULT=1)

# speex (audio codec) and OpenSSL libcrypto (RTMP handshake / DH) via pkg-config
set(ENV{PKG_CONFIG_PATH} "/opt/homebrew/opt/speex/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig:/opt/homebrew/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
find_package(PkgConfig REQUIRED)
pkg_check_modules(SPEEX REQUIRED speex)
pkg_check_modules(CRYPTO REQUIRED libcrypto)

IF(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  find_library(CORE_AUDIO CoreAudio REQUIRED)
  find_library(CORE_SERVICES CoreServices REQUIRED)
  find_library(AUDIO_UNIT AudioUnit REQUIRED)
  find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
  find_library(FOUNDATION Foundation REQUIRED)
  find_library(APPKIT AppKit REQUIRED)
ENDIF()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe -g -Wall -Wno-write-strings -fno-strict-aliasing -Wno-deprecated-declarations -Wno-deprecated-builtins -Wno-#pragma-messages")

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
  node_js_proxy_application.cpp
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

IF(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  target_link_libraries(${PROJECT_NAME} dl)
ENDIF()

IF(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  target_link_libraries(${PROJECT_NAME}
    ${CORE_AUDIO}
    ${CORE_SERVICES}
    ${AUDIO_UNIT}
    ${AUDIO_TOOLBOX}
    ${FOUNDATION}
    ${APPKIT})
ENDIF()
