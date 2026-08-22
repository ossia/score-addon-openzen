// Integration tests for the backend.
//
// The tests tagged [hardware] need a sensor plugged in and are skipped
// otherwise; everything else runs anywhere and covers the paths that must
// hold when no sensor is present - which is most of them, since an IMU that
// is not there is the normal state of affairs while a score is being written.
#include <OpenZen/Manager.hpp>

#include <ossia/network/context.hpp>
#include <ossia/network/context_functions.hpp>

#include <OpenZenCAPI.h>
#include <ZenTypes.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace ossia::openzen;

namespace
{
struct recorder final : sensor_listener
{
  std::atomic<int> imu_frames{0};
  std::atomic<int> gnss_frames{0};
  std::vector<link_event> events;
  capabilities caps;
  bool caps_seen{false};
  std::vector<int32_t> rates;

  void on_imu_data(const ZenImuData&) override { imu_frames++; }
  void on_gnss_data(const ZenGnssData&) override { gnss_frames++; }
  void on_link_event(const link_event& e) override
  {
    events.push_back(e);
    if(e.caps_valid)
    {
      caps = e.caps;
      caps_seen = true;
    }
    if(!e.supported_rates.empty())
      rates = e.supported_rates;
  }

  bool saw(link_state s) const
  {
    for(const auto& e : events)
      if(e.state == s)
        return true;
    return false;
  }
  link_state last() const
  {
    return events.empty() ? link_state::offline : events.back().state;
  }
};

/** Drives the network context, which is what runs the event pump. */
struct harness
{
  std::shared_ptr<ossia::net::network_context> ctx{
      ossia::net::create_network_context()};

  harness() { manager::instance().register_context(ctx->context); }
  ~harness() { manager::instance().unregister_context(ctx->context); }

  void spin(std::chrono::milliseconds d)
  {
    const auto until = std::chrono::steady_clock::now() + d;
    while(std::chrono::steady_clock::now() < until)
    {
      ossia::net::poll_network_context(*ctx);
      std::this_thread::sleep_for(1ms);
    }
  }

  //! Spin until `pred` holds or the budget runs out. Returns whether it held.
  template <typename F>
  bool spin_until(std::chrono::milliseconds budget, F&& pred)
  {
    const auto until = std::chrono::steady_clock::now() + budget;
    while(std::chrono::steady_clock::now() < until)
    {
      ossia::net::poll_network_context(*ctx);
      if(pred())
        return true;
      std::this_thread::sleep_for(2ms);
    }
    return pred();
  }
};

//! The first sensor OpenZen can see, if any.
std::optional<sensor_desc> find_a_sensor(harness& h)
{
  auto& mgr = manager::instance();
  mgr.set_scanning(true);
  sensor_list found;
  h.spin_until(20s, [&] {
    found = mgr.sensors();
    return !found.empty();
  });
  mgr.set_scanning(false);
  if(found.empty())
    return std::nullopt;
  return found.front();
}

session_config absent_sensor(std::string serial)
{
  session_config c;
  c.io_type = "LinuxDevice";
  c.serial = std::move(serial);
  c.identifier = "/dev/does-not-exist";
  c.watchdog = 300ms;
  return c;
}
}

TEST_CASE("the backend starts without hardware", "[backend]")
{
  harness h;
  CHECK_NOTHROW(manager::instance().sensors());
}

TEST_CASE("a sensor that is not there never blocks the caller", "[backend]")
{
  harness h;
  auto& mgr = manager::instance();
  recorder r;

  // acquire() must not wait on the hardware: obtainSensor() alone can take
  // several seconds, and this is reached from the GUI thread.
  const auto t0 = std::chrono::steady_clock::now();
  auto s = mgr.acquire(absent_sensor("no-such-imu-0001"), r);
  const auto acquire_time = std::chrono::steady_clock::now() - t0;

  REQUIRE(s != nullptr);
  CHECK(acquire_time < 50ms);

  h.spin_until(3s, [&] { return r.saw(link_state::searching); });
  CHECK(r.saw(link_state::searching));
  CHECK(r.imu_frames == 0);

  const auto t1 = std::chrono::steady_clock::now();
  mgr.release(s);
  CHECK(std::chrono::steady_clock::now() - t1 < 50ms);

  h.spin(300ms);
}

TEST_CASE("two devices cannot fight over one sensor", "[backend]")
{
  harness h;
  auto& mgr = manager::instance();
  recorder a, b;

  const auto cfg = absent_sensor("duplicate-0002");
  auto first = mgr.acquire(cfg, a);
  auto second = mgr.acquire(cfg, b);

  CHECK(first != nullptr);
  CHECK(second == nullptr);

  SECTION("and the sensor is available again once released")
  {
    mgr.release(first);
    h.spin(300ms);
    auto again = mgr.acquire(cfg, b);
    CHECK(again != nullptr);
    mgr.release(again);
    h.spin(300ms);
  }

  if(first)
    mgr.release(first);
  h.spin(300ms);
}

TEST_CASE("a listener that goes away is never called again", "[backend]")
{
  harness h;
  auto& mgr = manager::instance();

  {
    recorder r;
    auto s = mgr.acquire(absent_sensor("detach-0003"), r);
    REQUIRE(s != nullptr);
    h.spin(300ms);
    mgr.release(s);
    // r is destroyed here, while the backend may still hold the session.
  }

  h.spin(1s);
  SUCCEED("no use-after-free once the listener is gone");
}

