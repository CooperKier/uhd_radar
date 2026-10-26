## Config Settings
This is where config settings can be set and where default configs are stored. The recommended default settings for the USRP B205mini-i is `default.yaml`. 

For the B206mini, use `Rx_B206mini.yaml` for receive-only operation or
`Tx_B206mini.yaml` for transmit-only operation. These modes are selected by
`RF0.transmit` and `RF0.receive`; settings for a disabled direction are not applied
to the radio. The configurations use an internal clock and do not need GPS or
internet access during acquisition.

The B206mini requires UHD 4.9 or newer. The original `environment.yaml` pins an
older UHD release and should not be used to update a B206mini environment. The
B206mini examples are configured for 56 MS/s, so check USB 3 connectivity and host
performance before acquisition. Their gain values are requests, not calibrated
output powers for the B206mini.

From the repository root, with the B206mini Conda environment active:

```bash
python runRx.py config/Rx_B206mini.yaml
# Starts transmission; use with the intended RF test setup:
python runTx.py config/Tx_B206mini.yaml
```

`GENERATE.out_file` and `FILES.chirp_loc` are relative to `FILES.output_dir`.
Use `chirp.bin` with `output_dir: "data"` to read and write `data/chirp.bin`.
Plots are disabled by default for terminal/SSH use. RX-only runs do not need a
chirp file; TX-only runs save configuration and logs without an RX sample file.
The optional `gpspipe` and `gpxlogger` programs may be absent.

Important Settings:
* `freq` is the center frequency of the output. The default and RX/TX bench templates use `2.4e9` (2.4 GHz).
* `num_pulses` is how many pulses are sent before ending the main loop. Default is 10,000. Can set to -1 to keep going indefinitely until manually stopped.
* `save_loc` is where the SDR data will be temporarily saved. Default is `data/rx_samps.bin` in the data folder.
* `max_chirps_per_file` is how much data will be put in each file before breaking into a new file. Can set to -1 to stop chirps from being split between files, default is -1.
