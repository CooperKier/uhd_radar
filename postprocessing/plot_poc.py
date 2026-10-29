"""Plot a transferred PoC stacked-IQ export without needing the raw capture."""
import argparse
import json
import numpy as np
from scipy import signal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("export_file")
    parser.add_argument("--save", help="Write the figure instead of displaying it")
    args = parser.parse_args()
    if args.save:
        import matplotlib
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    with np.load(args.export_file, allow_pickle=False) as data:
        iq, chirp = data["stacked_iq"], data["chirp"]
        times = data["device_time_s"]
        report = json.loads(str(data["metadata_json"]))
    rate = float(report["config"]["RF0"]["rx_rate"])
    compressed = signal.fftconvolve(iq, chirp.conj()[None, ::-1], mode="valid", axes=1)
    compressed /= np.sum(np.abs(chirp) ** 2)
    delays = np.arange(compressed.shape[1]) * 1e6 / rate
    figure, ax = plt.subplots(figsize=(11, 5))
    image = ax.pcolormesh(times, delays, 20 * np.log10(np.maximum(np.abs(compressed.T), 1e-12)), shading="auto")
    ax.set(xlabel="Scheduled TX device time (s)", ylabel="Delay relative to preamble reference (µs)",
           title=f"PoC matched-filtered radargram — {report['valid_pulses']} pulses")
    figure.colorbar(image, ax=ax, label="Matched-filter amplitude (dB, arbitrary reference)")
    figure.tight_layout()
    if args.save:
        figure.savefig(args.save, dpi=150)
    else:
        plt.show()


if __name__ == "__main__":
    main()
