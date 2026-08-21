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

# Smoke test for the backend: runs without a score document, and above all
# without hardware. See tests/manager_smoke.cpp.
option(SCORE_ADDON_OPENZEN_TESTS "Build the OpenZen backend smoke test" OFF)
if(SCORE_ADDON_OPENZEN_TESTS)
  add_executable(score_addon_openzen_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/manager_smoke.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/OpenZen/Manager.cpp"
  )
  target_include_directories(score_addon_openzen_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_openzen_test PRIVATE score_openzen ossia)
  target_compile_features(score_addon_openzen_test PRIVATE cxx_std_20)
endif()
