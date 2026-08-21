#pragma once
#include <ossia/detail/lockfree_queue.hpp>
#include <ossia/detail/small_vector.hpp>
#include <ossia/detail/timer.hpp>
#include <ossia/detail/triple_buffer.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// OpenZen's own types, forward-declared so that this header stays cheap.
struct ZenImuData;
struct ZenGnssData;

namespace boost::asio
{
class io_context;
}

namespace ossia::openzen
{

/**
 * Where a sensor link currently stands.
 *
 * The important property is that a session never leaves the manager's registry
 * because of a state change: an unplugged sensor sits in `searching` and the
 * score-side node tree stays exactly as it was, cables and all.
 */
enum class link_state
{
  offline,    //! Not attempting to connect.
  searching,  //! The device is not present on any IO system.
  connecting, //! An obtain() is in flight. This blocks for seconds.
  streaming,  //! Data is flowing.
  failed      //! The last attempt failed; we will back off and retry.
};

const char* to_string(link_state) noexcept;

/** A sensor as reported by OpenZen's device listing. */
struct sensor_desc
{
  std::string name;
  std::string serial; //! Empty when the IO system cannot report one.
  std::string io_type;
  std::string identifier;
  uint32_t baud_rate{};
};
using sensor_list = std::vector<sensor_desc>;

/** Everything a link can tell the score side about itself. */
struct link_event
{
  link_state state{link_state::offline};
  std::string message;

  //! Only filled in on the transition to `streaming`.
  bool info_valid{false};
  std::string model;
  std::string serial;
  std::string firmware;
};

/**
 * Identity and configuration of one sensor link.
 *
 * `serial` is the identity we prefer, because it survives the sensor coming
 * back on a different port. `identifier` is the port-level address, used as
 * the fallback and as the fast path when we do match by serial.
 */
struct session_config
{
  std::string io_type;
  std::string serial;
  std::string identifier;
  uint32_t baud_rate{0};
  bool match_by_serial{true};

  int sampling_rate{200};
  int filter_mode{-1};
  bool degrees{true};

  bool auto_reconnect{true};
  std::chrono::milliseconds watchdog{500};

  /** Measurement groups to enable on the sensor. Indexed by output_id. */
  std::array<bool, 11> outputs{};
};

/**
 * Indices into session_config::outputs.
 *
 * One entry per physical quantity rather than per ZenImuProperty, because raw
 * and calibrated values are not independently switchable: the legacy firmware
 * derives the calibrated vector host-side from the raw one behind a single
 * flag, while IG1 exposes two. See manager::apply_outputs.
 */
enum output_id : uint8_t
{
  out_accel = 0,
  out_gyro,
  out_mag,
  out_quaternion,
  out_euler,
  out_angular_velocity,
  out_linear_accel,
  out_pressure,
  out_altitude,
  out_temperature,
  out_heave,
  out_count
};

/**
 * Receives everything that happens to one sensor.
 *
 * Every callback is invoked from the pump, which runs on the network context
 * that the listener registered with - the same thread ossia expects parameter
 * updates on. Nothing here is ever called from the backend thread.
 */
class sensor_listener
{
public:
  virtual ~sensor_listener();
  virtual void on_imu_data(const ZenImuData&) = 0;
  virtual void on_gnss_data(const ZenGnssData&) = 0;
  virtual void on_link_event(const link_event&) = 0;
};

class manager;

/**
 * The manager-side half of a sensor link.
 *
 * Held by shared_ptr so that the backend thread can keep working on a session
 * whose protocol has already gone away; the listener pointer is cleared first,
 * so late callbacks are simply dropped rather than dangling.
 */
class session
{
public:
  explicit session(session_config cfg);
  ~session();

  const session_config& config() const noexcept { return m_config; }

  /**
   * Detach the score side. After this returns no further callback will be
   * delivered, and any in-flight backend work becomes a no-op.
   */
  void detach() noexcept;

private:
  friend class manager;

  session_config m_config;

  //! Cleared by detach(); read by the pump before every dispatch.
  std::atomic<sensor_listener*> m_listener{nullptr};

  //! steady_clock nanoseconds of the last data event. Written by the pump,
  //! read by the backend thread's watchdog.
  std::atomic<int64_t> m_last_data{0};

  std::atomic<link_state> m_state{link_state::offline};

  //! Backend thread -> pump. Single producer, single consumer.
  ossia::spsc_queue<link_event> m_events;

  //! Owned by the backend thread only.
  struct backend_state;
  std::unique_ptr<backend_state> m_backend;
};

using session_ptr = std::shared_ptr<session>;

/**
 * The single shared OpenZen backend.
 *
 * Threading, in full:
 *
 *  - One backend thread, shared by every sensor. It owns all of the calls
 *    that block: obtainSensor() alone can take several seconds because
 *    OpenZen probes baud rates with a 2s IO timeout and retries. It also runs
 *    the device listing, the watchdogs and the reconnection state machines.
 *
 *  - No data thread at all. Following the joystick protocol's manager, the
 *    event queue is drained from an ossia::timer running on each registered
 *    network context, so measurement data is delivered on the very thread
 *    that ossia wants parameter updates on, and adding sensors adds no
 *    threads.
 *
 * Two ZenClients are used: one whose queue carries measurement data and is
 * drained by the pump, and one that only ever carries device-listing events
 * and is drained by the backend thread. Keeping them apart means discovery
 * never has to be routed across threads.
 */
class manager
{
public:
  static manager& instance();

  manager(const manager&) = delete;
  manager& operator=(const manager&) = delete;

  /**
   * Create a session for this configuration, or return nullptr if a session
   * already exists for the same sensor - two devices must not fight over one
   * serial port.
   */
  session_ptr acquire(session_config cfg, sensor_listener& listener);

  /** Detach and forget a session. Does not block on the hardware. */
  void release(const session_ptr&);

  /**
   * Latest device listing.
   *
   * Lock-free, but single-consumer: call it from the GUI thread only, which
   * is where DeviceEnumerator lives.
   */
  sensor_list sensors() const;

  /**
   * Ask for a device listing. Scans are also run automatically while any
   * session is searching for its sensor. `enable` keeps periodic scanning on
   * for as long as at least one caller wants it (the settings dialog does).
   */
  void set_scanning(bool enable);
  void request_scan();

  //! Registers/unregisters the pump timer on a network context, refcounted.
  void register_context(boost::asio::io_context&);
  void unregister_context(boost::asio::io_context&);

  /**
   * Queue a property write for the backend thread. Safe to call from the
   * audio thread: it only enqueues.
   */
  void set_bool_property(const session_ptr&, int property, bool value);
  void set_int_property(const session_ptr&, int property, int32_t value);
  void set_float_property(const session_ptr&, int property, float value);
  void execute_command(const session_ptr&, int property);

private:
  manager();
  ~manager();

  struct impl;
  std::unique_ptr<impl> m_impl;
};
}
