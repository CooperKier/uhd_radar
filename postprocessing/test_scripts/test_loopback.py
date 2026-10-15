import loopback_testing as lt
import argparse
import sys

sys.path.append('postprocessing')
import processing as pr

from ruamel.yaml import YAML as ym

# Check if a YAML file was provided as a command line argument
parser = argparse.ArgumentParser()
parser.add_argument("yaml_file", nargs='?', default='config/default.yaml',
        help='Path to YAML configuration file')

args = parser.parse_args()

import re, os
str_arg = str(args.yaml_file)
print("original string is:", str_arg)
match = re.search(r'(\d{8}_\d{6})', str_arg)
if match:
    timestamp = match.group(1)
else:
    timestamp = str_arg[5:20]
print("timestamp is:", timestamp)

# Initialize Constants
yaml = ym()                         # Always use safe load if not dumping
with open(args.yaml_file) as stream:
   config = yaml.load(stream)
   rx_params = config["PLOT"]
   sample_rate = rx_params["sample_rate"]    # Hertz
   
   orig_ch = rx_params["orig_chirp"]         # Chirp associated with the received data
   direct_start = rx_params["direct_start"]
   echo_start = rx_params["echo_start"]
   sig_speed = rx_params["sig_speed"]

   output_dir = config['FILES'].get('output_dir', 'data') 
   rx_samps = os.path.join(output_dir, timestamp + "_rx_samps.bin") # Received data to analyze
   prefix = os.path.join(output_dir, timestamp) # Format neccessary for loading data
   if not os.path.exists(rx_samps):
       yaml_dir = os.path.dirname(args.yaml_file)
       candidate = os.path.join(yaml_dir, timestamp + "_rx_samps.bin")
       if os.path.exists(candidate):
           rx_samps = candidate
           prefix = os.path.join(yaml_dir, timestamp)

   rx_sig = pr.extractSig(rx_samps)
   if not os.path.exists(orig_ch) and os.path.exists(os.path.join(output_dir, orig_ch)):
       orig_ch = os.path.join(output_dir, orig_ch)
   tx_sig = pr.extractSig(orig_ch)
   print("Loaded data")

   print("rx size is first, tx size is second")
   print(rx_sig.shape)
   print(tx_sig.shape)

   lt.plot_chirp(tx_sig, sample_rate)
   print("Plotting chirp")

   print("running main") #Chris' loopback_testing code 
   lt.main(prefix)