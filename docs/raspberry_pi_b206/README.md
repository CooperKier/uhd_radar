# Raspberry Pi 5 + B206mini setup

A terminal-only guide for **Ubuntu 24.04, 64-bit ARM**.

You need internet for installation, adequate Pi power/cooling, and a **USB 3 Type-A-to-USB-C data cable**. B206mini uses USB-C; B205mini uses Micro-B. Allow about 20 GB of free storage.

## 1. Connect to the new Pi and clone the code

On your Mac, substitute the new Pi's actual username and hostname/IP:

```bash
ssh NEW_USERNAME@NEW_PI_HOST_OR_IP
```

On the Pi:

```bash
uname -m
sudo apt-get update
sudo apt-get install -y git
mkdir -p ~/CapstoneORCA
cd ~/CapstoneORCA
git clone https://github.com/CooperKier/uhd_radar.git
cd uhd_radar
```

`uname -m` must show `aarch64`. The fixes are included in this repository; no separate patch is needed. Keep an existing checkout and its local changes if one is already present instead of cloning over it. Do not copy another computer's `sdr/build` directory.

## 2. Install the environment

```bash
bash docs/raspberry_pi_b206/bootstrap-pi.sh
```

This installs the basic tools and creates `uhd-b206`. Follow the activation commands it prints. For a new Miniforge installation:

```bash
source ~/miniforge3/etc/profile.d/conda.sh
conda activate uhd-b206
python docs/raspberry_pi_b206/prepare-configs.py --repo .
```

The environment uses **Python 3.11, UHD 4.9, Boost 1.88 and yaml-cpp 0.8**. Do not update it from the repository's original environment files: they require older UHD. Existing environments and local validation configurations are preserved; check them before reusing them.

## 3. Build and test

With `uhd-b206` active, from the repository root:

```bash
cmake -S sdr -B sdr/build -DCMAKE_PREFIX_PATH="$CONDA_PREFIX"
cmake --build sdr/build --parallel 2
ctest --test-dir sdr/build --output-on-failure -E 'gpsLock|check10MhzLock|checkAndSetTime|detectChannels|setRFParams|refLoLockDetect|setupGpio'
python -m pytest tests/ -q
```

Expect **17 C++ software tests and 18 Python tests** to pass. Use `--parallel 1` if memory is limited. Hardware/GPS tests are excluded.

## 4. Connect and detect the radio

```bash
uhd_images_downloader
sudo install -m 0644 "$CONDA_PREFIX/lib/uhd/utils/uhd-usrp.rules" /etc/udev/rules.d/uhd-usrp.rules
sudo udevadm control --reload-rules
```

Connect the B206mini directly to a **blue Pi USB port**, then unplug/reconnect it after installing the rules. Run:

```bash
lsusb -t
uhd_find_devices
uhd_usrp_probe
```

The **SDR's interface rows** must show `5000M` or faster. A `5000M` root hub with the SDR under `480M` is still USB 2. Try another USB 3 cable/blue port if needed. Do not run the radar program as root.

## 5. Test receive-only operation

Connect a **50-ohm load to RX2**. The TX/RX port should remain properly terminated or connected through the attenuator to the analyser.

```bash
set -o pipefail
python runRx.py data/pi_setup/rx.yaml 2>&1 | tee "data/pi_setup/rx-console-$(date +%Y%m%d-%H%M%S).log"
```

Expect **1,000 accepted receive windows, zero receiver errors**, and saved data under `data/pi_setup/rx/`. The configuration uses **1 ms pulse spacing and a 3 s startup delay**. With 1,120 complex samples per window, the IQ file should be **8,960,000 bytes**. A correct file size alone does not prove clean timing; check the log too.

Missing GPS messages are expected. Leave external serial GPS disconnected: that separate source path has an unresolved buffer defect. Internal-clock operation does not need Wi-Fi after setup.

## 6. Test transmit-only operation

Connect **TX/RX → 30 dB attenuator → spectrum analyser**. Leave RX2 terminated and check the analyser's input rating. The prepared TX configuration starts at **0 dB TX gain**, not the template's 65 dB.

Set the analyser to **450 MHz center, 50 MHz span, Max Hold and 100 ms sweep time**. Clear the old trace, then run:

```bash
python runTx.py data/pi_setup/tx.yaml 2>&1 | tee "data/pi_setup/tx-console-$(date +%Y%m%d-%H%M%S).log"
```

Look for the chirp around 450 MHz. Its nominal sweep is 437.5–462.5 MHz; the Kaiser window tapers the edges. TX-only saves configuration/logs but no RX sample file.

The full console should have no `L` (late), `U` (underflow), or `S` (sequence-error) markers. The program's completion message alone does not prove every burst transmitted correctly. An analyser peak marker is not necessarily total or in-pulse chirp power. Account for the external pad without adding the analyser's already-corrected internal attenuation again.

## If something fails

| Problem | First check |
|---|---|
| No devices found | UHD 4.9+, images, USB cable/speed, rules and Pi power |
| CMake mentions `/Users/...` | Move the old build folder aside and configure again on the Pi |
| Missing Boost/yaml-cpp/UHD libraries | Activate `uhd-b206` and reconfigure |
| Late commands or repeated `L` | Confirm 1 ms pulse spacing, 3 s startup delay and USB 3 speed |
| Thread-priority warning | Nonfatal, but may affect timing; resolve before claiming reliable performance |

Keep the saved configuration, full console log and analyser screenshot for each test. Repeat the runs to check consistency.

**Validation:** all 17 C++ and 18 Python software tests passed in the cloud. The ARM dependency recipe resolved successfully. Actual installation and RF validation still need to pass on the new Pi. The earlier Pi demonstrated RX operation, but fully clean TX timing and loopback remain to be established. Do not connect TX directly to RX; loopback needs verified attenuation and input-power margin.
