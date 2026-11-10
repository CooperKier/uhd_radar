# Bistatic radar

One B206 transmits and one B205 receives on separate hosts. Shared external
1 Hz PPS establishes timing; TX sends one-way RF sync before each finite
chirp train. RX sends nothing. Start RX first.

The implementation has been built and software-tested on macOS with UHD 4.11.
Physical RF/PPS operation and Raspberry Pi 5 / Ubuntu operation still need
on-device verification.

## Hardware

```text
1 Hz PPS -- splitter --> B205 REF
                    \--> B206 REF

RX host -- USB --> B205 RX2 <----- B206 TX/RX <-- USB -- TX host
```

Keep PPS running throughout the experiment and use the hardware-specified
signal levels. For cabled RF tests, use suitable attenuation: the
[Ettus B2xx manual](https://files.ettus.com/manual/page_usrp_b200.html)
specifies a maximum RF input of -15 dBm and at least 30 dB loopback attenuation.
No synchronized host clocks, network link, GPS USB controller or RF return
channel is required.

## Prepare

Use a C++17 compiler, CMake and UHD >=4.9 and <5. On each Pi, from the
repository root:

```bash
conda env create -f environment-rpi-b206mini.yaml
conda activate uhd-b206mini
uhd_images_downloader

# Choose the role for this host:
python run_bistatic.py rx --prepare
python run_bistatic.py tx --prepare
```

Preparation builds the shared executable, runs the C++ tests, generates the chirp
and validates settings without opening hardware. Repeat after source or
waveform changes. Both stations need matching code, waveform and experiment
fingerprints; device selection and RF gains may differ. Both use RF protocol
version 2.

The default experiment file is `config/bistatic.yaml`. Pass another config path
after `rx` or `tx` to override it.

`GENERATE.out_file` and `FILES.chirp_loc` must identify the same waveform under
`FILES.output_dir`. Keep their YAML alias or update both when renaming a chirp.
Required settings are validated before opening hardware.

## Run

On RX first, then TX:

```bash
python run_bistatic.py rx --loops 1
python run_bistatic.py tx --loops 1
```

Omit `--loops` to repeat until Ctrl-C. Limits count completed local cycles;
TX proceeds independently even if RX misses sync. RX keeps listening when TX
stops or every copy is missed, including after a one-cycle TX run.

Use `--device-args 'type=b200,serial=YOUR_SERIAL'` to select a radio. This
replaces `DEVICE.device_args` during preparation, validation and operation;
include any transport settings in the override. An empty configured argument
string permits automatic discovery.

`--output-dir data/my_run` selects a new directory; otherwise the runner uses
a timestamped directory under `data/`. Existing directories and cycle files
are refused. `--validate` checks the cached waveform/config without hardware.

## Files

Each RX cycle produces exactly two files, plus one shared run log:

```text
radio.log
cycle_000001_config.yaml
cycle_000001_rx_samps.bin
```

The YAML contains the experiment settings and a `CAPTURE` section with
completion status, TX session/cycle/attempt, fingerprint, PPS timing, clock
ratios, RX alignment and saved/received pulse counts. Device arguments reflect
the effective override. TX writes one YAML per cycle and the shared log.
There are no separate capture reports, copied run configs, JSON summaries or
per-cycle logs. `radio.log` contains driver output and the process exit status.

Only `CAPTURE.complete: true` indicates local completion. Failed or interrupted
IQ stays as `.bin.partial`; error details go in `CAPTURE.error`. Match the TX
and RX session/cycle/attempt to associate recordings. Local completion does
not establish that a radar return was received: inspect the IQ.

IQ is interleaved float32 I,Q, with one RX window per saved record. Presumming
averages consecutive complex windows after removing optional phase dithering.
The expected byte count is:

```text
(CHIRP.num_pulses / CHIRP.num_presums) * rx_window_samples * 8
```

Sync packets, guard samples and inter-pulse gaps are omitted. The existing
loader reads the IQ/YAML pair directly, with no per-cycle log required:

```python
from postprocessing.processing import load_radar_data
slow_time, sample_rate, iq = load_radar_data("data/YOUR_RX_RUN/cycle_000001")
```

## Timing and limits

TX broadcasts three CRC-protected sync copies at 0.20, 0.30 and 0.40 seconds
after a PPS edge. Any one valid copy selects the common physical PPS edge
three edges later. RX checks the fingerprint, packet freshness and PPS phase
using the hardware receive timestamp. It rejects late announcements and
resumes listening for a later cycle.

RX starts at `CHIRP.time_offset` after the selected edge; TX starts `tx_lead`
earlier. Measured PPS intervals compensate constant sample-clock error in
pulse spacing. Device time is never reset during a run.

RX streams a guarded train and aligns its first window to the nearest sample
using the actual hardware timestamp. Later timestamps allow 0.01 sample of
numerical rounding; missing/repeated samples, overflows, PPS loss, excessive
clock drift and disk errors fail the capture. Long trains use continuous RX
when they exceed the B2xx finite-command limit. Presumming reduces disk
output, not the USB sample rate.

PPS supplies timing, not a shared frequency reference or RF phase lock.
Carrier offset is measured during sync but is not corrected in saved IQ.
Complex presumming can cancel a return when independent oscillators rotate
its phase between pulses. Begin with `num_presums: 1` when checking individual
pulse alignment and phase stability.

This mode requires channel 0, fc32 CPU samples, internal clock reference plus
external PPS, positive finite pulse counts, a positive presumming factor that
divides the pulse count, no file splitting and disabled amplifier GPIO. Pulse
windows must be shorter than PRI, cycles are limited to 60 seconds, and at
least 0.25 seconds must remain to arm after the selected PPS.

## Software checks

Protocol, modem and pulse-stream tests run without UHD:

```bash
cmake -S bistatic -B bistatic/build-tests -DBISTATIC_BUILD_RADIO=OFF
cmake --build bistatic/build-tests --parallel 2
ctest --test-dir bistatic/build-tests --output-on-failure
```

With the shared radio executable built, also run:

```bash
python -m unittest discover -s tests -p test_run_bistatic.py -v
```

The dedicated CI job builds the executable with UHD >=4.9 and tests both roles.
`bistatic/build-tests/test_bistatic_protocol --noise-sweep` provides a simulated
AWGN decoding sweep; it does not measure hardware sensitivity.

For hardware verification, run one attenuated loopback cycle, compare TX/RX
identifiers and completion flags, check file size, and inspect the chirp delay
and spacing. Then repeat cycles and test Ctrl-C, missed RF sync and PPS loss.
The `[SYNC RX]` log reports sample flow, clipping indicators, correlation,
invalid headers and decoded packets to help distinguish streaming and RF
problems. A decoded sync packet alone does not prove radar alignment.
