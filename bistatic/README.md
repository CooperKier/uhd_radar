# B205 RX-only / B206 TX-only bistatic radar

Two Raspberry Pis run independent role executables using the same
[experiment configuration](../config/default.yaml). Start RX first. TX begins
later and broadcasts RF sync before each finite chirp train. The outer loop runs
forever unless `--loops N` is supplied. Ctrl-C stops either process.

This implementation has been compiled and software-tested on macOS with UHD
4.11. **Raspberry Pi 5 / Ubuntu builds and physical RF/PPS operation have not
been verified.** The commands below are the on-device validation procedure,
not a claim that those checks have already passed.

## Hardware connections

```text
External 1 Hz generator -- splitter --> B205 REF
                                  \--> B206 REF

Pi RX -- USB 3 --> B205       B206 <-- USB 3 -- Pi TX
                  RX2 <----- TRX     sync preambles + radar
```

One RF path from B206 to B205 is sufficient. The B206 uses only its transmit
antenna (`TX/RX` by default), and the B205 uses only its receive antenna
(`RX2` by default). The code configures and creates streams for the selected
direction only. RX never sends RF messages, and TX never opens an RX stream.
There is no return link, readiness ACK, or Wi-Fi coordination requirement.
Sync packets use the configured RF center frequency, 1 MS/s, differential
BPSK, and `BISTATIC.control_tx_gain` (default 0 dB, TX only). Radar data uses
the RF gains, rates, bandwidth and waveform from the config.

