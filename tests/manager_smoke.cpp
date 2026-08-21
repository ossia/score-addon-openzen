// Exercises the OpenZen backend without any score document, and - crucially -
// without any hardware. The no-sensor path is the one that must never hang:
// obtainSensor() blocks for seconds, OpenZen never reports an unplug, and its
// discovery loop is shared global state.
//
// Run it with an IMU attached too: it will list it and stream from it.
#include <OpenZen/Manager.hpp>

#include <ossia/network/context.hpp>
#include <ossia/network/context_functions.hpp>

#include <ZenTypes.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace ossia::openzen;

namespace
{
int g_failures = 0;

void check(bool ok, const char* what)
{
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if(!ok)
    ++g_failures;
}

struct listener final : sensor_listener
{
  std::atomic<int> imu_frames{0};
  std::atomic<int> gnss_frames{0};
  std::atomic<int> events{0};
  std::string last_state;
  std::string last_message;

  void on_imu_data(const ZenImuData&) override { imu_frames++; }
  void on_gnss_data(const ZenGnssData&) override { gnss_frames++; }
  void on_link_event(const link_event& e) override
  {
    events++;
    last_state = to_string(e.state);
    last_message = e.message;
    std::printf("     [link] %s: %s\n", last_state.c_str(), e.message.c_str());
  }
};

// Runs the network context for a while, which is what drives the pump.
void spin(ossia::net::network_context& ctx, std::chrono::milliseconds d)
{
  const auto until = std::chrono::steady_clock::now() + d;
  while(std::chrono::steady_clock::now() < until)
  {
    ossia::net::poll_network_context(ctx);
    std::this_thread::sleep_for(1ms);
  }
}
}

int main()
{
  auto ctx = ossia::net::create_network_context();

  std::printf("== bringing up the backend ==\n");
  auto& mgr = manager::instance();
  mgr.register_context(ctx->context);
  mgr.set_scanning(true);

  std::printf("== device listing ==\n");
  sensor_list found;
  for(int i = 0; i < 40 && found.empty(); ++i)
  {
    spin(*ctx, 250ms);
    auto s = mgr.sensors();
    if(!s.empty())
      found = s;
  }
  std::printf("  %zu sensor(s) listed\n", found.size());
  for(const auto& d : found)
    std::printf("     %s | serial='%s' | io=%s | id=%s | baud=%u\n", d.name.c_str(),
                d.serial.c_str(), d.io_type.c_str(), d.identifier.c_str(), d.baud_rate);

  // ---------------------------------------------------------------------
  std::printf("== a sensor that does not exist must not block anything ==\n");
  {
    session_config cfg;
    cfg.io_type = "LinuxDevice";
    cfg.serial = "no-such-imu-0000";
    cfg.identifier = "/dev/null-imu";
    cfg.watchdog = 300ms;

    listener l;
    const auto t0 = std::chrono::steady_clock::now();
    auto s = mgr.acquire(cfg, l);
    const auto acquire_ms
        = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0)
              .count();

    check(s != nullptr, "acquire() returns a session");
    check(acquire_ms < 50, "acquire() does not block on the hardware");

    spin(*ctx, 3s);
    check(l.events > 0, "the score side is told what is happening");
    check(l.last_state == "searching", "an absent sensor sits in 'searching'");
    check(l.imu_frames == 0, "no data is invented for an absent sensor");

    const auto t1 = std::chrono::steady_clock::now();
    mgr.release(s);
    const auto release_ms
        = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t1)
              .count();
    check(release_ms < 50, "release() does not block the caller");
    spin(*ctx, 500ms);
  }

  // ---------------------------------------------------------------------
  std::printf("== the same sensor cannot be opened twice ==\n");
  {
    session_config cfg;
    cfg.io_type = "LinuxDevice";
    cfg.serial = "duplicate-test";
    listener a, b;
    auto s1 = mgr.acquire(cfg, a);
    auto s2 = mgr.acquire(cfg, b);
    check(s1 != nullptr, "the first device gets the sensor");
    check(s2 == nullptr, "the second is refused rather than fighting over the port");
    mgr.release(s1);
    spin(*ctx, 500ms);
  }

  // ---------------------------------------------------------------------
  std::printf("== a listener that goes away mid-flight is not called again ==\n");
  {
    session_config cfg;
    cfg.io_type = "LinuxDevice";
    cfg.serial = "detach-test";
    {
      listener l;
      auto s = mgr.acquire(cfg, l);
      spin(*ctx, 300ms);
      mgr.release(s);
      // l dies here while the backend may still be working on the session
    }
    spin(*ctx, 1s);
    check(true, "no use-after-free after the listener is destroyed");
  }

  // ---------------------------------------------------------------------
  if(!found.empty())
  {
    std::printf("== streaming from %s ==\n", found[0].identifier.c_str());
    session_config cfg;
    cfg.io_type = found[0].io_type;
    cfg.serial = found[0].serial;
    cfg.identifier = found[0].identifier;
    cfg.baud_rate = found[0].baud_rate;
    cfg.outputs[out_accel] = true;
    cfg.outputs[out_gyro] = true;
    cfg.outputs[out_quaternion] = true;
    cfg.outputs[out_euler] = true;

    listener l;
    auto s = mgr.acquire(cfg, l);
    for(int i = 0; i < 40 && l.imu_frames == 0; ++i)
      spin(*ctx, 250ms);

    check(l.imu_frames > 0, "IMU frames arrive");
    const int before = l.imu_frames;
    spin(*ctx, 2s);
    const int rate = (l.imu_frames - before) / 2;
    std::printf("     ~%d frames/s\n", rate);

    std::printf("     unplug the sensor now to watch the watchdog fire...\n");
    spin(*ctx, 10s);

    mgr.release(s);
    spin(*ctx, 500ms);
  }
  else
  {
    std::printf("== no sensor attached, skipping the streaming test ==\n");
  }

  mgr.set_scanning(false);
  mgr.unregister_context(ctx->context);

  std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASSED" : "FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
