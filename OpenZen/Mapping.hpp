#pragma once
#include <OpenZen/Manager.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

/**
 * @file
 * The parts of the integration that are pure functions of their inputs:
 * axis conventions, identity, and connection ordering. Kept out of the
 * manager so they can be tested without a sensor, a network context, or
 * OpenZen itself - these are exactly the places where a silent mistake
 * produces plausible-looking but wrong numbers.
 */
namespace ossia::openzen
{

/**
 * ZenImuData carries the quaternion as (w, x, y, z).
 * ossia's orientation dataspace stores (x, y, z, w) - see euler_u::to_neutral
 * in libossia, whose neutral form puts w last.
 */
inline std::array<float, 4> to_ossia_quaternion(const float q[4]) noexcept
{
  return {q[1], q[2], q[3], q[0]};
}

/**
 * LPMS reports Euler angles as (roll, pitch, yaw).
 * ossia's euler unit is (yaw, pitch, roll) - see euler_u::array_parameters,
 * which spells it "ypr".
 */
inline std::array<float, 3> to_ossia_euler(const float r[3]) noexcept
{
  return {r[2], r[1], r[0]};
}

/**
 * How a session is identified, and therefore which sessions collide.
 *
 * Serial number first: it is the identity that survives the sensor coming
 * back on a different port.
 */
inline std::string identity_key(const session_config& c)
{
  return c.io_type + '|'
         + (!c.serial.empty() && c.match_by_serial ? c.serial : c.identifier);
}

/**
 * Baud rates to try, in order.
 *
 * The rate a unit runs at is whatever was last written to its flash, and no
 * IO system can report it before connecting: OpenZen assumes a default and
 * gives up when that is wrong. 921600 is the LPMS factory default while
 * OpenZen's Linux backend assumes 115200, so any fixed guess is wrong for
 * some sensor.
 */
inline std::vector<uint32_t>
baud_candidates(uint32_t last_good, uint32_t configured, const std::vector<uint32_t>& probe)
{
  std::vector<uint32_t> out;
  const auto add = [&](uint32_t b) {
    if(b != 0 && std::find(out.begin(), out.end(), b) == out.end())
      out.push_back(b);
  };

  add(last_good);
  add(configured);
  for(uint32_t b : probe)
    add(b);
  return out;
}

//! The order the probe list is tried in, most likely first.
inline const std::vector<uint32_t>& default_probe_bauds()
{
  static const std::vector<uint32_t> v{921600, 115200, 460800, 230400,
                                       57600,  38400,  19200,  9600};
  return v;
}

/**
 * Whether `caps` asks for anything `have` does not already cover.
 *
 * Node creation is additive - a node is never removed once it exists - so
 * this is the test for "is there anything new to create", not for equality.
 */
inline bool adds_anything(const capabilities& have, const capabilities& caps) noexcept
{
  const auto missing = [](bool h, bool c) { return c && !h; };
  return missing(have.accel, caps.accel) || missing(have.accel_raw, caps.accel_raw)
         || missing(have.gyro, caps.gyro) || missing(have.gyro_raw, caps.gyro_raw)
         || missing(have.mag, caps.mag) || missing(have.mag_raw, caps.mag_raw)
         || missing(have.quaternion, caps.quaternion)
         || missing(have.euler, caps.euler)
         || missing(have.angular_velocity, caps.angular_velocity)
         || missing(have.linear_accel, caps.linear_accel)
         || missing(have.pressure, caps.pressure)
         || missing(have.altitude, caps.altitude)
         || missing(have.temperature, caps.temperature)
         || missing(have.heave, caps.heave) || missing(have.gnss, caps.gnss);
}

//! Union of two capability sets, for the additive tree.
inline capabilities merge(const capabilities& a, const capabilities& b) noexcept
{
  capabilities c;
  c.accel = a.accel || b.accel;
  c.accel_raw = a.accel_raw || b.accel_raw;
  c.gyro = a.gyro || b.gyro;
  c.gyro_raw = a.gyro_raw || b.gyro_raw;
  c.mag = a.mag || b.mag;
  c.mag_raw = a.mag_raw || b.mag_raw;
  c.quaternion = a.quaternion || b.quaternion;
  c.euler = a.euler || b.euler;
  c.angular_velocity = a.angular_velocity || b.angular_velocity;
  c.linear_accel = a.linear_accel || b.linear_accel;
  c.pressure = a.pressure || b.pressure;
  c.altitude = a.altitude || b.altitude;
  c.temperature = a.temperature || b.temperature;
  c.heave = a.heave || b.heave;
  c.gnss = a.gnss || b.gnss;
  return c;
}
}
