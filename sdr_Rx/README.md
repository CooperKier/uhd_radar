# Shared-PPS TX/RX proof of concept

This bench PoC uses separate [sdr_Tx](../sdr_Tx) and [sdr_Rx](.) builds, a shared
[experiment config](../config/poc_shared_pps.yaml), and one generator supplying
the same 1 Hz PPS edge to both radios. It uses experiment-relative time: the
first shared edge is time zero. GPS, UTC synchronization, and a network timing
service are not required for this bench procedure.

The existing [run_tx.py](../run_tx.py) and combined [sdr](../sdr) build remain
available. The PoC is started with [run_poc.py](../run_poc.py).

## Hardware setup

Use two supported B2xx radios and a 64-bit Pi host for each, or separate serial
selections on one host. Activate the B206mini UHD environment on both hosts.

```text
                         +--> TX radio REF (PPS)
1 Hz pulse generator ----+
                         +--> RX radio REF (PPS)

TX radio TRX ------------ RF path ------------ RX radio RX2
```

The generator is a timing source; the TX radio generates the RF preamble and
chirps. Configure a clean rising-edge 1 Hz pulse, with levels and drive suitable
for the radios' PPS inputs. Verify the actual waveform at both inputs under the
connected load; generator amplitude displays can assume a different termination.
Use an appropriate distribution arrangement and account for cable delays.
See the [Ettus timing-input requirements](https://kb.ettus.com/index.php?redirect=no&title=B200/B210/B200mini/B205mini/B206mini#Timing_Reference_Input).

For an RF coax loopback, use suitable attenuation and keep RX input power within
the hardware specification. Do not connect the transmitter directly to the
receiver without establishing a safe input level. The PoC starts at low TX gain.

**Keep generator output disabled until both programs report `[SYNC] ARMED`.**
Each program checks for an existing PPS signal before arming. Enabling it early
can establish different zero edges. After a failure or interruption, disable PPS
and restart both processes together.

## Prepare once on each host

From the repository root, with the appropriate environment active:

```bash
conda activate uhd-b206mini
uhd_images_downloader
```

On the transmitter host:

```bash
python run_poc.py tx config/poc_shared_pps.yaml --prepare
```

On the receiver host:

```bash
python run_poc.py rx config/poc_shared_pps.yaml --prepare
```

Preparation builds only the selected role, generates the chirp, and validates
the finite capture plan without opening either radio. Repeat it after changing
source code or waveform settings. Use identical experiment settings on both
hosts. The PoC uses 4 MS/s, 1,000 chirps, and about 34 MB of raw RX IQ by default;
these are separate settings from the existing 56 MS/s experiment.

## Run the experiment

1. Disable generator output.
2. Start RX:

   ```bash
   python run_poc.py rx config/poc_shared_pps.yaml --output-dir data/poc_rx_test01
   ```

3. Start TX in its own terminal/host:

   ```bash
   python run_poc.py tx config/poc_shared_pps.yaml --output-dir data/poc_tx_test01
   ```

4. Wait for **both** terminals to print `[SYNC] ARMED`.
5. Enable the generator's PPS output. Both radios latch time zero on its first
   edge. RX begins shortly before the scheduled first frame, at five seconds
   after time zero by default. No simultaneous keyboard action is required.
6. Wait for both finite runs to finish. RX automatically aligns and stacks the
   recording, without displaying plots. Both terminals print elapsed run time;
   this includes the operator's PPS wait. RX also prints processing time.

Output directories must be new. Use a different name for every attempt; raw
captures and prior exports are never automatically deleted. Ctrl-C stops the
role and retains any partial RX recording, marked incomplete. The decoder
refuses incomplete captures and timestamp discontinuities.

For two radios on one host, add each device's serial without editing the shared
file:

```bash
python run_poc.py rx config/poc_shared_pps.yaml --device-args "type=b200,serial=RX_SERIAL"
python run_poc.py tx config/poc_shared_pps.yaml --device-args "type=b200,serial=TX_SERIAL"
```

Use two terminals for those commands. `--gps gpsd` optionally preserves the
existing gpspipe/GPX location logging, independently of generator-based timing.
The default PoC mode does not require GPS services. Legacy TX GPS defaults are
unchanged.

## Frame and processing behavior

Each timed RF frame contains:

```text
4 repeated known PN blocks | differential-BPSK header + CRC32 | guard | radar pulses
```

The fixed preamble is independent of the radar phase seed. Header protocol v1
is little-endian and includes run ID, frame/pulse index, seed, dithering flag,
TX/RX sample counts, sample rate, PRI, frame dimensions, stacking count, RF
frequency, scheduled frame time, chirp bandwidth and length. Both devices still
need the same bootstrap frequency, sample rate, and framing parameters. This
PoC validates header metadata against the shared file; it does not remotely
reconfigure an initially unknown receiver. Chirp type/window remain in the
saved config and reference waveform rather than in the RF header.

The preamble/header repeat every 50 radar pulses by default. Sync framing adds
gaps between groups; timestamps in the export account for those gaps. Within a
group, pulses use `CHIRP.pulse_rep_int`. Preamble and header are undithered.
Optional radar dithering uses seed 0 by default and retains the original
MT19937 uint32-to-float32-radians phase sequence.

RX records continuous complex64 IQ in bounded chunks and logs each chunk's
device timestamp. After capture, [the decoder](../sdr_common/poc.py):

1. Checks capture completeness and timestamp continuity.
2. Finds preambles and validates header CRC/run ID/config fields.
3. Estimates per-frame carrier offset and reference phase, then refines them
   using the CRC-validated header symbols.
4. Estimates sample-clock drift from adjacent valid frame arrivals and linearly
   resamples radar windows.
5. Undoes optional phase dithering by absolute pulse index.
6. Averages valid pulses into the configured `num_presums` groups and exports
   complex IQ with timestamps and actual stack counts.

Failed header frames are reported and excluded; losing a frame does not shift
the phase sequence or merge separate pulse groups. Partial stacks retain their
actual counts. Raw IQ and timestamp/error logs remain available for inspection.
Input processing uses a memory map and small frame/search buffers. The current
PoC limits the accumulated stacked output to 256 MiB; larger experiments need a
streamed export format.

## Transfer and plot

Each RX directory contains `rx_iq.bin`, `rx_timestamps.csv`, `capture.yaml`, the
config, terminal log, `sync_report.json`, and `stacked_iq.npz`. The NPZ contains
stacked complex IQ, reference chirp, pulse indices, stack counts, scheduled TX
device times, and metadata. Transfer that NPZ for plotting; retain the full run
directory on the Pi during validation. Transfer GPS/GPX files too if using them.

From the desktop, replacing the host and Pi path:

```bash
scp PI_USER@PI_HOST:~/uhd_radar/data/poc_rx_test01/stacked_iq.npz .
python postprocessing/plot_poc.py stacked_iq.npz
```

The plot applies matched filtering to the already-stacked IQ and uses delay
relative to the preamble reference, not an assumed monostatic range. To save a
figure without a display:

```bash
python postprocessing/plot_poc.py stacked_iq.npz --save radargram.png
```

For manual processing, capture with `--no-process`, then run:

```bash
python postprocessing/process_poc.py data/poc_rx_test01
```

## Validation and limits

```bash
python -m unittest discover -s tests -p test_poc.py
cmake --build sdr_Rx/build --target poc_pps_test
ctest --test-dir sdr_Rx/build --output-on-failure
```

The signal tests use frames emitted by the actual C++ transmitter, inject
carrier/phase offset, noise, sample-clock drift and failed headers, and test
the Python decoder, stacking and plot export without SDR hardware. The PPS test
simulates first-edge arming, an already-running generator, missing PPS,
cancellation, and a missed scheduling deadline.

This is a finite bench PoC, not a field-validated synchronization system.
Validate both physical PPS inputs, simultaneous first-edge time zero, RF levels,
frame CRC success, measured frequency offset and recovered chirps on the actual
radios. The default repeated-block CFO estimator has an unambiguous range of
about ±16.1 kHz at 4 MS/s; offsets outside that range require a wider acquisition
scheme. Preamble timing detection is currently at sample resolution, with linear
inter-frame drift correction. It requires a usable reference path and sufficiently
stable drift within a frame; strong multipath and rapid motion need further work.

PPS establishes the initial epoch and does not discipline the independent RF
oscillators. A successful header alone does not prove coherent radar performance.
The PoC does not implement GPS UTC labeling, indefinite runs, or late joining
beyond the configured preamble search/capture window.
