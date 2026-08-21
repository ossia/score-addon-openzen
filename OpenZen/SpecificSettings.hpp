#pragma once
#include <QString>

#include <optional>
#include <verdigris>

namespace OpenZen
{

/**
 * @brief Which measurement groups the sensor should produce.
 *
 * These are not a filter on our side: each one maps to the
 * ZenImuProperty_Output* booleans that we set on the sensor itself. Turning a
 * group off shrinks the frame the sensor puts on the wire, and at a fixed baud
 * rate that is what determines the sample rate one can actually reach. Keeping
 * the defaults narrow is therefore the single most effective performance knob.
 *
 * These are groups rather than individual properties because raw and
 * calibrated values are not independently switchable across the range: on the
 * legacy firmware a single OutputRawAcc flag produces both aRaw and the
 * host-computed a, while IG1 has a separate OutputAccCalibrated. One toggle
 * per physical quantity is both what a user wants and what the hardware can
 * actually do.
 */
struct OutputSettings
{
  bool accel{true};
  bool gyro{true};
  bool mag{false};
  bool quaternion{true};
  bool euler{true};
  bool angularVelocity{false};
  bool linearAccel{false};
  bool pressure{false};
  bool altitude{false};
  bool temperature{false};
  bool heave{false};

  bool operator==(const OutputSettings&) const noexcept = default;
};

struct SpecificSettings
{
  /** OpenZen IO system key: "LinuxDevice", "WindowsDevice", "Bluetooth", ... */
  QString ioType;

  /**
   * Hardware serial number. This is the identity we prefer, because it
   * survives the device coming back on a different port after a replug.
   * May be empty when the IO system cannot report one before connecting
   * (plain COM ports on Windows), in which case we learn it after the first
   * successful connection.
   */
  QString serialNumber;

  /**
   * Port-level address: "/dev/ttyUSB0", "\\\\.\\COM3", a Bluetooth MAC.
   * Used as the fallback identity, and as the fast path even when matching by
   * serial number (we try the remembered port first, then verify).
   */
  QString identifier;

  /** 0 lets OpenZen negotiate. */
  int baudRate{0};

  /** Prefer serialNumber over identifier when both are known. */
  bool matchBySerial{true};

  /** Sensor sampling rate in Hz. 0 leaves whatever the sensor is set to. */
  int samplingRate{200};

  /** ZenImuProperty_FilterMode. -1 leaves whatever the sensor is set to. */
  int filterMode{-1};

  /** Report angles in degrees rather than radians (IG1 and newer). */
  bool degrees{true};

  OutputSettings outputs;

  /** Keep trying to (re)attach to the sensor for as long as the device exists. */
  bool autoReconnect{true};

  /**
   * How long the data stream may go silent before we treat the link as dead
   * and start reconnecting. OpenZen does not report unplugs on its own, so
   * this watchdog is our primary disconnection signal.
   */
  int watchdogMs{500};

  /** Optional score-side rate limiting, in milliseconds between updates. */
  std::optional<int> rate;
};
}

Q_DECLARE_METATYPE(OpenZen::SpecificSettings)
W_REGISTER_ARGTYPE(OpenZen::SpecificSettings)
Q_DECLARE_METATYPE(OpenZen::OutputSettings)
W_REGISTER_ARGTYPE(OpenZen::OutputSettings)
