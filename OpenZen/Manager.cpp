#include "Manager.hpp"

#include <ossia/detail/logger.hpp>

#include <boost/asio/io_context.hpp>

#include <OpenZen.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>

#if defined(__unix__) || defined(__APPLE__)
#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ossia::openzen
{
namespace
{
using clock = std::chrono::steady_clock;

int64_t now_ns() noexcept
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             clock::now().time_since_epoch())
      .count();
}

constexpr auto k_backoff_min = std::chrono::milliseconds{250};
constexpr auto k_backoff_max = std::chrono::milliseconds{4000};
constexpr auto k_scan_interval = std::chrono::milliseconds{2000};
constexpr auto k_scan_timeout = std::chrono::seconds{20};
constexpr auto k_backend_tick = std::chrono::milliseconds{50};

//! Bounded so that a backlog can never stall the network thread.
constexpr int k_max_events_per_tick = 1024;

std::string identity_key(const session_config& c)
{
  // Serial first: it is the identity that survives a replug onto another port.
  return c.io_type + '|'
         + (!c.serial.empty() && c.match_by_serial ? c.serial : c.identifier);
}

/**
 * Explain why a port could not be opened.
 *
 * OpenZen reports every open failure as InvalidAddress, which is not much to
 * go on when the real reason is that the serial port belongs to a group the
 * user is not in - a first-run stumbling block on Linux.
 */
std::string explain_open_failure(const std::string& identifier)
{
#if defined(__unix__) || defined(__APPLE__)
  if(identifier.empty() || identifier.front() != '/')
    return {};

  if(::access(identifier.c_str(), F_OK) != 0)
    return identifier + " is gone";

  if(::access(identifier.c_str(), R_OK | W_OK) != 0)
  {
    std::string msg = "no permission to open " + identifier;
    struct ::stat st{};
    if(::stat(identifier.c_str(), &st) == 0)
    {
      if(const auto* gr = ::getgrgid(st.st_gid))
        msg += std::string{" (owned by group '"} + gr->gr_name
               + "'; add your user to it and log back in)";
    }
    return msg;
  }
#else
  (void)identifier;
#endif
  return {};
}

void copy_field(char* dst, std::size_t n, const std::string& src) noexcept
{
  const auto len = std::min(src.size(), n - 1);
  std::memcpy(dst, src.data(), len);
  dst[len] = '\0';
}
}

const char* to_string(link_state s) noexcept
{
  switch(s)
  {
    case link_state::offline:
      return "offline";
    case link_state::searching:
      return "searching";
    case link_state::connecting:
      return "connecting";
    case link_state::streaming:
      return "streaming";
    case link_state::failed:
      return "failed";
  }
  return "unknown";
}

sensor_listener::~sensor_listener() = default;

/** Backend-thread-only part of a session. */
struct session::backend_state
{
  std::optional<zen::ZenSensor> sensor;
  std::optional<zen::ZenSensorComponent> imu;
  std::optional<zen::ZenSensorComponent> gnss;

  //! 0 while not attached.
  uintptr_t sensor_handle{0};

  clock::time_point next_attempt{};
  std::chrono::milliseconds backoff{k_backoff_min};

  //! The port we last connected on successfully; tried first next time.
  std::string last_good_identifier;
};

session::session(session_config cfg)
    : m_config{std::move(cfg)}
    , m_backend{std::make_unique<backend_state>()}
{
  m_backend->last_good_identifier = m_config.identifier;
}

session::~session() = default;

void session::detach() noexcept
{
  m_listener.store(nullptr, std::memory_order_release);
}

// ---------------------------------------------------------------------------

struct manager::impl
{
  //! Carries measurement data; drained by the pump on the network thread.
  ZenClientHandle_t data_handle{};
  std::optional<zen::ZenClient> data_client;

  //! Carries device listings only; drained by the backend thread.
  ZenClientHandle_t scan_handle{};
  std::optional<zen::ZenClient> scan_client;

  // --- registry, backend thread + acquire/release ---
  std::mutex registry_mutex;
  std::vector<session_ptr> sessions;

  // --- dispatch table: backend thread -> pump ---
  using dispatch_entry = std::pair<uintptr_t, session_ptr>;
  using dispatch_table = std::vector<dispatch_entry>;
  ossia::triple_buffer<dispatch_table> dispatch{dispatch_table{}};

