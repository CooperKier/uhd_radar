#!/usr/bin/env python3
"""Create terminal-friendly RX/TX validation configs without changing the templates."""
import argparse
from pathlib import Path
from ruamel.yaml import YAML

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo', type=Path, default=Path.home() / 'CapstoneORCA' / 'uhd_radar')
args = parser.parse_args()
repo = args.repo.expanduser().resolve()
yaml = YAML()
folder = repo / 'data' / 'pi_setup'
folder.mkdir(parents=True, exist_ok=True)
for mode, template in [('rx', 'Rx_B206mini.yaml'), ('tx', 'Tx_B206mini.yaml')]:
    destination = folder / f'{mode}.yaml'
    if destination.exists():
        print(f'Preserved existing configuration: {destination}')
        continue
    with (repo / 'config' / template).open() as stream:
        cfg = yaml.load(stream)
    cfg['GENERATE'].update(out_file='chirp.bin', show_plot=False)
    cfg['RF0'].update(transmit=(mode == 'tx'), receive=(mode == 'rx'), tx_gain=0)
    cfg['CHIRP'].update(time_offset=3.0, pulse_rep_int=1e-3, num_pulses=1000, num_presums=1)
    output = folder / mode
    output.mkdir(exist_ok=True)
    cfg['FILES'].update(output_dir=str(output.relative_to(repo)), chirp_loc='chirp.bin',
                        save_loc='rx_samps.bin', gps_loc='gps_log.txt', max_chirps_per_file=-1)
    cfg['RUN_MANAGER'].update(save_gps=False, final_save_loc=None, save_partial_files=False)
    with destination.open('x') as stream:
        yaml.dump(cfg, stream)
    print(f'Created {destination}')
