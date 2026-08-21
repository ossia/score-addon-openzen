cmake_minimum_required(VERSION 3.13 FATAL_ERROR)

if(NOT TARGET score_lib_base)
  include(ScoreExternalAddon)
endif()

project(score_addon_openzen LANGUAGES CXX)

score_common_setup()

include(openzen.cmake)
if(NOT TARGET score_openzen)
  message(STATUS "score-addon-openzen: OpenZen unavailable, skipping")
  return()
endif()

add_library(score_addon_openzen
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/Device.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/Manager.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/Protocol.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/ProtocolFactory.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/ProtocolSettingsWidget.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/SpecificSettings.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_openzen.hpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/Device.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/Manager.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/Protocol.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/ProtocolFactory.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/ProtocolSettingsWidget.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/SpecificSettingsSerialization.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_openzen.cpp"
)

target_include_directories(score_addon_openzen
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(score_addon_openzen
  PUBLIC
    score_plugin_deviceexplorer
  PRIVATE
    score_openzen
)

setup_score_plugin(score_addon_openzen)

# Tests. `test_mapping` needs nothing at all; `test_backend` runs the real
# backend, and the cases tagged [hardware] are skipped when no sensor is
# attached. `manager_smoke` is a standalone harness that prints a readable
# trace and pauses so the cable can be pulled - handy when bringing up a new
# sensor, but not something to run in CI.
option(SCORE_ADDON_OPENZEN_TESTS "Build the OpenZen tests" OFF)
if(SCORE_ADDON_OPENZEN_TESTS)
  if(NOT TARGET Catch2::Catch2WithMain)
    include(FetchContent)
    FetchContent_Declare(
      Catch2openzen
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.15.3
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2openzen)
  endif()

  add_executable(score_addon_openzen_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mapping.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_backend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/Manager.cpp"
  )
  target_include_directories(score_addon_openzen_tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_openzen_tests
    PRIVATE Catch2::Catch2WithMain score_openzen ossia)
  target_compile_features(score_addon_openzen_tests PRIVATE cxx_std_20)

  # Only the cases that need no sensor, so this is safe to run anywhere.
  add_test(NAME score_addon_openzen_tests
           COMMAND score_addon_openzen_tests "~[hardware]")

  add_executable(score_addon_openzen_smoke
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/manager_smoke.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/Manager.cpp"
  )
  target_include_directories(score_addon_openzen_smoke
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_openzen_smoke PRIVATE score_openzen ossia)
  target_compile_features(score_addon_openzen_smoke PRIVATE cxx_std_20)
endif()
