# Builds OpenZen as a static library.
#
# We deliberately do *not* add_subdirectory() OpenZen's own CMakeLists:
#  - it does add_subdirectory(external/spdlog), and score already defines an
#    `spdlog` target through libossia, so the two collide,
#  - it defaults ZEN_CSHARP / ZEN_TESTS / ZEN_EXAMPLES to ON,
#  - it installs headers and export sets we have no use for.
# Instead we compile the subset we need against score's own dependencies.

set(OPENZEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/openzen")
set(OPENZEN_GSL "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/GSL")
set(OPENZEN_EXPECTED "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/expected-lite")

foreach(dep "${OPENZEN_ROOT}/include/OpenZen.h"
            "${OPENZEN_GSL}/include/gsl/span"
            "${OPENZEN_EXPECTED}/include/nonstd/expected.hpp")
  if(NOT EXISTS "${dep}")
    message(WARNING "score-addon-openzen: missing ${dep} - run git submodule update --init --recursive")
    return()
  endif()
endforeach()

set(OPENZEN_SRCS
  "${OPENZEN_ROOT}/src/ISensorProperties.cpp"
  "${OPENZEN_ROOT}/src/LpMatrix.cpp"
  "${OPENZEN_ROOT}/src/OpenZen.cpp"
  "${OPENZEN_ROOT}/src/Sensor.cpp"
  "${OPENZEN_ROOT}/src/SensorClient.cpp"
  "${OPENZEN_ROOT}/src/SensorManager.cpp"
  "${OPENZEN_ROOT}/src/SensorProperties.cpp"

  "${OPENZEN_ROOT}/src/communication/ConnectionNegotiator.cpp"
  "${OPENZEN_ROOT}/src/communication/Modbus.cpp"
  "${OPENZEN_ROOT}/src/communication/ModbusCommunicator.cpp"
  "${OPENZEN_ROOT}/src/communication/SyncedModbusCommunicator.cpp"

  "${OPENZEN_ROOT}/src/components/ComponentFactoryManager.cpp"
  "${OPENZEN_ROOT}/src/components/GnssComponent.cpp"
  "${OPENZEN_ROOT}/src/components/ImuComponent.cpp"
  "${OPENZEN_ROOT}/src/components/ImuIg1Component.cpp"
  "${OPENZEN_ROOT}/src/components/factories/GnssComponentFactory.cpp"
  "${OPENZEN_ROOT}/src/components/factories/ImuComponentFactory.cpp"

  "${OPENZEN_ROOT}/src/io/IoManager.cpp"
  "${OPENZEN_ROOT}/src/io/can/CanManager.cpp"
  "${OPENZEN_ROOT}/src/io/interfaces/CanInterface.cpp"
  "${OPENZEN_ROOT}/src/io/interfaces/TestSensorInterface.cpp"
  "${OPENZEN_ROOT}/src/io/systems/TestSensorSystem.cpp"

  "${OPENZEN_ROOT}/src/properties/Ig1CoreProperties.cpp"
  "${OPENZEN_ROOT}/src/properties/Ig1GnssProperties.cpp"
  "${OPENZEN_ROOT}/src/properties/Ig1ImuProperties.cpp"
  "${OPENZEN_ROOT}/src/properties/LegacyCoreProperties.cpp"
  "${OPENZEN_ROOT}/src/properties/LegacyImuProperties.cpp"
)

if(WIN32)
  list(APPEND OPENZEN_SRCS
    "${OPENZEN_ROOT}/src/io/interfaces/windows/WindowsDeviceInterface.cpp"
    "${OPENZEN_ROOT}/src/io/systems/windows/EnumerateSerialPorts.cpp"
    "${OPENZEN_ROOT}/src/io/systems/windows/WindowsDeviceSystem.cpp"
    "${OPENZEN_ROOT}/src/utility/windows/FindThisModule.cpp"
    "${OPENZEN_ROOT}/src/utility/windows/WindowsDll.cpp"
  )
elseif(APPLE)
  list(APPEND OPENZEN_SRCS
    "${OPENZEN_ROOT}/src/io/interfaces/posix/PosixDeviceInterface.cpp"
    "${OPENZEN_ROOT}/src/io/systems/mac/MacDeviceSystem.cpp"
    "${OPENZEN_ROOT}/src/utility/posix/PosixDll.cpp"
  )
else()
  list(APPEND OPENZEN_SRCS
    "${OPENZEN_ROOT}/src/io/interfaces/posix/PosixDeviceInterface.cpp"
    "${OPENZEN_ROOT}/src/io/systems/linux/LinuxDeviceSystem.cpp"
    "${OPENZEN_ROOT}/src/utility/posix/PosixDll.cpp"
  )
endif()

# Classic Bluetooth (RFCOMM) support. Needs libbluetooth on Linux, IOBluetooth
# on macOS, bthprops on Windows.
set(OPENZEN_BLUETOOTH 0)
if(UNIX AND NOT APPLE)
  find_library(OPENZEN_LIBBLUETOOTH NAMES bluetooth)
  if(OPENZEN_LIBBLUETOOTH)
    set(OPENZEN_BLUETOOTH 1)
    list(APPEND OPENZEN_SRCS
      "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/linux/BTSerialPortBinding.cc"
      "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/linux/DeviceINQ.cc"
    )
  else()
    message(STATUS "score-addon-openzen: libbluetooth not found, building without Bluetooth support")
  endif()
elseif(WIN32)
  set(OPENZEN_BLUETOOTH 1)
  list(APPEND OPENZEN_SRCS
    "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/windows/BluetoothHelpers.cc"
    "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/windows/BTSerialPortBinding.cc"
    "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/windows/DeviceINQ.cc"
  )
elseif(APPLE)
  set(OPENZEN_BLUETOOTH 1)
  list(APPEND OPENZEN_SRCS
    "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/osx/BluetoothDeviceResources.mm"
    "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/osx/BluetoothWorker.mm"
    "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/osx/BTSerialPortBinding.mm"
    "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/osx/DeviceINQ.mm"
    "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/osx/pipe.c"
  )
endif()

if(OPENZEN_BLUETOOTH)
  list(APPEND OPENZEN_SRCS
    "${OPENZEN_ROOT}/src/utility/bluetooth-serial-port/Enums.cc"
    "${OPENZEN_ROOT}/src/io/bluetooth/BluetoothDeviceFinder.cpp"
    "${OPENZEN_ROOT}/src/io/bluetooth/BluetoothDeviceHandler.cpp"
    "${OPENZEN_ROOT}/src/io/systems/BluetoothSystem.cpp"
    "${OPENZEN_ROOT}/src/io/interfaces/BluetoothInterface.cpp"
  )
endif()

add_library(score_openzen STATIC ${OPENZEN_SRCS})

target_include_directories(score_openzen SYSTEM
  PUBLIC
    "${OPENZEN_ROOT}/include"
  PRIVATE
    "${OPENZEN_ROOT}/src"
    "${OPENZEN_GSL}/include"
    "${OPENZEN_EXPECTED}/include"
)

target_compile_definitions(score_openzen
  PUBLIC
    # We link OpenZen in statically, so its symbols must not be marked
    # dllimport/dllexport. OpenZen.h reads this too, hence PUBLIC.
    ZEN_API_STATIC=1
  PRIVATE
    # Compiles out the RTCM3 RTK correction sources, the only users of
    # standalone asio. GNSS position data is unaffected.
    ZEN_NO_RTK=1
    $<$<BOOL:${OPENZEN_BLUETOOTH}>:ZEN_BLUETOOTH=1>
)

# Turns on OpenZen's SPDLOG_DEBUG traces, which log every frame exchanged with
# the sensor. Invaluable when a unit will not negotiate, far too noisy
# otherwise.
option(SCORE_ADDON_OPENZEN_VERBOSE "Log every frame exchanged with the sensor" OFF)
if(SCORE_ADDON_OPENZEN_VERBOSE)
  target_compile_definitions(score_openzen PRIVATE SPDLOG_ACTIVE_LEVEL=1)
endif()

target_compile_features(score_openzen PRIVATE cxx_std_17)
set_target_properties(score_openzen PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  UNITY_BUILD OFF
)

# Third-party code: we do not want score's warning set applied to it.
if(MSVC)
  target_compile_options(score_openzen PRIVATE /w)
else()
  target_compile_options(score_openzen PRIVATE -w)
endif()

find_package(Threads REQUIRED)
target_link_libraries(score_openzen
  PRIVATE
    Threads::Threads
    spdlog::spdlog
    $<$<PLATFORM_ID:Linux>:atomic>
    $<$<PLATFORM_ID:Linux>:dl>
    $<$<PLATFORM_ID:Linux>:rt>
    $<$<BOOL:${OPENZEN_BLUETOOTH}>:${OPENZEN_LIBBLUETOOTH}>
)

if(WIN32)
  target_link_libraries(score_openzen PRIVATE ws2_32 $<$<BOOL:${OPENZEN_BLUETOOTH}>:bthprops>)
elseif(APPLE AND OPENZEN_BLUETOOTH)
  find_library(OPENZEN_FOUNDATION Foundation)
  find_library(OPENZEN_IOBLUETOOTH IOBluetooth)
  target_link_libraries(score_openzen PRIVATE ${OPENZEN_FOUNDATION} ${OPENZEN_IOBLUETOOTH} -fobjc-arc)
endif()