  struct command
  {
    enum kind_t : uint8_t
    {
      none,
      connect,
      disconnect,
      set_bool,
      set_int,
      set_float,
      exec,
      quit
    } kind{none};

    session_ptr sess;
    int property{};
    union
    {
      bool b;
      int32_t i;
      float f;
    } value{};
  };
  ossia::blocking_mpmc_queue<command> commands;

  // --- discovery ---
  std::atomic<int> scan_refcount{0};
  std::atomic<bool> scan_requested{false};
  ossia::triple_buffer<sensor_list> discovered{sensor_list{}};

  bool scan_in_progress{false};
  clock::time_point scan_started{};
  clock::time_point last_scan{};
  std::chrono::milliseconds scan_pace{k_scan_interval};
  sensor_list scan_accumulator;
  sensor_list last_results; //! backend-thread copy, for candidate resolution

  //! Consumer-side cache for sensors(); GUI thread only.
  mutable sensor_list last_published;

  // --- pump ---
  struct timer_context
  {
    boost::asio::io_context* context{};
    ossia::timer timer;
    int count{1};

    explicit timer_context(boost::asio::io_context& ctx)
        : context{&ctx}
        , timer{ctx}
    {
    }
    timer_context(timer_context&&) = default;
    timer_context& operator=(timer_context&&) = default;
  };
  std::mutex timers_mutex;
  std::vector<timer_context> timers;

  //! Pump-thread-local mirror of the dispatch table.
  dispatch_table pump_table;

  std::atomic<bool> quit{false};
  std::thread backend;

  // -----------------------------------------------------------------------
  // Pump: runs on a network context thread.
  // -----------------------------------------------------------------------
  void pump()
  {
    dispatch.consume(pump_table);

    ZenEvent ev{};
    int budget = k_max_events_per_tick;
    while(budget-- > 0 && ZenPollNextEvent(data_handle, &ev))
      dispatch_event(ev);

    // Link state changes are produced by the backend thread but delivered
    // here, so that the score side only ever hears from one thread.
    link_event le;
    for(auto& [handle, sess] : pump_table)
    {
      if(!sess)
        continue;
      while(sess->m_events.try_dequeue(le))
      {
        if(auto* l = sess->m_listener.load(std::memory_order_acquire))
          l->on_link_event(le);
      }
    }
  }

  void dispatch_event(const ZenEvent& ev) noexcept
  {
    if(ev.eventType != ZenEventType_ImuData && ev.eventType != ZenEventType_GnssData)
      return;

    // Linear scan: the table holds one entry per open sensor, so this stays
    // in one cache line for any realistic rig.
    for(auto& [handle, sess] : pump_table)
    {
      if(handle != ev.sensor.handle || !sess)
        continue;

      sess->m_last_data.store(now_ns(), std::memory_order_relaxed);

      if(auto* l = sess->m_listener.load(std::memory_order_acquire))
      {
        if(ev.eventType == ZenEventType_ImuData)
          l->on_imu_data(ev.data.imuData);
        else
          l->on_gnss_data(ev.data.gnssData);
      }
      return;
    }
  }

  // -----------------------------------------------------------------------
  // Backend thread: everything that may block.
  // -----------------------------------------------------------------------
  void backend_loop()
  {
    command cmd;
    while(!quit.load(std::memory_order_relaxed))
    {
      if(commands.wait_dequeue_timed(cmd, k_backend_tick))
      {
        if(cmd.kind == command::quit)
          break;
        handle_command(cmd);
        cmd = command{};
      }

      const auto now = clock::now();
      poll_discovery(now);
      tick_sessions(now);
    }

    shutdown_sessions();
  }

