// Unit tests for the parts that are pure functions of their inputs.
// No sensor, no network context, no OpenZen: these run anywhere.
#include <OpenZen/Mapping.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ossia::openzen;

TEST_CASE("quaternion is reordered from Zen's wxyz to ossia's xyzw", "[mapping]")
{
  // Distinct values so a wrong permutation cannot accidentally pass.
  const float zen[4] = {1.f, 2.f, 3.f, 4.f}; // w=1 x=2 y=3 z=4
  const auto o = to_ossia_quaternion(zen);

  CHECK(o[0] == 2.f); // x
  CHECK(o[1] == 3.f); // y
  CHECK(o[2] == 4.f); // z
  CHECK(o[3] == 1.f); // w

  SECTION("the identity quaternion survives the trip")
  {
    const float identity[4] = {1.f, 0.f, 0.f, 0.f}; // w=1
    const auto id = to_ossia_quaternion(identity);
    CHECK(id[0] == 0.f);
    CHECK(id[1] == 0.f);
    CHECK(id[2] == 0.f);
    CHECK(id[3] == 1.f); // ossia puts w last
  }
}

TEST_CASE("euler angles are reordered from LPMS roll/pitch/yaw to ossia ypr", "[mapping]")
{
  const float lpms[3] = {10.f, 20.f, 30.f}; // roll, pitch, yaw
  const auto o = to_ossia_euler(lpms);

  CHECK(o[0] == 30.f); // yaw
  CHECK(o[1] == 20.f); // pitch
  CHECK(o[2] == 10.f); // roll
}

TEST_CASE("a session is identified by serial number where there is one", "[identity]")
{
  session_config c;
  c.io_type = "LinuxDevice";
  c.serial = "LPMSCO3D001B0039";
  c.identifier = "/dev/ttyUSB0";

  SECTION("serial wins, so the port may change freely")
  {
    auto moved = c;
    moved.identifier = "/dev/ttyUSB7";
    CHECK(identity_key(c) == identity_key(moved));
  }

  SECTION("two different sensors do not collide")
  {
    auto other = c;
    other.serial = "LPMSCO3D001B0040";
    CHECK(identity_key(c) != identity_key(other));
  }

  SECTION("without a serial the port is the identity")
  {
    c.serial.clear();
    auto other = c;
    other.identifier = "/dev/ttyUSB1";
    CHECK(identity_key(c) != identity_key(other));
  }

  SECTION("matching by port is honoured even when a serial is known")
  {
    c.match_by_serial = false;
    auto moved = c;
    moved.identifier = "/dev/ttyUSB7";
    CHECK(identity_key(c) != identity_key(moved));
  }

  SECTION("the same address on two IO systems is two sensors")
  {
    auto other = c;
    other.io_type = "Bluetooth";
    CHECK(identity_key(c) != identity_key(other));
  }
}

TEST_CASE("baud rates are tried most-likely-first and never twice", "[baud]")
{
  const auto& probe = default_probe_bauds();

  SECTION("the factory default leads the probe")
  {
    // An LPMS ships at 921600 while OpenZen's Linux backend assumes 115200,
    // so the probe must not simply trust the latter.
    CHECK(probe.front() == 921600u);
    CHECK(std::find(probe.begin(), probe.end(), 115200u) != probe.end());
  }

  SECTION("a known-good rate is tried first, so reconnection is immediate")
  {
    const auto v = baud_candidates(460800, 0, probe);
    REQUIRE(!v.empty());
    CHECK(v.front() == 460800u);
  }

  SECTION("a configured rate beats the probe list but not a known-good one")
  {
    const auto v = baud_candidates(460800, 115200, probe);
    REQUIRE(v.size() >= 2);
    CHECK(v[0] == 460800u);
    CHECK(v[1] == 115200u);
  }

  SECTION("no rate is tried twice")
  {
    auto v = baud_candidates(921600, 115200, probe);
    auto sorted = v;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end());
  }

  SECTION("zero means unset and is not probed")
  {
    const auto v = baud_candidates(0, 0, probe);
    CHECK(std::find(v.begin(), v.end(), 0u) == v.end());
    CHECK(v.size() == probe.size());
  }

  SECTION("every rate in the probe list is reachable")
  {
    const auto v = baud_candidates(0, 0, probe);
    for(auto b : probe)
      CHECK(std::find(v.begin(), v.end(), b) != v.end());
  }
}

TEST_CASE("the node tree only ever grows", "[capabilities]")
{
  capabilities have;
  have.accel = true;
  have.quaternion = true;

  SECTION("a new measurement is noticed")
  {
    capabilities next = have;
    next.gyro = true;
    CHECK(adds_anything(have, next));
    CHECK(merge(have, next).gyro);
  }

  SECTION("the same set again changes nothing")
  {
    CHECK_FALSE(adds_anything(have, have));
  }

  SECTION("a sensor that comes back with fewer measurements takes none away")
  {
    // Otherwise a reconnection would delete nodes a score has cables to.
    capabilities fewer;
    fewer.accel = true;

    CHECK_FALSE(adds_anything(have, fewer));

    const auto merged = merge(have, fewer);
    CHECK(merged.accel);
    CHECK(merged.quaternion);
  }

  SECTION("a GNSS component appearing later is picked up")
  {
    capabilities with_gnss = have;
    with_gnss.gnss = true;
    CHECK(adds_anything(have, with_gnss));
    CHECK(merge(have, with_gnss).gnss);
  }

  SECTION("merging is symmetric")
  {
    capabilities a, b;
    a.gyro = true;
    b.mag = true;
    CHECK(merge(a, b) == merge(b, a));
  }
}
