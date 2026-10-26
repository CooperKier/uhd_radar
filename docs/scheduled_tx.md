# Scheduled transmission and Pi clock setup

The repository includes `TxRunHighNoon.py` for TX-only scheduling,
`RxRunHighNoon.py` for RX-only scheduling, and `set_pi_time.py` for the Pi's
Linux clock. Run these from the repository root. Update the **whole checkout**;
copying only a runner omits the C++ changes and shared Python helper it needs.

## 1. Set/check each Pi's clock

With internet access, use the configured Ubuntu network-time service:

```bash
sudo python3 set_pi_time.py --ntp
date -u
timedatectl status
```

The script waits up to 30 seconds for the service to report synchronization and
fails if it cannot. Ethernet works; Wi-Fi is not specifically required. This
does not install a missing time service or turn a disconnected Pi into a
network-synchronized clock. Use `--timeout 120` if the service needs more time.

For offline manual setup, substitute the **actual current date/time**. Preview
first without sudo; this example is only a format illustration:

```bash
python3 set_pi_time.py --at "2026-10-26T11:55:00-06:00" --dry-run
sudo python3 set_pi_time.py --at "2026-10-26T11:55:00-06:00"
```

Alternatively, omit the numeric offset and specify `--timezone America/Denver`.
Times without an offset or zone are UTC. The full date is required because a
new Pi may have the wrong date. The utility sets the supplied instant when the
command executes; manual typing/SSH latency limits accuracy. It does not wait
until that instant. It checks the resulting clock and returns nonzero if the
change or verification fails.

Manual mode disables the network-time service managed by `timedatectl` and
leaves it disabled after success. If setting the clock fails, it attempts to
restore a previously enabled service. Use `--ntp` later to re-enable network
time. It sets Linux system time, not the display time zone, RTC, or SDR time.
Correct time after reboot depends on the Pi's working RTC or a time service.

**Set the clock before launching either scheduled runner. Do not change or step
the clock while a run is waiting or armed.** Set both Pis to correct UTC before
using matching schedules. These utilities do not provide GPS/PPS synchronization.

## 2. Schedule TX

Activate the UHD 4.9 environment and use the tracked TX-only configuration:

```bash
conda activate uhd-b206
python TxRunHighNoon.py --at "12:00:00" --timezone America/Denver --config config/Tx_B206mini.yaml --dry-run
python TxRunHighNoon.py --at "12:00:00" --timezone America/Denver --config config/Tx_B206mini.yaml
```

This example means noon **today in Denver**. Choose a future time at least 30
seconds ahead, allowing several minutes for the first build. For another day,
specify a full ISO time such as `--at "2026-10-26T12:00:00-06:00"`, replacing the
date as needed. Past times fail; neither runner silently schedules tomorrow.
An explicit offset determines the instant even when `--timezone` differs.

`--dry-run` validates the configuration/time and displays the TX gain without
writing files, building, or contacting the radio. Review the configuration and
existing lab connections before launching the actual transmission.

Without `--config`, TX uses `config/Tx_B206mini.yaml`: **2.4 GHz**, **0 dB TX
gain**, **56 MS/s**, and **1,000 twenty-microsecond chirps at 1 ms spacing**.
The chirp bandwidth is 25 MHz and phase dithering is disabled. Transmission
lasts approximately one second. The runner preserves the selected RF settings.
It requires:

- `RF0.transmit: true`, `RF0.receive: false`;
- `DEVICE.clk_ref: internal`, `cpu_format: fc32`, exactly one TX channel;
- a finite `num_pulses >= 1`, with `pulse_rep_int >= tx_duration > 0`.

The matching RX template records 50 two-millisecond windows at 100 ms spacing,
over approximately five seconds. Start RX one second before TX to collect
windows before, during, and after transmission. The wider windows allow for
different pulse phases; they do not provide precise synchronization.

An updated checkout contains both tracked templates. Previously generated files
under `data/`, including `data/pi_setup/tx.yaml` and `data/tx_loopback.yaml`, keep
their old settings. Use the tracked templates above for the new defaults.

The requested time is for the **first transmitted sample**, followed by the
configured pulse train. `CHIRP.time_offset` and RX-relative `tx_lead` do not
shift that first TX time. This runner generates its chirp, builds the native
program, initializes the SDR early, maps the Pi clock to SDR time near the
deadline, and queues timestamped bursts through UHD. It fails if preparation
runs late instead of transmitting immediately as a fallback.

Each run keeps its configuration and generated waveform in
`data/scheduled_tx/<UTC-time>_<unique-id>/`. On success it also saves a timestamped
configuration and UHD log there; TX-only mode produces no RX sample file.
On failure, inspect `uhd_stdout.log` in the repository root and that run folder.
Ctrl-C stops further scheduling, but bursts already queued in the radio may
still transmit.

## 3. Check the result

The TX log must contain:

```text
[SCHEDULE] First TX burst acknowledged; target device time: ...
[SCHEDULE] TX complete; acknowledged bursts: ...
```

The count must match `num_pulses`. The scheduler keeps at most six bursts
outstanding and requires full sends and UHD burst acknowledgements. A late
deadline, short send, asynchronous TX fault, or missing acknowledgement fails
the run. TX does not retry missed bursts at a later time.

An acknowledgement reports UHD burst completion. The printed target is not a
measured RF emission time or independent UTC timestamp. Pi clock error, USB
clock-query uncertainty, oscillator drift, and hardware delay still affect
absolute timing. Validate timing with suitable hardware before relying on it
to align two SDRs. See [RX scheduling and timing limits](scheduled_rx.md).

The software tests simulate transport acknowledgements/failures and mock all
clock-changing commands. No RF transmission or system-clock change is performed
by those tests; physical timing still needs testing on the Pis.
