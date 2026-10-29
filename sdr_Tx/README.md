# TX-only SDR

For the generator-PPS, preamble/header and separate-receiver proof of concept,
use [run_poc.py](../run_poc.py) and the [shared-PPS instructions](../sdr_Rx/README.md).
That path builds `radar_tx_poc`; the existing `radar_tx` workflow below remains
available for the original TX-only experiment.

This directory adapts the device, chirp, RF, phase dithering, transmit loop, and
GPS code from [sdr](../sdr) for a Raspberry Pi 5 with a B206mini. It builds
independently and has no receive stream, receive configuration requirements,
presumming, received sample output, or RX-dependent scheduling. The original
SDR directory continues to provide the combined radar.

The example [tx_b206mini.yaml](../config/tx_b206mini.yaml) contains both TX and
RX parameters so identical copies can describe the experiment on both devices
and accompany their data for post-processing. The TX executable ignores RX,
RX archival, and plotting fields. The config also includes RF1 values for the
original SDR loader's single-channel interface; this does not activate a second
frontend. Device roles must be selected by their respective runners: the original
[run.py](../run.py) still runs combined TX/RX when `RF0.transmit` is true and is
not a receive-only runner for this shared file. Identical settings alone do not
synchronize the two devices' clocks or pulse starts.

GPS logging runs as separate processes supervised by [run_tx.py](../run_tx.py).
If a GPS process exits, the runner stops transmission and reports an error.
An active logger does not guarantee a satellite fix. The runner starts GPS and
TX concurrently without waiting for a fix; check your GPS fix before starting
an experiment that requires position throughout the run.

## Prepare on the Raspberry Pi

Use a 64-bit Linux installation and activate the B206mini Conda environment.
The Pi needs a C++17 compiler and CMake. For the default GPS mode it also needs
gpsd configured for the GPS device and the `gpspipe`/`gpxlogger` utilities
(typically installed with `sudo apt install gpsd gpsd-clients`).

From the repository root:

```bash
conda activate uhd-b206mini
uhd_images_downloader
python run_tx.py config/tx_b206mini.yaml --prepare
```

Preparation builds into this directory's `build` folder, generates the chirp,
and validates its size and settings without opening the radio. It does not
transmit, access GPS, or fetch GoogleTest. Run it again after source or waveform
changes. The environment includes `ruamel.yaml` for chirp generation; update
an already-created environment if that dependency is missing.

## Run

```bash
python run_tx.py config/tx_b206mini.yaml
```

Normal runs reuse the executable and load the chirp once. The TX loop reuses
buffers, applies optional phase dithering with the original seed, and schedules
timed bursts independently of GPS. It checks UHD asynchronous events and burst
acknowledgements, and reports an error for underruns, late packets, or partial
sends. No recompilation, waveform regeneration, scientific Python imports, GPS
startup sleep, or RX file archiving happens in the normal run path.

Each run gets a new `data/tx_<UTC timestamp>` directory containing its config,
GPS logs, GPS process diagnostics, and start/finish metadata. GPS records use the
host timestamp; GPS logging does not synchronize the SDR clock. The default
internal clock uses relative device timing with no PPS startup wait. An actual
supported GPSDO can still be selected with `DEVICE.clk_ref: gpsdo`; its lock and
PPS synchronization necessarily take longer. A USB GPS alone is not a GPSDO.

For a GPS emitting NMEA GGA sentences directly on the serial port:

```bash
python run_tx.py config/tx_b206mini.yaml --gps serial --gps-device /dev/ttyACM0
```

The serial logger uses 115200 baud, accepts `$GNGGA` and `$GPGGA`, verifies the
checksum and a valid fix, and writes `host_timestamp_us,latitude,longitude,altitude`
to `gps_log.txt`. It uses the GPS's existing NMEA configuration; it does not send
UBX configuration commands. Use one GPS access method per run. For direct serial
access, release that port from gpsd first.

Ctrl-C and SIGTERM stop TX before GPS. A short scheduling window bounds queued
future pulse starts; driver shutdown and an in-progress pulse can add delay.
`CHIRP.num_pulses: -1` repeats until stopped, while nonnegative values schedule
that many pulses. This version supports one TX channel and `fc32` samples.
`CHIRP.time_offset` is relative to the first TX pulse; legacy `tx_lead` is ignored.
Optional amplifier GPIO uses TX ATR with no lead/trail delays. The example gain
is 0 dB; set frequency, bandwidth, gain and pulse timing for your experiment.

## Validation

```bash
python run_tx.py config/tx_b206mini.yaml --validate
python -m unittest discover -s tests -p test_run_tx.py
```

The first command validates config and cached waveform without opening hardware.
The second tests the launcher with simulated subprocesses. On the Pi, also test
device discovery, GPS fix/log output, finite pulse completion, continuous TX,
and Ctrl-C behavior. Measure first-transmit latency and check for underruns at
the intended pulse rate; performance has not been measured on the Pi.