  void handle_command(const command& cmd)
  {
    if(!cmd.sess)
      return;
    auto& s = *cmd.sess;

    switch(cmd.kind)
    {
      case command::connect:
        s.m_state.store(link_state::searching);
        s.m_backend->next_attempt = clock::now();
        s.m_backend->backoff = k_backoff_min;
        emit_event(s, link_state::searching, "looking for the sensor");
        publish_dispatch();
        break;

      case command::disconnect:
        drop_sensor(s, "disconnected");
        s.m_state.store(link_state::offline);
        emit_event(s, link_state::offline, "disconnected");
        publish_dispatch();
        break;

      case command::set_bool:
        if(s.m_backend->imu)
          s.m_backend->imu->setBoolProperty(cmd.property, cmd.value.b);
        break;
      case command::set_int:
        if(s.m_backend->imu)
          s.m_backend->imu->setInt32Property(cmd.property, cmd.value.i);
        break;
      case command::set_float:
        if(s.m_backend->imu)
          s.m_backend->imu->setFloatProperty(cmd.property, cmd.value.f);
        break;
      case command::exec:
        if(s.m_backend->imu)
          s.m_backend->imu->executeProperty(cmd.property);
        break;

      default:
        break;
    }
  }

  void tick_sessions(clock::time_point now)
  {
    std::vector<session_ptr> local;
    {
      std::lock_guard _{registry_mutex};
      local = sessions;
    }

    bool anyone_searching = false;

    for(auto& sp : local)
    {
      auto& s = *sp;
      switch(s.m_state.load(std::memory_order_relaxed))
      {
        case link_state::offline:
          break;

        case link_state::streaming: {
          // OpenZen does not tell us when a cable is pulled: the IO thread
          // simply returns and the sensor goes quiet forever. Silence is
          // therefore our disconnection signal, and it also covers the cases
          // no IO error would report - a brown-out, a wedged firmware, an RF
          // dropout on Bluetooth.
          const auto silence = now_ns() - s.m_last_data.load(std::memory_order_relaxed);
          const auto limit
              = std::chrono::duration_cast<std::chrono::nanoseconds>(s.m_config.watchdog)
                    .count();
          if(silence > limit)
          {
            drop_sensor(s, "link went silent");
            s.m_state.store(link_state::searching);
            s.m_backend->backoff = k_backoff_min;
            s.m_backend->next_attempt = now;
            emit_event(s, link_state::searching, "sensor stopped responding");
            anyone_searching = true;
          }
          break;
        }

        case link_state::searching:
        case link_state::failed:
          anyone_searching = true;
          if(now >= s.m_backend->next_attempt)
            try_connect(s, now);
          break;

        case link_state::connecting:
          // try_connect is synchronous, so this is never observed here.
          break;
      }
    }

    if(anyone_searching)
      scan_requested.store(true, std::memory_order_relaxed);
  }

  /**
   * Candidate descriptors for a session, best first.
   *
   * Serial number matching is preferred, but only some IO systems report a
   * serial before connecting: on Linux the SiLabs USB serial string is right
   * there in sysfs, while a plain Windows COM port carries no identity at
   * all. For the latter we fall back to trying ports and verifying the serial
   * over the wire once connected.
   */
  std::vector<ZenSensorDesc> candidates_for(const session& s) const
  {
    const auto& c = s.m_config;
    std::vector<ZenSensorDesc> out;

    const auto make_desc = [&](const sensor_desc& d) {
      ZenSensorDesc desc{};
      copy_field(desc.name, sizeof(desc.name), d.name);
      copy_field(desc.ioType, sizeof(desc.ioType), d.io_type);
      copy_field(desc.identifier, sizeof(desc.identifier), d.identifier);
      // Linux resolves the tty from the serial on every obtain, so passing it
      // is what makes reconnection survive ttyUSB0 -> ttyUSB1.
      copy_field(desc.serialNumber, sizeof(desc.serialNumber), d.serial);
      desc.baudRate = c.baud_rate != 0 ? c.baud_rate : d.baud_rate;
      return desc;
    };

    const bool by_serial = c.match_by_serial && !c.serial.empty();

    // 1. A listing entry that advertises exactly our serial.
    if(by_serial)
    {
      for(const auto& d : last_results)
      {
        if(!c.io_type.empty() && d.io_type != c.io_type)
          continue;
        if(!d.serial.empty() && d.serial == c.serial)
          out.push_back(make_desc(d));
      }
    }

    // 2. The port we know it was on.
    const auto& preferred = s.m_backend->last_good_identifier.empty()
                                ? c.identifier
                                : s.m_backend->last_good_identifier;
    for(const auto& d : last_results)
    {
      if(!c.io_type.empty() && d.io_type != c.io_type)
        continue;
      if(d.identifier == preferred)
      {
        auto desc = make_desc(d);
        if(by_serial && d.serial.empty())
          copy_field(desc.serialNumber, sizeof(desc.serialNumber), std::string{});
        out.push_back(desc);
      }
    }

    // 3. Matching by serial on an IO system that cannot advertise one: sweep
    //    the remaining ports and verify over the wire.
    if(by_serial)
    {
      for(const auto& d : last_results)
      {
        if(!c.io_type.empty() && d.io_type != c.io_type)
          continue;
        if(!d.serial.empty())
          continue; // already covered by step 1
        if(d.identifier == preferred)
          continue; // already covered by step 2
        auto desc = make_desc(d);
        copy_field(desc.serialNumber, sizeof(desc.serialNumber), std::string{});
        out.push_back(desc);
      }
    }

    return out;
  }

