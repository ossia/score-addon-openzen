#pragma once
#include <OpenZen/Manager.hpp>
#include <OpenZen/SpecificSettings.hpp>

#include <ossia/network/base/protocol.hpp>
#include <ossia/network/context.hpp>
#include <ossia/network/value/value.hpp>

#include <array>

namespace ossia::net
{
class parameter_base;
}

namespace ossia::openzen
{

/**
 * One IMU, exposed as an ossia device.
 *
 * The node tree is built once, from the saved settings, and is never torn
 * down: a sensor that is unplugged goes to /connected = false and stays
 * otherwise exactly as it was, so every cable drawn against it in the score
 * survives the cable being pulled. Reattaching is the manager's job; this
 * class only follows along.
 */
class protocol final
    : public ossia::net::protocol_base
    , public sensor_listener
{
public:
  protocol(ossia::net::network_context_ptr ctx, const OpenZen::SpecificSettings& settings);
  ~protocol() override;

  // protocol_base
  void set_device(ossia::net::device_base& dev) override;
  bool pull(ossia::net::parameter_base&) override;
  bool push(const ossia::net::parameter_base&, const ossia::value& v) override;
  bool push_raw(const ossia::net::full_parameter_data&) override;
  bool observe(ossia::net::parameter_base&, bool) override;
  bool update(ossia::net::node_base&) override;
  bool connected() const noexcept override;
  void stop() override;

  // sensor_listener
  void on_imu_data(const ZenImuData&) override;
  void on_gnss_data(const ZenGnssData&) override;
  void on_link_event(const link_event&) override;

private:
  void build_tree(ossia::net::node_base& root);
  void build_gnss_tree(ossia::net::node_base& root);

  //! Deliver a value, skipping it when nothing changed. Many IMU fields carry
  //! a slow quantity inside a fast frame; not republishing them keeps the
  //! whole downstream graph from waking up for nothing.
  void emit(ossia::net::parameter_base* p, ossia::value&& v);
  template <std::size_t N>
  void emit_vec(
      ossia::net::parameter_base* p, std::array<float, N>& cache,
      const std::array<float, N>& v);
  void emit_float(ossia::net::parameter_base* p, float& cache, float v);

  ossia::net::network_context_ptr m_context;
  ossia::net::device_base* m_device{};
  OpenZen::SpecificSettings m_settings;
  session_ptr m_session;

  //! Set once the tree exists; until then callbacks have nowhere to go.
  bool m_ready{false};
  bool m_connected{false};

  struct
  {
    ossia::net::parameter_base* connected{};
    ossia::net::parameter_base* status{};

    ossia::net::parameter_base* model{};
    ossia::net::parameter_base* serial{};
    ossia::net::parameter_base* firmware{};

    ossia::net::parameter_base* quaternion{};
    ossia::net::parameter_base* euler{};
    ossia::net::parameter_base* accel{};
    ossia::net::parameter_base* gyro{};
    ossia::net::parameter_base* mag{};
    ossia::net::parameter_base* raw_accel{};
    ossia::net::parameter_base* raw_gyro{};
    ossia::net::parameter_base* raw_mag{};
    ossia::net::parameter_base* angular_velocity{};
    ossia::net::parameter_base* linear_accel{};
    ossia::net::parameter_base* pressure{};
    ossia::net::parameter_base* altitude{};
    ossia::net::parameter_base* temperature{};
    ossia::net::parameter_base* gyro_temperature{};
    ossia::net::parameter_base* heave{};
    ossia::net::parameter_base* timestamp{};
    ossia::net::parameter_base* frame{};

    ossia::net::parameter_base* gnss_latitude{};
    ossia::net::parameter_base* gnss_longitude{};
    ossia::net::parameter_base* gnss_altitude{};
    ossia::net::parameter_base* gnss_velocity{};
    ossia::net::parameter_base* gnss_heading{};
    ossia::net::parameter_base* gnss_fix{};
    ossia::net::parameter_base* gnss_satellites{};
    ossia::net::parameter_base* gnss_accuracy{};

    ossia::net::parameter_base* ctl_streaming{};
    ossia::net::parameter_base* ctl_rate{};
    ossia::net::parameter_base* ctl_calibrate_gyro{};
    ossia::net::parameter_base* ctl_reset_orientation{};
  } m_params;

  //! Last values pushed, for change detection.
  struct
  {
    std::array<float, 4> quaternion{};
    std::array<float, 3> euler{}, accel{}, gyro{}, mag{};
    std::array<float, 3> raw_accel{}, raw_gyro{}, raw_mag{};
    std::array<float, 3> angular_velocity{}, linear_accel{};
    float pressure{}, altitude{}, temperature{}, gyro_temperature{}, heave{};
  } m_last;
};
}
