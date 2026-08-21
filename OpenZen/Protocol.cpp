#include "Protocol.hpp"

#include <ossia/detail/logger.hpp>
#include <ossia/network/base/device.hpp>
#include <ossia/network/base/node.hpp>
#include <ossia/network/base/parameter.hpp>
#include <ossia/network/common/complex_type.hpp>
#include <ossia/network/generic/generic_device.hpp>

#include <OpenZen.h>

#include <cmath>
#include <stdexcept>

namespace ossia::openzen
{
namespace
{
session_config to_config(const OpenZen::SpecificSettings& s)
{
  session_config c;
  c.io_type = s.ioType.toStdString();
  c.serial = s.serialNumber.toStdString();
  c.identifier = s.identifier.toStdString();
  c.baud_rate = s.baudRate > 0 ? uint32_t(s.baudRate) : 0u;
  c.match_by_serial = s.matchBySerial;
  c.sampling_rate = s.samplingRate;
  c.filter_mode = s.filterMode;
  c.degrees = s.degrees;
  c.auto_reconnect = s.autoReconnect;
  c.watchdog = std::chrono::milliseconds{s.watchdogMs > 0 ? s.watchdogMs : 500};

  const auto& o = s.outputs;
  c.outputs[out_accel] = o.accel;
  c.outputs[out_gyro] = o.gyro;
  c.outputs[out_mag] = o.mag;
  c.outputs[out_quaternion] = o.quaternion;
  c.outputs[out_euler] = o.euler;
  c.outputs[out_angular_velocity] = o.angularVelocity;
  c.outputs[out_linear_accel] = o.linearAccel;
  c.outputs[out_pressure] = o.pressure;
  c.outputs[out_altitude] = o.altitude;
  c.outputs[out_temperature] = o.temperature;
  c.outputs[out_heave] = o.heave;
  return c;
}

ossia::net::parameter_base*
make(ossia::net::node_base& root, const char* path, const char* type)
{
  auto* p = ossia::create_parameter(root, path, type);
  if(p)
    p->set_access(ossia::access_mode::GET);
  else
    ossia::logger().error("openzen: cannot create {} as '{}'", path, type);
  return p;
}

ossia::net::parameter_base*
make_rw(ossia::net::node_base& root, const char* path, const char* type)
{
  auto* p = ossia::create_parameter(root, path, type);
  if(p)
    p->set_access(ossia::access_mode::SET);
  else
    ossia::logger().error("openzen: cannot create {} as '{}'", path, type);
  return p;
}

template <std::size_t N>
bool same(const std::array<float, N>& a, const std::array<float, N>& b) noexcept
{
  for(std::size_t i = 0; i < N; ++i)
    if(a[i] != b[i])
      return false;
  return true;
}
}

protocol::protocol(
    ossia::net::network_context_ptr ctx, const OpenZen::SpecificSettings& settings)
    : protocol_base{flags{}}
    , m_context{std::move(ctx)}
    , m_settings{settings}
{
  auto& mgr = manager::instance();

  m_session = mgr.acquire(to_config(settings), *this);
  if(!m_session)
    throw std::runtime_error("This sensor is already open by another device");

  // Joins the shared event pump. No thread is created: the pump runs on this
  // network context, which is also where ossia wants parameter updates.
  mgr.register_context(m_context->context);
}

protocol::~protocol()
{
  auto& mgr = manager::instance();
  if(m_session)
  {
    mgr.release(m_session);
    m_session.reset();
  }
  mgr.unregister_context(m_context->context);
}

bool protocol::connected() const noexcept
{
  return m_connected;
}

void protocol::stop()
{
  if(m_session)
  {
    manager::instance().release(m_session);
    m_session.reset();
  }
}

void protocol::set_device(ossia::net::device_base& dev)
{
  m_device = &dev;
  build_tree(dev.get_root_node());
  m_ready = true;

  if(m_params.status)
    m_params.status->push_value(std::string{"searching"});
  if(m_params.connected)
    m_params.connected->push_value(false);
}

void protocol::build_tree(ossia::net::node_base& root)
{
  const auto& o = m_settings.outputs;

  m_params.connected = make(root, "/connected", "bool");
  m_params.status = make(root, "/status", "string");

  m_params.model = make(root, "/info/model", "string");
  m_params.serial = make(root, "/info/serial", "string");
  m_params.firmware = make(root, "/info/firmware", "string");

  // The orientation units let score convert between quaternion, euler and
  // axis-angle representations wherever these are used.
  if(o.quaternion)
    m_params.quaternion = make(root, "/imu/quaternion", "quaternion");
  if(o.euler)
    m_params.euler = make(root, "/imu/euler", "euler");

  if(o.accel)
  {
    m_params.accel = make(root, "/imu/accel", "vec3f");
    m_params.raw_accel = make(root, "/imu/raw/accel", "vec3f");
  }
  if(o.gyro)
  {
    m_params.gyro = make(root, "/imu/gyro", "vec3f");
    m_params.raw_gyro = make(root, "/imu/raw/gyro", "vec3f");
  }
  if(o.mag)
  {
    m_params.mag = make(root, "/imu/mag", "vec3f");
    m_params.raw_mag = make(root, "/imu/raw/mag", "vec3f");
  }
  if(o.angularVelocity)
    m_params.angular_velocity = make(root, "/imu/angular_velocity", "vec3f");
  if(o.linearAccel)
    m_params.linear_accel = make(root, "/imu/linear_accel", "vec3f");
  if(o.pressure)
    m_params.pressure = make(root, "/imu/pressure", "float");
  if(o.altitude)
    m_params.altitude = make(root, "/imu/altitude", "float");
  if(o.temperature)
  {
    m_params.temperature = make(root, "/imu/temperature", "float");
    m_params.gyro_temperature = make(root, "/imu/gyro_temperature", "float");
  }
  if(o.heave)
    m_params.heave = make(root, "/imu/heave", "float");

  m_params.timestamp = make(root, "/imu/timestamp", "float");
  m_params.frame = make(root, "/imu/frame", "int");

  build_gnss_tree(root);

  m_params.ctl_streaming = make_rw(root, "/control/streaming", "bool");
  m_params.ctl_rate = make_rw(root, "/control/rate", "int");
  m_params.ctl_calibrate_gyro = make_rw(root, "/control/calibrate_gyro", "impulse");
  m_params.ctl_reset_orientation = make_rw(root, "/control/reset_orientation", "impulse");
}

void protocol::build_gnss_tree(ossia::net::node_base& root)
{
  // Always present: whether the sensor has a GNSS component is only known
  // after connecting, and the tree must not change shape when it does.
  //
  // ossia's position dataspace has no geographic unit, so latitude and
  // longitude are plain values here, as in score's GPS protocol.
  m_params.gnss_latitude = make(root, "/gnss/latitude", "float");
  m_params.gnss_longitude = make(root, "/gnss/longitude", "float");
  m_params.gnss_altitude = make(root, "/gnss/altitude", "float");
  m_params.gnss_velocity = make(root, "/gnss/velocity", "float");
  m_params.gnss_heading = make(root, "/gnss/heading", "float");
  m_params.gnss_fix = make(root, "/gnss/fix", "int");
  m_params.gnss_satellites = make(root, "/gnss/satellites", "int");
  m_params.gnss_accuracy = make(root, "/gnss/accuracy", "vec2f");
}

void protocol::emit(ossia::net::parameter_base* p, ossia::value&& v)
{
  if(p && m_device)
    m_device->apply_incoming_message({*this, 0}, *p, std::move(v));
}

template <std::size_t N>
void protocol::emit_vec(
    ossia::net::parameter_base* p, std::array<float, N>& cache,
    const std::array<float, N>& v)
{
  if(!p || same(cache, v))
    return;
  cache = v;
  emit(p, ossia::value{v});
}

void protocol::emit_float(ossia::net::parameter_base* p, float& cache, float v)
{
  if(!p || cache == v)
    return;
  cache = v;
  emit(p, ossia::value{v});
}

void protocol::on_imu_data(const ZenImuData& d)
{
  if(!m_ready)
    return;

  // ZenImuData carries the quaternion as (w, x, y, z); ossia's orientation
  // dataspace stores (x, y, z, w) - see euler_u::to_neutral in libossia.
  emit_vec<4>(
      m_params.quaternion, m_last.quaternion, {d.q[1], d.q[2], d.q[3], d.q[0]});

  // LPMS reports Euler angles as (roll, pitch, yaw); ossia's euler unit is
  // (yaw, pitch, roll).
  emit_vec<3>(m_params.euler, m_last.euler, {d.r[2], d.r[1], d.r[0]});

  emit_vec<3>(m_params.accel, m_last.accel, {d.a[0], d.a[1], d.a[2]});
  emit_vec<3>(m_params.gyro, m_last.gyro, {d.g[0], d.g[1], d.g[2]});
  emit_vec<3>(m_params.mag, m_last.mag, {d.b[0], d.b[1], d.b[2]});

  emit_vec<3>(
      m_params.raw_accel, m_last.raw_accel, {d.aRaw[0], d.aRaw[1], d.aRaw[2]});
  emit_vec<3>(m_params.raw_gyro, m_last.raw_gyro, {d.gRaw[0], d.gRaw[1], d.gRaw[2]});
  emit_vec<3>(m_params.raw_mag, m_last.raw_mag, {d.bRaw[0], d.bRaw[1], d.bRaw[2]});

  emit_vec<3>(
      m_params.angular_velocity, m_last.angular_velocity, {d.w[0], d.w[1], d.w[2]});
  emit_vec<3>(
      m_params.linear_accel, m_last.linear_accel,
      {d.linAcc[0], d.linAcc[1], d.linAcc[2]});

  emit_float(m_params.pressure, m_last.pressure, d.pressure);
  emit_float(m_params.altitude, m_last.altitude, d.altitude);
  emit_float(m_params.temperature, m_last.temperature, d.temperature);
  emit_float(m_params.gyro_temperature, m_last.gyro_temperature, d.gTemp);
  emit_float(m_params.heave, m_last.heave, d.heaveMotion);

  emit(m_params.timestamp, float(d.timestamp));
  emit(m_params.frame, int(d.frameCount));
}

void protocol::on_gnss_data(const ZenGnssData& d)
{
  if(!m_ready)
    return;

  emit(m_params.gnss_latitude, float(d.latitude));
  emit(m_params.gnss_longitude, float(d.longitude));
  emit(m_params.gnss_altitude, float(d.height));
  emit(m_params.gnss_velocity, float(d.velocity));
  emit(m_params.gnss_heading, float(d.headingOfVehicle));
  emit(m_params.gnss_fix, int(d.fixType));
  emit(m_params.gnss_satellites, int(d.numberSatellitesUsed));
  emit(
      m_params.gnss_accuracy,
      ossia::value{std::array<float, 2>{
          float(d.horizontalAccuracy), float(d.verticalAccuracy)}});
}

void protocol::on_link_event(const link_event& ev)
{
  if(!m_ready)
    return;

  const bool now_connected = ev.state == link_state::streaming;
  if(now_connected != m_connected)
  {
    m_connected = now_connected;
    emit(m_params.connected, now_connected);

    if(!now_connected)
    {
      // Force the next frame through the change filter: the values the
      // sensor comes back with must not be swallowed as "unchanged".
      m_last = {};
    }
    else
    {
      this->on_connection_open();
    }
  }

  emit(m_params.status, ev.message.empty() ? std::string{to_string(ev.state)}
                                           : ev.message);

  if(ev.info_valid)
  {
    emit(m_params.model, ev.model);
    emit(m_params.serial, ev.serial);
    emit(m_params.firmware, ev.firmware);
  }
}

bool protocol::pull(ossia::net::parameter_base&)
{
  return false;
}

bool protocol::push(const ossia::net::parameter_base& p, const ossia::value& v)
{
  // May be reached from the execution thread: only ever enqueue here, never
  // touch the hardware.
  if(!m_session)
    return false;

  auto& mgr = manager::instance();

  if(&p == m_params.ctl_streaming)
  {
    mgr.set_bool_property(m_session, ZenImuProperty_StreamData, ossia::convert<bool>(v));
    return true;
  }
  if(&p == m_params.ctl_rate)
  {
    mgr.set_int_property(m_session, ZenImuProperty_SamplingRate, ossia::convert<int>(v));
    return true;
  }
  if(&p == m_params.ctl_calibrate_gyro)
  {
    mgr.execute_command(m_session, ZenImuProperty_CalibrateGyro);
    return true;
  }
  if(&p == m_params.ctl_reset_orientation)
  {
    mgr.execute_command(m_session, ZenImuProperty_ResetOrientationOffset);
    return true;
  }
  return false;
}

bool protocol::push_raw(const ossia::net::full_parameter_data&)
{
  return false;
}

bool protocol::observe(ossia::net::parameter_base&, bool)
{
  return false;
}

bool protocol::update(ossia::net::node_base&)
{
  return false;
}
}