  void try_connect(session& s, clock::time_point now)
  {
    auto& b = *s.m_backend;
    auto candidates = candidates_for(s);

    if(candidates.empty())
    {
      if(s.m_state.load() != link_state::searching)
      {
        s.m_state.store(link_state::searching);
        emit_event(
            s, link_state::searching,
            s.m_config.serial.empty()
                ? "waiting for " + s.m_config.identifier
                : "waiting for sensor " + s.m_config.serial);
      }
      b.next_attempt = now + k_scan_interval;
      return;
    }

    s.m_state.store(link_state::connecting);
    emit_event(s, link_state::connecting, "connecting");

    for(const auto& desc : candidates)
    {
      auto [err, sensor] = data_client->obtainSensor(desc);
      if(err != ZenSensorInitError_None)
        continue;

      // The IO system did not advertise a serial, so check the one the sensor
      // reports before we accept this port as ours.
      if(s.m_config.match_by_serial && !s.m_config.serial.empty()
         && desc.serialNumber[0] == '\0')
      {
        auto [perr, serial] = sensor.getStringProperty(ZenSensorProperty_SerialNumber);
        if(perr == ZenError_None && !serial.empty() && serial != s.m_config.serial)
          continue; // ~ZenSensor releases it
      }

      if(!configure(s, sensor, desc))
        continue;

      b.last_good_identifier = desc.identifier;
      b.backoff = k_backoff_min;
      s.m_last_data.store(now_ns(), std::memory_order_relaxed);
      s.m_state.store(link_state::streaming);
      publish_dispatch();
      return;
    }

    b.backoff = std::min(b.backoff * 2, k_backoff_max);
    b.next_attempt = now + b.backoff;
    s.m_state.store(link_state::failed);

    std::string why;
    for(const auto& desc : candidates)
    {
      why = explain_open_failure(desc.identifier);
      if(!why.empty())
        break;
    }
    emit_event(
        s, link_state::failed,
        why.empty() ? std::string{"could not open the sensor"} : why);
  }