TEST_CASE("many absent sensors are all serviced", "[backend]")
{
  // One backend thread is shared by every sensor, so a sensor that is
  // searching must not starve the others.
  harness h;
  auto& mgr = manager::instance();

  constexpr int N = 8;
  std::vector<recorder> recorders(N);
  std::vector<session_ptr> sessions;
  for(int i = 0; i < N; ++i)
  {
    auto s = mgr.acquire(absent_sensor("many-" + std::to_string(i)), recorders[i]);
    REQUIRE(s != nullptr);
    sessions.push_back(s);
  }

  h.spin_until(10s, [&] {
    for(const auto& r : recorders)
      if(!r.saw(link_state::searching))
        return false;
    return true;
  });

  for(int i = 0; i < N; ++i)
    CHECK(recorders[i].saw(link_state::searching));

  for(auto& s : sessions)
    mgr.release(s);
  h.spin(500ms);
}

// ---------------------------------------------------------------------------
// These need a sensor. Run with `[hardware]` to select them.
// ---------------------------------------------------------------------------

TEST_CASE("a connected sensor is listed", "[hardware]")
{
  harness h;
  auto found = find_a_sensor(h);
  if(!found)
    SKIP("no OpenZen sensor attached");

  INFO("found " << found->name << " on " << found->identifier);
  CHECK(!found->identifier.empty());
  CHECK(!found->io_type.empty());
}

TEST_CASE("a connected sensor streams and reports what it measures", "[hardware]")
{
  harness h;
  auto found = find_a_sensor(h);
  if(!found)
    SKIP("no OpenZen sensor attached");

  auto& mgr = manager::instance();
  recorder r;

  session_config cfg;
  cfg.io_type = found->io_type;
  cfg.serial = found->serial;
  cfg.identifier = found->identifier;
  // 0: probe. The listing's rate is only what the IO system assumes, not what
  // the sensor is running at.
  cfg.baud_rate = 0;
  cfg.watchdog = 1s;

  auto s = mgr.acquire(cfg, r);
  REQUIRE(s != nullptr);

  // Probing every baud rate costs a couple of seconds each.
  const bool streaming = h.spin_until(90s, [&] { return r.imu_frames > 0; });
  INFO("last state: " << to_string(r.last()));
  REQUIRE(streaming);

  SECTION("the measurements are read back from the sensor")
  {
    CHECK(r.caps_seen);
    // Every LPMS unit has at least an accelerometer.
    CHECK(r.caps.accel);
  }

  SECTION("frames keep arriving at a sensible rate")
  {
    const int before = r.imu_frames;
    h.spin(2s);
    const int rate = (r.imu_frames - before) / 2;
    INFO("~" << rate << " frames/s");
    CHECK(rate > 5);
  }

  SECTION("the link reports itself as streaming")
  {
    CHECK(r.saw(link_state::streaming));
  }

  SECTION("the rates are remembered for the settings dialog")
  {
    // The dialog has no session and cannot block on the hardware, so it asks
    // the manager for whatever the sensor said last time it connected.
    const auto remembered = manager::instance().known_rates(
        cfg.io_type, cfg.serial, cfg.identifier);
    CHECK(remembered == r.rates);

    SECTION("and an unknown sensor simply reports nothing")
    {
      CHECK(manager::instance()
                .known_rates(cfg.io_type, "no-such-serial-9999", "/dev/nope")
                .empty());
    }
  }

  SECTION("the sensor reports which sampling rates it accepts")
  {
    // A sensor NACKs any rate outside its own set, so asking for one is a
    // good way to lose a link that was working. LP-Research document
    // 5/10/50/100/250/500 Hz for the LPMS3 family; this checks the hardware
    // against that rather than trusting the datasheet.
    INFO("advertised rates: " << [&] {
      std::string s;
      for(auto v : r.rates)
        s += std::to_string(v) + " ";
      return s;
    }());

    REQUIRE(!r.rates.empty());
    CHECK(std::find(r.rates.begin(), r.rates.end(), 100) != r.rates.end());
    CHECK(std::find(r.rates.begin(), r.rates.end(), 200) == r.rates.end());
  }

  mgr.release(s);
  h.spin(1s);
}

TEST_CASE("reconnecting to the same sensor works", "[hardware]")
{
  // The port must really be released, and a second connection must find the
  // sensor again - this is the path a watchdog-triggered reconnection takes.
  harness h;
  auto found = find_a_sensor(h);
  if(!found)
    SKIP("no OpenZen sensor attached");

  auto& mgr = manager::instance();

  session_config cfg;
  cfg.io_type = found->io_type;
  cfg.serial = found->serial;
  cfg.identifier = found->identifier;
  cfg.baud_rate = 0;

  for(int attempt = 0; attempt < 2; ++attempt)
  {
    INFO("attempt " << attempt);
    recorder r;
    auto s = mgr.acquire(cfg, r);
    REQUIRE(s != nullptr);
    CHECK(h.spin_until(90s, [&] { return r.imu_frames > 0; }));
    mgr.release(s);
    h.spin(1s);
  }
}
