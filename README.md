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

### Bandwidth

The measurement checkboxes are not a filter on our side: they map to the
sensor's own `ZenImuProperty_Output*` flags. Every measurement enabled takes
room in the frame the sensor puts on the wire, and at a fixed baud rate that is
what decides the sample rate that can be reached. Turning off what a patch does
not use is the most effective thing one can do here.

Which property gates which field depends on the firmware family, and getting it
wrong changes the frame length rather than merely enabling the wrong value - see
`manager::apply_outputs`.

## Testing

`tests/manager_smoke.cpp` exercises the backend with and without hardware; the
no-hardware path is the one that must never hang.

```bash
cmake . -DSCORE_ADDON_OPENZEN_TESTS=1
cmake --build . --target score_addon_openzen_test
./score_addon_openzen_test
```

With a sensor attached it will also stream from it, and pauses so that the
cable can be pulled to watch the watchdog fire.