  /**
   * Enable exactly the measurement groups the document asks for.
   *
   * Which ZenImuProperty gates which field of ZenImuData depends on the
   * firmware family, so getting this wrong does not merely enable the wrong
   * value - it changes the length of the frame the sensor emits, and the
   * parser reads the following fields at the wrong offsets.
   *
   *  - Legacy (LPMS-B2, CU2, ...): one flag per quantity. OutputRawAcc yields
   *    both aRaw and the host-computed a; likewise gyro and mag.
   *  - IG1: raw and calibrated are separate properties, and the gyro exists
   *    twice. The LPMS-BE1 is the one model that reports its only gyro in the
   *    second slot, which is exactly how OpenZen's own negotiator recognises
   *    it (ConnectionNegotiator.cpp).
   *
   * The family is detected by reading a property that only IG1 knows about:
   * the legacy property table answers ZenError_UnknownProperty for it.
   */
  static void apply_outputs(
      zen::ZenSensorComponent& imu, const session_config& c, const std::string& model)
  {
    const auto set = [&](int property, bool on) { imu.setBoolProperty(property, on); };

    const bool ig1
        = imu.getBoolProperty(ZenImuProperty_OutputAccCalibrated).first == ZenError_None;

    set(ZenImuProperty_OutputQuat, c.outputs[out_quaternion]);
    set(ZenImuProperty_OutputEuler, c.outputs[out_euler]);
    set(ZenImuProperty_OutputAngularVel, c.outputs[out_angular_velocity]);
    set(ZenImuProperty_OutputLinearAcc, c.outputs[out_linear_accel]);
    set(ZenImuProperty_OutputPressure, c.outputs[out_pressure]);
    set(ZenImuProperty_OutputAltitude, c.outputs[out_altitude]);
    set(ZenImuProperty_OutputTemperature, c.outputs[out_temperature]);
    set(ZenImuProperty_OutputHeaveMotion, c.outputs[out_heave]);

    set(ZenImuProperty_OutputRawAcc, c.outputs[out_accel]);
    set(ZenImuProperty_OutputRawMag, c.outputs[out_mag]);

    if(!ig1)
    {
      set(ZenImuProperty_OutputRawGyr, c.outputs[out_gyro]);
      return;
    }

    set(ZenImuProperty_OutputAccCalibrated, c.outputs[out_accel]);
    set(ZenImuProperty_OutputMagCalib, c.outputs[out_mag]);

    const bool second_gyro_is_primary = model.find("BE1") != std::string::npos;
    const bool gyro = c.outputs[out_gyro];
    if(second_gyro_is_primary)
    {
      set(ZenImuProperty_OutputRawGyr0, false);
      set(ZenImuProperty_OutputGyr0AlignCalib, false);
      set(ZenImuProperty_OutputRawGyr1, gyro);
      set(ZenImuProperty_OutputGyr1AlignCalib, gyro);
    }
    else
    {
      set(ZenImuProperty_OutputRawGyr0, gyro);
      set(ZenImuProperty_OutputGyr0AlignCalib, gyro);
      set(ZenImuProperty_OutputRawGyr1, false);
      set(ZenImuProperty_OutputGyr1AlignCalib, false);
    }
    // Bias-calibrated gyro vectors are parsed into a scratch buffer by
    // OpenZen and never reach ZenImuData, so they are pure wire overhead.
    set(ZenImuProperty_OutputGyr0BiasCalib, false);
    set(ZenImuProperty_OutputGyr1BiasCalib, false);
  }

  /**
   * Read back what the sensor is actually going to send us.
   *
   * The same firmware split as apply_outputs: on the legacy firmware a single
   * OutputRawAcc flag produces both the raw and the host-calibrated vector,
   * while IG1 gates them separately.
   */
  static capabilities
  read_capabilities(zen::ZenSensorComponent& imu, const std::string& model, bool has_gnss)
  {
    const auto get = [&](int property) {
      auto [err, value] = imu.getBoolProperty(property);
      return err == ZenError_None && value;
    };

    capabilities c;
    c.gnss = has_gnss;
    c.quaternion = get(ZenImuProperty_OutputQuat);
    c.euler = get(ZenImuProperty_OutputEuler);
    c.angular_velocity = get(ZenImuProperty_OutputAngularVel);
    c.linear_accel = get(ZenImuProperty_OutputLinearAcc);
    c.pressure = get(ZenImuProperty_OutputPressure);
    c.altitude = get(ZenImuProperty_OutputAltitude);
    c.temperature = get(ZenImuProperty_OutputTemperature);
    c.heave = get(ZenImuProperty_OutputHeaveMotion);

    const bool ig1
        = imu.getBoolProperty(ZenImuProperty_OutputAccCalibrated).first == ZenError_None;

    c.accel_raw = get(ZenImuProperty_OutputRawAcc);
    c.mag_raw = get(ZenImuProperty_OutputRawMag);

    if(!ig1)
    {
      // One flag each, and it fills both the raw and the calibrated vector.
      c.accel = c.accel_raw;
      c.mag = c.mag_raw;
      c.gyro = c.gyro_raw = get(ZenImuProperty_OutputRawGyr);
      return c;
    }

    c.accel = get(ZenImuProperty_OutputAccCalibrated);
    c.mag = get(ZenImuProperty_OutputMagCalib);

    if(model.find("BE1") != std::string::npos)
    {
      c.gyro_raw = get(ZenImuProperty_OutputRawGyr1);
      c.gyro = get(ZenImuProperty_OutputGyr1AlignCalib);
    }
    else
    {
      c.gyro_raw = get(ZenImuProperty_OutputRawGyr0);
      c.gyro = get(ZenImuProperty_OutputGyr0AlignCalib);
    }
    return c;
  }

