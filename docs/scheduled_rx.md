# Schedule receive-only acquisition

`RxRunHighNoon.py` initializes the SDR early and schedules the **first RX sample**
for a requested wall-clock time. It does not wait until that time to launch UHD.
Subsequent receive windows follow `CHIRP.pulse_rep_int`, `rx_duration`,
`num_pulses`, and `num_presums` in your RX configuration.

For the matching TX runner and the Pi clock-setting utility, see
[scheduled TX and clock setup](scheduled_tx.md).

From the repository root, activate your working UHD environment. First check
the Pi's date/time and synchronization status:

```bash
conda activate uhd-b206
date -u
timedatectl status
```

For the tracked RX template, preview a start at noon UTC today:

```bash
python RxRunHighNoon.py --at "12:00:00" --config config/Rx_B206mini.yaml --dry-run
```

Remove `--dry-run` to build, initialize, wait, and capture. Choose a time **at
least 30 seconds ahead**, allowing several minutes for the first build. If
noon has already passed, specify a future date explicitly. For example, replace
the date below with the date you intend to use:

```bash
python RxRunHighNoon.py --at "2026-10-26T12:00:00Z" --config config/Rx_B206mini.yaml
```

For noon in a local time zone instead:

```bash
python RxRunHighNoon.py --at "12:00:00" --timezone America/Denver --config config/Rx_B206mini.yaml
```

- Without `--timezone` or an explicit offset, input times are **UTC**.
- A time alone means **today** in the chosen zone; it never rolls to tomorrow.
- An explicit ISO offset, such as `2026-10-26T12:00:00-06:00`, determines the
  instant even if `--timezone` differs. Fractional seconds are accepted.
- Ambiguous or nonexistent daylight-saving times require an explicit offset.
- Without `--config`, the script uses `config/Rx_B206mini.yaml`: 2.4 GHz,
  0 dB RX gain, 56 MS/s, and 50 two-millisecond windows spaced 100 ms apart.
  Phase dithering is disabled and `num_presums` is 1. These settings produce
  44,800,000 IQ bytes if every window succeeds. The file contains the windows,
  not the gaps between them.
- RX-only mode (`transmit: false`, `receive: true`) and `clk_ref: internal` are
  required. This script does not start a transmitter.

The wider RX windows can contain a full 20-microsecond TX chirp even when the
two SDRs have different pulse phases. They do not correct clock errors or the
first-packet failure described below. Older local files such as
`data/pi_setup/rx.yaml` and `data/rx_loopback.yaml` are preserved during updates;
use the tracked template above to get the current settings after `git pull`.

Each run creates `data/scheduled_rx/<UTC-time>_<unique-id>/`. It keeps a copy of
the requested time/configuration and saves IQ data and the UHD log there on
successful completion. Your source YAML remains unchanged. A failed run keeps
its raw files for inspection. Ctrl-C cancels the run; partial data must be
treated as incomplete.

The log includes the target SDR time, a clock-query half-span, and:

```text
[SCHEDULE] First RX hardware timestamp: ...
```

The first packet must be error-free, complete, and timestamped within one sample
of the computed SDR target unless an explicit device-time tolerance is configured.
The current RX bench template sets `CHIRP.rx_start_tolerance: 1e-6` (1 microsecond).
The reported complete, error-free hardware window was approximately 125 ns
(7 samples at 56 MS/s) after its computed target; the previous one-sample check
rejected it. The configured tolerance accepts that offset without changing the
requested time or correcting the hardware delay. The actual timestamp and error
are still logged. Without this setting, the check defaults to one sample.
A missed deadline, failed first packet, or missing
confirmation causes an error instead of reporting a successful scheduled start.
Later receive errors follow the existing runner's retry behavior; inspect the
whole log before claiming an error-free capture. Pre-summing can average later
windows into the first saved window; the confirmation refers to acquisition.

## Timing precision

**This does not guarantee an exact UTC start or synchronize two SDRs.** It maps
the Pi's system clock to the SDR's free-running clock using five bracketed USB
clock queries, choosing the shortest query. The mapping is measured about ten
seconds before the start and used in UHD's timed RX command. It refuses to arm
with less than two seconds remaining or a query span over 50 ms.

Actual UTC error includes the Pi's clock error, USB query uncertainty, oscillator
drift, and hardware delays. The logged half-span is only a clock-query estimate;
it is not a measured total timing accuracy. Do not change the Pi's clock after
arming. Hardware timestamp confirmation verifies the computed SDR time, not an
independent UTC reference.

Wi-Fi is not required during capture if the Pi's clock is already correct, but
an unsynchronized clock can start at the wrong real-world time. GPS/PPS-based
synchronization needs a separate hardware and software setup. The scheduled path
does not configure the external serial GPS or use it as a timing reference.

Software tests cover time interpretation, deadline handling, output isolation,
runner cleanup, clock mapping, and first-timestamp comparison. The cloud has no
attached SDR, so physical start-time accuracy still needs to be measured on the Pi.

## If the first scheduled RX window fails

`[SCHEDULE] First RX diagnostics` records the received/expected sample counts,
whether UHD supplied a timestamp, its difference from the requested SDR time
in seconds and samples, and the UHD error. The following metadata block
includes fragmentation and end-of-burst information. Keep these lines when
reporting a failure; the final process exit status alone does not identify it.

A first-window failure can mean a transport error, a short receive, a missing
timestamp, or a timestamp difference greater than the configured tolerance
(at least one sample). The program rejects these; it does not silently retry
the first window at a later time. The tolerance is a device-time acceptance
limit, not an estimate or guarantee of UTC accuracy.
The `[TX]` stop messages afterward come from the shared RX-command scheduling
thread and do not establish a transmitter fault.

RX should still collect noise when the other SDR is not transmitting. Test RX
alone while diagnosing this failure. Check the SDR's own interface speed with
`lsusb -t`: a device at `480M` has a USB 2 connection even if a separate root
hub reports `5000M`. USB speed, receive-window size, and host load are relevant
to transport errors; a narrow receive window is not needed to diagnose timing.
