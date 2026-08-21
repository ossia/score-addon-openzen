# score-addon-openzen

Access [LP-Research](https://lp-research.com) LPMS inertial measurement units
from [ossia score](https://ossia.io), through
[OpenZen](https://bitbucket.org/lpresearch/openzen).

Serial/USB works everywhere; classic Bluetooth is built in when the platform
Bluetooth development files are present.

## Building

```bash
git submodule update --init --recursive
```

Then drop the folder into `score/src/addons` and configure score as usual.

## Design notes

### One shared backend, no thread per sensor

Everything goes through a single `ossia::openzen::manager`
(`OpenZen/Manager.hpp`), following the same shape as libossia's joystick
protocol manager:

* **One backend thread**, shared by every sensor, owns every call that can
  block. `obtainSensor()` alone can take several seconds - OpenZen probes baud
  rates with a 2s IO timeout and retries - so it must never be reached from the
  GUI or the network thread.
* **No data thread at all.** The measurement event queue is drained by an
  `ossia::timer` on each registered network context, so samples are delivered
  on the thread ossia already wants parameter updates on, and adding a
  seventeenth IMU adds no threads.

Two `ZenClient`s are used: one whose queue carries measurement data and is
drained by the pump, and one that only carries device listings and is drained
by the backend thread. That way discovery never has to be routed across
threads.

`push()` may be reached from the audio thread, so control writes are only ever
enqueued, never executed inline.

### Surviving a replug

**OpenZen does not detect disconnection.** `ZenEventType_SensorDisconnected` is
only emitted from `Sensor::~Sensor`, that is, when *we* release the sensor. When
a cable is pulled, `PosixDeviceInterfaceImpl::run()` returns from its IO thread
and nothing else happens: the sensor object stays alive and simply goes quiet
forever.

So silence is the disconnection signal. A watchdog on the backend thread
(default 500 ms, configurable) declares the link dead, releases the port and
starts looking for the sensor again. This also covers what no IO error would
report anyway - a brown-out, a wedged firmware, an RF dropout on Bluetooth.

**The node tree is built once and never torn down.** A sensor that is not
present sits at `/connected = false` with the rest of the tree intact, so every
cable drawn against it in a score survives the cable being pulled - and a
document opens with its full tree whether or not the hardware is there.

On reconnection the whole configuration is pushed onto the sensor again, so
what the document says is always what the hardware ends up doing, whatever was
left in its flash.

### Finding the sensor again

Matching is by serial number where possible, since that is what survives the
sensor coming back on a different port:

1. A listing entry that advertises our serial number. On Linux the SiLabs USB
   serial string is in sysfs and OpenZen re-resolves the tty from it on every
   connection, so `/dev/ttyUSB0` becoming `/dev/ttyUSB1` is a non-event.
2. Otherwise the port it was last seen on.
3. Otherwise - a plain Windows COM port carries no identity at all - the
   remaining ports are tried and the serial number is verified over the wire
   once connected.

### Measurements are detected, not configured

Nothing about what a unit measures has to be picked by hand. On connection the
sensor's `ZenImuProperty_Output*` flags are read back and a node is created for
each measurement it actually produces - so a unit with a magnetometer gets
`/imu/mag`, one without simply does not, and a GNSS component brings `/gnss/*`
with it.

Node creation is additive: a node is never removed once it exists. Unplugging a
sensor, or reconnecting one that has been reconfigured in the meantime, leaves
every cable drawn against it intact. The tree is saved with the document, so a
score reopens with its full namespace whether or not the hardware is present.

Which property gates which field depends on the firmware family, and getting it
wrong changes the frame length rather than merely enabling the wrong value: the
legacy firmware derives the calibrated vector host-side from the raw one behind
a single flag, IG1 gates them separately, and the LPMS-BE1 reports its only gyro
in the second slot. See `manager::read_capabilities` and
`manager::apply_outputs`.

### Bandwidth

Turning *off* measurements is still available, behind "Detect measurements from
the sensor". These map to the same `ZenImuProperty_Output*` flags: every
measurement enabled takes room in the frame the sensor puts on the wire, and at
a fixed baud rate that is what decides the sample rate that can be reached.

### Serial port permissions on Linux

`/dev/ttyUSB*` usually belongs to a group (`uucp`, `dialout` depending on the
distribution) that a user is not in by default, and the sensor will sit in
`failed` with a message saying so. Fix it once with:

```bash
sudo usermod -aG uucp $USER   # or dialout
```

then log out and back in.

### Baud rate

The rate a unit runs at is whatever was last written to its flash, and no IO
system can report it before connecting: OpenZen assumes a default and gives up
when that is wrong. 921600 is the LPMS factory default while OpenZen's Linux
backend assumes 115200, so any fixed guess is wrong for some sensor. The
backend therefore probes, one rate per attempt so that a sensor working
through the list does not monopolise the thread every other sensor shares, and
remembers what worked so reconnection is immediate.

The sampling rate is left alone by default for the same reason: sensors accept
only a fixed set of rates and answer the rest with a NACK, and losing a working
link over a preference is the wrong trade. Setting one that the sensor refuses
is reported and otherwise ignored.

## Testing

```bash
cmake . -DSCORE_ADDON_OPENZEN_TESTS=1
cmake --build . --target score_addon_openzen_tests
./score_addon_openzen_tests            # everything
./score_addon_openzen_tests "~[hardware]"   # only what needs no sensor
./score_addon_openzen_tests "[hardware]"    # only what does
```

- `tests/test_mapping.cpp` covers the pure functions - axis conventions,
  identity, baud ordering, the additive capability merge. These are where a
  silent mistake produces plausible-looking but wrong numbers, so they are
  tested against hand-worked values.
- `tests/test_backend.cpp` runs the real backend. The cases tagged
  `[hardware]` are skipped when no sensor is attached; the rest cover the
  paths that must hold when nothing is plugged in, which is the normal state
  of affairs while a score is being written.

`ctest` runs the no-hardware subset. There is also
`score_addon_openzen_smoke`, a standalone harness that prints a readable trace
and pauses so the cable can be pulled to watch the watchdog fire - useful when
bringing up a new sensor, not something for CI.

Add `-DSCORE_ADDON_OPENZEN_VERBOSE=1` to log every frame exchanged with the
sensor; that is how the CURS3 problems above were found.