  /**
   * Push the whole configuration onto the sensor and start streaming.
   *
   * Applied on every (re)connection, so what the document says is always what
   * the hardware ends up doing, whatever was left in its flash. Bails out on
   * the first failure: each of these is a Modbus round-trip with a 2.5s
   * timeout, and the backend thread is shared with every other sensor.
   */
  bool configure(session& s, zen::ZenSensor& sensor, const ZenSensorDesc& desc)
  {
    auto imu = sensor.getAnyComponentOfType(g_zenSensorType_Imu);
    if(!imu)
    {
      emit_event(s, link_state::failed, "no IMU component on this sensor");
      return false;
    }

    const auto& c = s.m_config;

    // Configure while quiet: writes do not have to compete with the stream.
    if(imu->setBoolProperty(ZenImuProperty_StreamData, false) != ZenError_None)
      return false;

    // Not supported before IG1; a failure here is not fatal.
    imu->setBoolProperty(ZenImuProperty_DegRadOutput, !c.degrees);

    std::string model;
    if(auto [e, m] = sensor.getStringProperty(ZenSensorProperty_SensorModel);
       e == ZenError_None)
      model = m;

    // By default the sensor keeps whatever measurements it is set up for and
    // we simply report them; overriding is opt-in, for trading measurements
    // against bandwidth.
    if(!c.auto_outputs)
      apply_outputs(*imu, c, model);

    if(c.sampling_rate > 0)
    {
      if(imu->setInt32Property(ZenImuProperty_SamplingRate, c.sampling_rate)
         != ZenError_None)
        return false;
    }

    if(c.filter_mode >= 0)
      imu->setInt32Property(ZenImuProperty_FilterMode, c.filter_mode);

    if(imu->setBoolProperty(ZenImuProperty_StreamData, true) != ZenError_None)
      return false;

    link_event ev;
    ev.state = link_state::streaming;
    ev.message = std::string{"streaming from "} + desc.identifier;
    ev.info_valid = true;
    ev.model = model;
    if(auto [e, serial] = sensor.getStringProperty(ZenSensorProperty_SerialNumber);
       e == ZenError_None)
      ev.serial = serial;
    if(auto [e, fw] = sensor.getStringProperty(ZenSensorProperty_FirmwareInfo);
       e == ZenError_None)
      ev.firmware = fw;

    auto& b = *s.m_backend;
    b.imu.emplace(*imu);
    auto gnss = sensor.getAnyComponentOfType(g_zenSensorType_Gnss);
    if(gnss)
      b.gnss.emplace(*gnss);

    ev.caps_valid = true;
    ev.caps = read_capabilities(*imu, model, bool(gnss));
    b.sensor_handle = sensor.sensor().handle;
    b.sensor.emplace(std::move(sensor));

    s.m_events.enqueue(std::move(ev));
    return true;
  }

  void drop_sensor(session& s, const char* why)
  {
    auto& b = *s.m_backend;
    if(!b.sensor)
      return;

    ossia::logger().info("openzen: dropping sensor ({})", why);
    b.imu.reset();
    b.gnss.reset();
    b.sensor.reset(); // releases the port, joins OpenZen's IO thread
    b.sensor_handle = 0;
    publish_dispatch();
  }

  void emit_event(session& s, link_state st, std::string msg)
  {
    link_event ev;
    ev.state = st;
    ev.message = std::move(msg);
    s.m_events.enqueue(std::move(ev));
  }

  /**
   * Republish the pump's dispatch table.
   *
   * Backend thread only, without exception: triple_buffer is
   * single-producer/single-consumer, so a second producer would corrupt it.
   * acquire()/release() therefore only touch the registry and let the
   * backend thread republish when it processes their command.
   */
  void publish_dispatch()
  {
    dispatch_table t;
    {
      std::lock_guard _{registry_mutex};
      t.reserve(sessions.size());
      for(auto& s : sessions)
        t.emplace_back(s->m_backend->sensor_handle, s);
    }
    dispatch.produce(std::move(t));
  }