Keep PPS running before startup and throughout the experiment; there is no
manual PPS enable step. Use the hardware-specified signal levels. For cabled
RF bench tests, use suitable attenuation in the forward path: the
[Ettus manual](https://files.ettus.com/manual/page_usrp_b200.html) specifies a
maximum RF input of -15 dBm and at least 30 dB attenuation for loopback.

The Pis do not need synchronized host clocks or a network timing link.
There is no GPS USB controller or GPS fix validation in this first mode.
Separate GPS PPS sources are a future step: PPS labels, receiver validity,
and physical timing accuracy will need verification before deployment.

## Prepare on each Pi 5 running 64-bit Ubuntu

Install a C++17 compiler, CMake and the Conda environment dependencies. From
the repository root on each Pi:

```bash
conda env create -f environment-rpi-b206mini.yaml
conda activate uhd-b206mini
uhd_images_downloader
uhd_config_info --version
uhd_find_devices
```

Both stations need UHD >=4.9 and <5 as specified by the environment. With
that environment active, prepare the appropriate role:

```bash
# B205 / RX Pi
python run_bistatic.py rx config/default.yaml --prepare

# B206 / TX Pi
python run_bistatic.py tx config/default.yaml --prepare
```

Preparation builds the selected executable, runs the C++ protocol/DSP tests,
generates the chirp, and validates the config without opening a radio. Those
checks passing **on the Pi** establish an Ubuntu/ARM software baseline; they
do not establish USB streaming throughput or synchronization accuracy.
Repeat preparation after changing source code or waveform settings. Both
stations must have the same code version, effective config and waveform;
their printed experiment fingerprints must match. Device serials and gains
may differ without changing the waveform/timing fingerprint.

This one-way mode uses RF protocol version 2. Copy this version to **both**
Pis and rerun preparation; it rejects the earlier ACK-based protocol.

To run the Python runner/config-validation tests on each prepared Pi as well:

```bash
python -m unittest discover -s tests -p test_run_bistatic.py -v
```

## Start one bench cycle

On the RX Pi first:

```bash
python run_bistatic.py rx config/default.yaml --loops 1
```

Then on the TX Pi:

```bash
python run_bistatic.py tx config/default.yaml --loops 1
```

With more than one USRP attached, select the local radio explicitly:

```bash
python run_bistatic.py rx config/default.yaml --device-args 'type=b200,serial=YOUR_B205_SERIAL' --loops 1
python run_bistatic.py tx config/default.yaml --device-args 'type=b200,serial=YOUR_B206_SERIAL' --loops 1
```

An override replaces `DEVICE.device_args`; include any transport tuning you
need in the override as well. Output goes to a new timestamped directory in
`data/`; `--output-dir data/my_rx_run` selects another new directory. Existing
directories and cycle files are refused.

After the one-cycle test succeeds, omit `--loops` on both stations to repeat
forever, or give a positive number of completed local cycles. TX continues
its cycles even when RX is stopped or misses sync. RX keeps listening when
TX is stopped. Loop counts are local, not a distributed completion guarantee.

## One synchronization / radar cycle

1. Each radio locks its master-clock configuration before observing PPS. It
   measures successive 1 Hz edges. The two local second counters can differ
   because the programs start separately.
2. TX broadcasts three sync copies at phases 0.20, 0.30 and 0.40 seconds
   after one observed edge. Each identifies the same TX session, radar
   cycle, attempt and experiment fingerprint. The copy index identifies its
   expected phase. All three select the same radar reference edge, three
   physical PPS edges after their common preceding edge.
3. RX can accept **any one** valid copy, using its **hardware RX timestamp**
   to find the corresponding edge in its local PPS history. It checks the
   expected PPS phase, config fingerprint and freshness. It sends nothing.
4. Both wait for the selected physical edge, then schedule the configured
   train. `CHIRP.time_offset` is measured from that edge; RX starts then,
   and TX starts `CHIRP.tx_lead` earlier. No device time is reset during a run.
5. After the finite train and an additional verified PPS, each saves its
   status. TX starts broadcasting sync for the next cycle; RX resumes listening.

Packets have a versioned header and CRC. Wrong configs, duplicate or stale
cycles, corrupted packets and late sync are rejected. If all three copies
are missed, RX skips that cycle and listens for the next one. TX proceeds
regardless of receiver readiness; there is no reception or completion
confirmation. Compare the session/cycle/attempt in both reports to associate
captures. A local `complete: true` report establishes local streaming/file
completion; inspect the IQ to establish that the radar signal was received.

UHD's local TX burst-completion events are still checked for streaming
errors. Those are driver status messages over USB, not RF ACKs from RX.

This initial implementation fails the run on a data-stream error, PPS loss,
clock jump, excessive drift or disk-write failure. It does not silently
schedule extra chirps, drop corrupt windows, or call an incomplete file
complete. Missing sync packets leave RX searching for a later cycle.

## Phase dithering, frequency and data format

Phase dithering is optional and disabled in `config/default.yaml` for initial
testing. When enabled, TX/RX restart the same deterministic sequence on each
cycle, and RX removes that deliberate rotation before saving, as the
original combined radar does.

PPS establishes the edge used for timing. It does **not** provide a common
frequency reference or RF phase lock; see
[Ettus synchronization documentation](https://files.ettus.com/manual/page_sync.html).
The control preamble estimates carrier offset (unambiguous range +/-31.25 kHz).
PPS intervals estimate each radio's clock-rate error. That estimate scales
pulse spacing and receive-window locations to avoid accumulating the measured
constant clock error over a train. This is software scheduling compensation,
not oscillator disciplining. Thermal changes, phase noise, calibration error
and electrical PPS skew still require measurement on hardware.

Carrier-offset measurements are saved; carrier/phase correction is **not**
applied to saved radar IQ. Consequently this first mode requires
`CHIRP.num_presums: 1`. Coherent averaging across chirps needs additional phase
tracking or suitable frequency-reference hardware. The default two-second
train remains a hardware validation target, not a demonstrated coherence
interval.

Each RX cycle contains these files under the run directory:

```text
cycle_000001_rx_samps.bin     interleaved float32 I,Q, one configured RX window/chirp
cycle_000001_config.yaml     experiment config, compatible with existing readers
cycle_000001_uhd_stdout.log  legacy-format start log
cycle_000001_capture.yaml    session/cycle/attempt, PPS, clock and completion metadata
```

The binary has the same per-record layout used by the original code. The
sync preambles and zero-valued inter-chirp intervals are not written
to that file. With the default settings it contains 10,000 records of 1,120
complex samples, or **89,600,000 bytes**. Incomplete IQ is retained with the
`.bin.partial` suffix and `complete: false`. TX saves its timing/completion
report and config. Both roles also save `radio.log` and `run.json`.

Existing processing can load an RX cycle by prefix:

```python
from postprocessing.processing import load_radar_data
slow_time, sample_rate, iq = load_radar_data("data/YOUR_RX_RUN/cycle_000001")
```

At 56 MS/s the code continuously streams each approximately two-second
train and extracts only the configured RX windows. Memory is bounded by
stream buffers; Pi/USB throughput and disk performance must be measured.
This mode requires fc32 CPU samples, active channel 0, finite positive
`num_pulses`, `num_presums: 1`, no file splitting and disabled amplifier GPIO.
Each role reads only its active direction's channel/rate/gain/antenna
settings. `RF0.transmit` must be true on TX; RX can use false and still
share the same experiment fingerprint. The original combined runner remains
independent.

## Validation checklist on Ubuntu / Pi hardware

1. Run both `--prepare` commands and record `uhd_config_info --version`.
2. Run one real cycle with the forward TX-to-RX RF path and shared PPS connected.
   Compare the session/cycle/attempt in both reports, require both exit
   statuses to be zero and `complete: true`, and check RX file size.
3. Use a measured RF loopback/reference path to verify the received chirp
   location, sample spacing and carrier offset; compare against the expected
   delay. A decoded sync packet alone does not prove sample alignment.
4. Repeat several cycles, then test omission of `--loops` and Ctrl-C. Confirm
   that complete cycle files remain readable and interrupted data is marked
   incomplete.
5. Start TX with RX stopped: TX should complete its requested cycles without
   waiting. Start RX while TX repeats indefinitely: RX should join a later
   cycle. Block the forward RF path: RX should keep listening while TX
   continues. Restore it and check that RX acquires a subsequent cycle.
6. Remove PPS during synchronization and during capture. The run should fail
   instead of marking a capture complete. Try different experiment configs:
   RX should ignore the mismatched sync while TX continues independently.

Software-only protocol/DSP checks can also be built without UHD:

```bash
cmake -S bistatic -B bistatic/build-tests -DBISTATIC_BUILD_RADIO=OFF
cmake --build bistatic/build-tests --parallel 2
ctest --test-dir bistatic/build-tests --output-on-failure
```

## Troubleshooting RX waiting after PPS acquisition

`[PPS] Acquired` confirms local PPS acquisition. Radar recording starts only
after RX accepts a valid RF packet. Seeing RF power on an analyzer does not
establish that RX can decode that packet. Start RX before TX and keep TX
repeating while diagnosing the link.

Both roles print `[CONTROL]` with the actual RF center frequency, control
sample rate, gain and antenna. Verify that the centers agree and that the
RF path reaches the selected RX antenna. RX prints `[SYNC RX] Listening`
when continuous control reception starts, then interval statistics every
five seconds while waiting:

* `samples`, `timeouts`, `overflows` and `timestamp_gaps` describe control
  streaming. Nonzero sample counts establish sample flow, including noise;
  they do not establish packet reception. Frequent overflows or gaps can
  prevent decoding. Zero samples with repeated timeouts indicates a stream
  problem rather than simply weak RF.
* `peak_dbfs` is peak normalized complex IQ power, not RF input dBm.
  `near_fullscale` counts samples with I or Q magnitude >=0.98; sustained
  counts suggest possible clipping, including during stronger radar bursts.
* `best_corr` is the largest preamble score searched during the interval,
  not SNR or a probability. `preamble_hits` counts threshold crossings;
  `invalid_headers` counts candidates rejected by CRC/header validation.
  Overlapping buffers can revisit candidates, so these are decoder-work
  counts, not counts of distinct transmitted packets.
* `decoded` counts valid packet decodes. Each decode also prints its cycle,
  copy number, correlation and estimated carrier offset. Subsequent sync
  messages explicitly identify configuration mismatches, PPS-phase rejection
  with measured/expected phases, or duplicate/stale packets.

Compare both printed experiment fingerprints and regenerate the chirp with
`--prepare` after waveform changes. For phase rejections, verify that both
REF inputs receive aligned physical PPS edges; matching 1 Hz frequencies
from independent free-running generators are insufficient. Preserve the
packet and timing checks while locating the RF, streaming or timing fault.
These diagnostics also appear in the runner's `radio.log`.

## Testing noisy preambles

The detector averages samples within each control symbol and correlates the
differential symbol sequence against its known preamble. A candidate needs
normalized correlation of at least `BISTATIC.preamble_threshold` (default
0.85), then a valid packet CRC, matching experiment fingerprint, expected PPS
phase and fresh cycle identifiers. Correlation score is not a probability of
correct decoding. The packet CRC detects errors; there is no forward error
correction that repairs a damaged header.

TX sends three complete sync copies per cycle. RX needs just one valid copy.
If noise destroys all three, RX skips the train and continues searching;
TX continues independently. Persistent interference or fading can damage all
copies, so repetition cannot guarantee reception. With `--loops 1` on TX,
RX can remain waiting indefinitely after missing that cycle; Ctrl-C stops it.
Use several TX cycles or continuous TX for a noise/recovery test.

After the software-only build above, run a simulated noise sweep:

```bash
bistatic/build-tests/test_bistatic_protocol --noise-sweep
```

It sends 200 simulated packets at each SNR, from -20 to +20 dB, through the
actual decoder. It adds complex white Gaussian noise, random carrier offsets
within +/-12 kHz, unknown phase and arbitrary sample alignment. Output reports
decoded-packet counts and valid headers detected in noise-only buffers. SNR
means signal power divided by total complex noise power **per input sample**.
The sweep uses the default 0.85 detector threshold.

These results characterize this simulation, not the radio's measured
sensitivity. They exclude multipath, fading, clipping, sample-clock drift and
host/USB failures. Finite noise-only trials cannot establish a zero false
detection rate. The regular regression tests also check CRC corruption,
lost copies, stale cycles, PPS mapping, and TX/RX pulse windows.

For a real bench test, begin with a known, safely attenuated RF reference
path and working PPS. Increase attenuation in measured steps and compare
TX cycle IDs with the `cycle` field in RX capture reports; missing IDs
indicate skipped trains. RX file numbering counts local captures and can
differ from the TX cycle IDs. Briefly block the RF path, then restore it:
RX should join a later cycle. A completed capture still needs inspection
for the expected chirp and delay; successful sync alone does not establish
radar signal quality. Pi 5/Ubuntu RF/PPS testing remains pending.
