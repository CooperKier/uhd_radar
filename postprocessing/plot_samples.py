from signal import pause
import sys
import argparse
import numpy as np
import scipy.signal as sp
import processing as pr
import matplotlib.pyplot as plt
from ruamel.yaml import YAML as ym
import os



# Check if a YAML file was provided as a command line argument
parser = argparse.ArgumentParser()
parser.add_argument("yaml_file", nargs='?', default='config/default.yaml',
        help='Path to YAML configuration file')

args = parser.parse_args()

import re
str_arg = str(args.yaml_file)
match = re.search(r'(\d{8}_\d{6})', str_arg)
if match:
    timestamp = match.group(1)
else:
    timestamp = str_arg[5:20]


# Initialize Constants
yaml_path = args.yaml_file
if not os.path.exists(yaml_path):
    candidates = [
        re.sub(r'(\.yaml|\.yml)$', r'_config\1', yaml_path),
        yaml_path + "_config.yaml",
        yaml_path + ".yaml",
        os.path.join("data", timestamp + "_config.yaml") if timestamp else None,
        os.path.join(os.path.dirname(yaml_path), timestamp + "_config.yaml") if (timestamp and os.path.dirname(yaml_path)) else None,
    ]
    for cand in candidates:
        if cand and os.path.exists(cand):
            yaml_path = cand
            break

yaml = ym()                         # Always use safe load if not dumping
with open(yaml_path) as stream:
   config = yaml.load(stream)
   rx_params = config["PLOT"]
   sample_rate = rx_params["sample_rate"]    # Hertz
   
   orig_ch = rx_params["orig_chirp"]         # Chirp associated with the received data
   direct_start = rx_params["direct_start"]
   echo_start = rx_params["echo_start"]
   sig_speed = rx_params["sig_speed"]

   print("The timestamp is: ", timestamp)
   print("If the timestamp is incorrect, adjust plot_samples.py code")

   output_dir = config['FILES'].get('output_dir', 'data')
   rx_samps = os.path.join(output_dir, timestamp + "_rx_samps.bin") # Received data to analyze
   if not os.path.exists(rx_samps):
       yaml_dir = os.path.dirname(yaml_path)
       candidate = os.path.join(yaml_dir, timestamp + "_rx_samps.bin")
       if os.path.exists(candidate):
           rx_samps = candidate

   if not os.path.exists(orig_ch) and os.path.exists(os.path.join(output_dir, orig_ch)):
       orig_ch = os.path.join(output_dir, orig_ch)


   
print("--- Loaded constants from config.yaml ---")
# Read and plot RX/TX
rx_sig = pr.extractSig(rx_samps)

# Calculate samples per pulse
rx_duration = config.get("CHIRP", {}).get("rx_duration", 20e-6)
samples_per_pulse = int(rx_duration * sample_rate)
if samples_per_pulse <= 0:
    samples_per_pulse = 1120

# Cap received samples at n pulses
max_pulses = 3000
if len(rx_sig) > max_pulses * samples_per_pulse:
    rx_sig = rx_sig[:max_pulses * samples_per_pulse]

print("--- Plotting real samples read from %s ---" % rx_samps)
pr.plotChirpVsPulses(rx_sig, 'Received Samples', samples_per_pulse)

tx_sig = pr.extractSig(orig_ch)
print("--- Plotting transmited chirp, stored in %s ---" % orig_ch)
pr.plotChirpVsTime(tx_sig, 'Transmitted Chirp', sample_rate)

# Correlate the two chirps to determine time difference
print("--- Match filtering received chirp with transmitted chirp ---")
xcorr_sig = sp.correlate(rx_sig, tx_sig, mode='full', method='auto')
# as finddirectpath is written right now, it must be called before taking log of the signal
# because if not, negative log values could have a greater absolute value than positive log values.
dir_peak = pr.findDirectPath(xcorr_sig, direct_start, True) 

#Test code, Only have one option uncommented--------
#Option 1:
xcorr_sig = 20 * np.log10(np.absolute(xcorr_sig))

#Option 2:
#xcorr_normalized = np.abs(xcorr_sig)
#xcorr_normalized /= np.max(xcorr_normalized)
#xcorr_sig = 20 * np.log10(xcorr_normalized + 0.00000000000000001)
#-------------------------------------------

print("--- Plotting result of match filter ---")
xcorr_samps = np.shape(xcorr_sig)[0]
print(echo_start)
xcorr_time = np.zeros(xcorr_samps)
for x in range (xcorr_samps):
    xcorr_time[x] = x * 1e6 /sample_rate

# plt.figure()
# plt.plot(xcorr_time, xcorr_sig)
# plt.title("Output of Match Filter: Signal")
# plt.xlabel('Time (us)')
# plt.ylabel('Power [dB]')
# plt.grid()

#COLIN TEST CODE----------
numSamps = 2000
xTime = np.arange(-10,numSamps) * 1e6 / sample_rate
    
plt.figure()
plt.plot(xTime, xcorr_sig[dir_peak-10:dir_peak+numSamps])
plt.title("Output of Match Filter: Peaks")
plt.xlabel('Time (us)')
plt.ylabel('Power [dB]')
plt.grid()
plt.ticklabel_format(style='plain', useOffset=False, axis='x')
#-------------------

#Unedited version of above code
# plt.figure()
# #plt.plot(range(-10,10000), xcorr_sig[dir_peak-10:dir_peak+10000])
# plt.title("Output of Match Filter: Peaks")
# plt.xlabel('Sample')
# plt.ylabel('Power [dB]')
# plt.grid()

[echo_samp, echo_dist] = pr.findEcho(xcorr_sig, sample_rate, dir_peak, echo_start, sig_speed, True)

sys.stdout.flush()
#plt.savefig("match_filter.png", dpi=150)
plt.show()