  // -----------------------------------------------------------------------
  // Discovery
  // -----------------------------------------------------------------------
  void poll_discovery(clock::time_point now)
  {
    if(scan_in_progress)
    {
      ZenEvent ev{};
      while(ZenPollNextEvent(scan_handle, &ev))
      {
        switch(ev.eventType)
        {
          case ZenEventType_SensorFound: {
            const auto& d = ev.data.sensorFound;
            sensor_desc sd;
            sd.name = d.name;
            sd.serial = d.serialNumber;
            sd.io_type = d.ioType;
            sd.identifier = d.identifier;
            sd.baud_rate = d.baudRate;
            scan_accumulator.push_back(std::move(sd));
            // Publish straight away rather than waiting for the whole listing.
            // OpenZen walks every IO system in one pass, and a Bluetooth
            // inquiry takes ten seconds or more; a USB sensor must not be
            // held back behind it.
            publish_discovery();
            break;
          }
          case ZenEventType_SensorListingProgress:
            if(ev.data.sensorListingProgress.complete)
              finish_scan();
            break;
          default:
            break;
        }
      }

      // A Bluetooth inquiry can wedge; do not let it block scanning forever.
      if(scan_in_progress && now - scan_started > k_scan_timeout)
        finish_scan();
      return;
    }

    const bool wanted = scan_refcount.load(std::memory_order_relaxed) > 0
                        || scan_requested.load(std::memory_order_relaxed);
    if(wanted && now - last_scan >= scan_pace)
    {
      scan_requested.store(false, std::memory_order_relaxed);
      scan_accumulator.clear();
      scan_in_progress = true;
      scan_started = now;
      ZenListSensorsAsync(scan_handle);
    }
  }

  //! Make the devices found so far visible to the rest of the application.
  void publish_discovery()
  {
    last_results = scan_accumulator;
    sensor_list copy = scan_accumulator;
    discovered.produce(std::move(copy));
  }

  void finish_scan()
  {
    scan_in_progress = false;
    const auto now = clock::now();

    // A listing that had to wait on a Bluetooth inquiry should not be
    // restarted two seconds later: pace the next one by how long this one
    // actually took.
    const auto elapsed
        = std::chrono::duration_cast<std::chrono::milliseconds>(now - scan_started);
    scan_pace = std::max(k_scan_interval, elapsed);
    last_scan = now;

    // Final and authoritative: this is the publication that also drops
    // devices which have gone away.
    publish_discovery();
    scan_accumulator.clear();
  }

  void shutdown_sessions()
  {
    std::vector<session_ptr> local;
    {
      std::lock_guard _{registry_mutex};
      local.swap(sessions);
    }
    for(auto& s : local)
      drop_sensor(*s, "shutting down");
  }
};

// ---------------------------------------------------------------------------

manager::manager()
    : m_impl{std::make_unique<impl>()}
{
  if(ZenInit(&m_impl->data_handle) != ZenError_None)
    throw std::runtime_error("OpenZen: could not create the data client");
  m_impl->data_client.emplace(m_impl->data_handle);

  if(ZenInit(&m_impl->scan_handle) != ZenError_None)
    throw std::runtime_error("OpenZen: could not create the scan client");
  m_impl->scan_client.emplace(m_impl->scan_handle);

  // Forces OpenZen's own SensorManager singleton to be constructed now, while
  // we are still constructing. Without this it would be built later, on the
  // first obtain, and would therefore be torn down *before* us at exit - at
  // which point releasing our sensors would resurrect a destroyed singleton.
  ZenListSensorsAsync(m_impl->scan_handle);
  m_impl->scan_in_progress = true;
  m_impl->scan_started = clock::now();

  m_impl->backend = std::thread{[this] { m_impl->backend_loop(); }};
}

manager::~manager()
{
  m_impl->quit.store(true, std::memory_order_relaxed);
  impl::command q;
  q.kind = impl::command::quit;
  m_impl->commands.enqueue(std::move(q));

  if(m_impl->backend.joinable())
    m_impl->backend.join();

  {
    std::lock_guard _{m_impl->timers_mutex};
    m_impl->timers.clear();
  }

  m_impl->data_client.reset();
  m_impl->scan_client.reset();
}

manager& manager::instance()
{
  static manager m;
  return m;
}

