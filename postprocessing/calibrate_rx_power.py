import os
import re
import sys
import argparse
import numpy as np
import matplotlib.pyplot as plt
from ruamel.yaml import YAML as ym

# Import local processing module
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
import processing as pr


def resolve_file(target):
    """Resolve a target (config YAML, data prefix, or binary file) to (bin_path, yaml_path, label)."""
    target_str = str(target).strip()
    match = re.search(r'(\d{8}_\d{6})', target_str)
    timestamp = match.group(1) if match else os.path.basename(target_str)

    bin_path = None
    yaml_path = None

    if target_str.endswith(('.yaml', '.yml')):
        yaml_path = target_str
        prefix = re.sub(r'(_config)?(\.yaml|\.yml)$', '', target_str)
        cand = prefix + "_rx_samps.bin"
        if os.path.exists(cand):
            bin_path = cand
    elif target_str.endswith('.bin'):
        bin_path = target_str
        prefix = re.sub(r'(_rx_samps)?\.bin$', '', target_str)
        cand = prefix + "_config.yaml"
        if os.path.exists(cand):
            yaml_path = cand
    else:
        prefix = target_str
        for suffix in ["_config.yaml", ".yaml"]:
            if os.path.exists(prefix + suffix):
                yaml_path = prefix + suffix
                break
        for suffix in ["_rx_samps.bin", ".bin"]:
            if os.path.exists(prefix + suffix):
                bin_path = prefix + suffix
                break

    if timestamp:
        data_dir = os.path.dirname(target_str) or "data"
        if not bin_path:
            cand = os.path.join(data_dir, f"{timestamp}_rx_samps.bin")
            if os.path.exists(cand):
                bin_path = cand
        if not yaml_path:
            cand = os.path.join(data_dir, f"{timestamp}_config.yaml")
            if os.path.exists(cand):
                yaml_path = cand

    label = timestamp if timestamp else os.path.basename(target_str)
    return bin_path, yaml_path, label


def main():
    parser = argparse.ArgumentParser(description="Plot raw Frequency vs. Relative Power Spectrum (dBFS)")
    parser.add_argument("targets", nargs='*', default=['data/20260912_015300_config.yaml'],
                        help="One or more config YAMLs, data prefixes, or .bin sample files")
    parser.add_argument("--nfft", type=int, default=65536, help="FFT size (default: 65536)")
    args = parser.parse_args()

    plt.figure(figsize=(10, 6))

    yaml_reader = ym()

    for target in args.targets:
        bin_path, yaml_path, label = resolve_file(target)

        if not bin_path or not os.path.exists(bin_path):
            print(f"Warning: Could not locate binary sample file for '{target}', skipping.")
            continue

        sample_rate = 56e6
        fc = 915e6

        if yaml_path and os.path.exists(yaml_path):
            with open(yaml_path) as stream:
                cfg = yaml_reader.load(stream)
            sample_rate = cfg.get("PLOT", {}).get("sample_rate") or cfg.get("GENERATE", {}).get("sample_rate", 56e6)
            fc = cfg.get("RF0", {}).get("freq", 915e6)

        # Load raw IQ samples
        rx_sig = pr.extractSig(bin_path)

        # Compute windowed FFT
        nfft = min(args.nfft, len(rx_sig))
        window = np.hanning(nfft)
        s1 = np.sum(window)

        # Average available segments
        num_segments = min(16, len(rx_sig) // nfft)
        if num_segments > 0:
            psd = np.mean([np.abs(np.fft.fftshift(np.fft.fft(rx_sig[i*nfft:(i+1)*nfft] * window)))**2 for i in range(num_segments)], axis=0)
        else:
            psd = np.abs(np.fft.fftshift(np.fft.fft(rx_sig[:nfft] * window)))**2

        # Explicit power calculation in a 50-ohm system:
        # V_peak is normalized digital magnitude (|X| / S1)
        # V_rms^2 = V_peak^2 / 2
        # P_watts = V_rms^2 / R (explicitly dividing by 50 ohms)
        # P_dBm   = 10 * log10(P_watts / 1 mW)
        R = 50.0  # System impedance in Ohms
        v_peak_sq = psd / (s1 ** 2)
        v_rms_sq = v_peak_sq / 2.0
        p_watts = v_rms_sq / R
        power_dbm = 10 * np.log10(p_watts / 1e-3 + 1e-15)

        freqs_mhz = (np.fft.fftshift(np.fft.fftfreq(nfft, 1.0 / sample_rate)) + fc) / 1e6
        peak_idx = np.argmax(power_dbm)
        peak_freq = freqs_mhz[peak_idx]
        peak_power = power_dbm[peak_idx]

        print(f"[{label}] Peak: {peak_power:+.2f} dBm at {peak_freq:.4f} MHz (Fs: {sample_rate/1e6:.1f} MHz, Fc: {fc/1e6:.1f} MHz)")

        plt.plot(freqs_mhz, power_dbm, label=f"{label} (Peak: {peak_power:+.1f} dBm)")

    plt.title("Frequency vs. Power Spectrum")
    plt.xlabel("Frequency (MHz)")
    plt.ylabel("Power (dBm)")
    plt.grid(True)
    plt.legend(loc="upper right")
    plt.ticklabel_format(style="plain", useOffset=False, axis="x")
    plt.tight_layout()
    plt.show()


if __name__ == "__main__":
    main()