session_ptr manager::acquire(session_config cfg, sensor_listener& listener)
{
  const auto key = identity_key(cfg);

  auto s = std::make_shared<session>(std::move(cfg));
  s->m_listener.store(&listener, std::memory_order_release);

  {
    std::lock_guard _{m_impl->registry_mutex};
    for(const auto& existing : m_impl->sessions)
    {
      if(identity_key(existing->config()) == key)
        return nullptr; // already open by another device
    }
    m_impl->sessions.push_back(s);
  }

  // The dispatch table is republished by the backend thread when it picks
  // this up: triple_buffer has a single producer, and that producer is the
  // backend thread.
  impl::command c;
  c.kind = impl::command::connect;
  c.sess = s;
  m_impl->commands.enqueue(std::move(c));

  return s;
}

void manager::release(const session_ptr& s)
{
  if(!s)
    return;

  // Stop callbacks first: from here on the score side is out of the picture,
  // whatever the backend thread is still doing with this session.
  s->detach();

  {
    std::lock_guard _{m_impl->registry_mutex};
    std::erase(m_impl->sessions, s);
  }

  // The port is closed by the backend thread when it gets to it. Doing it
  // here would block the caller - which may be the GUI thread - for as long
  // as it takes OpenZen to join its IO thread.
  impl::command c;
  c.kind = impl::command::disconnect;
  c.sess = s;
  m_impl->commands.enqueue(std::move(c));
}

sensor_list manager::sensors() const
{
  // consume() only writes through when the producer has published something
  // new, so the latest listing has to be kept on this side; otherwise every
  // call but the one right after a scan would report an empty list.
  m_impl->discovered.consume(m_impl->last_published);
  return m_impl->last_published;
}

void manager::set_scanning(bool enable)
{
  if(enable)
  {
    m_impl->scan_refcount.fetch_add(1, std::memory_order_relaxed);
    m_impl->scan_requested.store(true, std::memory_order_relaxed);
  }
  else
  {
    m_impl->scan_refcount.fetch_sub(1, std::memory_order_relaxed);
  }
}

void manager::request_scan()
{
  m_impl->scan_requested.store(true, std::memory_order_relaxed);
}

void manager::register_context(boost::asio::io_context& ctx)
{
  std::lock_guard _{m_impl->timers_mutex};
  for(auto& t : m_impl->timers)
  {
    if(t.context == &ctx)
    {
      t.count++;
      return;
    }
  }

  auto& t = m_impl->timers.emplace_back(ctx);
  // 2ms: an IMU at 400Hz produces a sample every 2.5ms, so this keeps the
  // added latency below one sample period without waking up needlessly.
  t.timer.set_delay(std::chrono::milliseconds{2});
  t.timer.start([impl = m_impl.get()] { impl->pump(); });
}

void manager::unregister_context(boost::asio::io_context& ctx)
{
  std::lock_guard _{m_impl->timers_mutex};
  for(auto it = m_impl->timers.begin(); it != m_impl->timers.end();)
  {
    if(it->context == &ctx && --it->count == 0)
      it = m_impl->timers.erase(it);
    else
      ++it;
  }
}

void manager::set_bool_property(const session_ptr& s, int property, bool value)
{
  if(!s)
    return;
  impl::command c;
  c.kind = impl::command::set_bool;
  c.sess = s;
  c.property = property;
  c.value.b = value;
  m_impl->commands.enqueue(std::move(c));
}

void manager::set_int_property(const session_ptr& s, int property, int32_t value)
{
  if(!s)
    return;
  impl::command c;
  c.kind = impl::command::set_int;
  c.sess = s;
  c.property = property;
  c.value.i = value;
  m_impl->commands.enqueue(std::move(c));
}

void manager::set_float_property(const session_ptr& s, int property, float value)
{
  if(!s)
    return;
  impl::command c;
  c.kind = impl::command::set_float;
  c.sess = s;
  c.property = property;
  c.value.f = value;
  m_impl->commands.enqueue(std::move(c));
}

void manager::execute_command(const session_ptr& s, int property)
{
  if(!s)
    return;
  impl::command c;
  c.kind = impl::command::exec;
  c.sess = s;
  c.property = property;
  m_impl->commands.enqueue(std::move(c));
}
}
