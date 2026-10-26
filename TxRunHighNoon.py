#!/usr/bin/env python3
"""Schedule a TX-only pulse train against the Pi clock with UHD timed bursts.

This uses the same approximate host/SDR clock mapping as RxRunHighNoon.py.
Burst acknowledgements detect transport errors; they do not measure UTC accuracy.
"""
import argparse
import math
from pathlib import Path
import sys
import uuid
from zoneinfo import ZoneInfo

from ruamel.yaml import YAML
from high_noon_time import parse_start_time
from RxRunHighNoon import cleanup_failed_run, require_preparation_time

REPO_ROOT = Path(__file__).resolve().parent


def load_tx_config(path):
    """Require TX-only operation and a finite, non-overlapping pulse train."""
    with Path(path).open() as stream:
        config = YAML(typ="safe").load(stream)
    rf, device, chirp = config["RF0"], config["DEVICE"], config["CHIRP"]
    if not rf.get("transmit", True) or rf.get("receive", True):
        raise ValueError("Scheduled TX requires transmit: true and receive: false in RF0.")
    if device["clk_ref"] != "internal" or device.get("cpu_format", "fc32") != "fc32":
        raise ValueError("Scheduled TX requires clk_ref: internal and cpu_format: fc32.")
    if any(key in chirp for key in ("rx_start_utc", "tx_start_utc")):
        raise ValueError("Use the original TX configuration without an existing scheduled time.")
    duration, period = float(chirp["tx_duration"]), float(chirp["pulse_rep_int"])
    pulses = chirp["num_pulses"]
    if (not math.isfinite(duration) or not math.isfinite(period) or duration <= 0
            or period < duration or type(pulses) is not int or pulses < 1):
        raise ValueError("Scheduled TX requires num_pulses >= 1 and positive finite, non-overlapping burst timing.")
    return config


def prepare_runtime_config(config, target, repo=REPO_ROOT):
    """Keep a separate configuration, generated chirp, and logs for every TX run."""
    stamp = target.strftime("%Y%m%dT%H%M%SZ") + "_" + uuid.uuid4().hex[:8]
    run_dir = repo / "data" / "scheduled_tx" / stamp
    run_dir.mkdir(parents=True, exist_ok=False)
    config["CHIRP"]["tx_start_utc"] = target.timestamp()
    config["FILES"].update(output_dir=str(run_dir.relative_to(repo)),
                           chirp_loc="chirp.bin", max_chirps_per_file=-1)
    config["GENERATE"].update(out_file="chirp.bin", show_plot=False)
    config["RUN_MANAGER"].update(save_gps=False, final_save_loc=None, save_partial_files=False)
    config["SCHEDULE"] = {"requested_utc": target.isoformat(), "time_basis": "Pi system clock; no GPS/PPS"}
    destination = run_dir / "scheduled_tx.yaml"
    with destination.open("x") as stream:
        YAML().dump(config, stream)
    return destination


def run_transmission(config_path, target):
    """Generate/build before the deadline, then require first and final TX ACK logs."""
    from runTx import RadarProcessRunner

    confirmations = set()

    def observe(line):
        if line.startswith("[SCHEDULE] First TX burst acknowledged;"):
            confirmations.add("first")
        if line.startswith("[SCHEDULE] TX complete; acknowledged bursts:"):
            confirmations.add("complete")

    runner = RadarProcessRunner(str(config_path.relative_to(REPO_ROOT)), log_processing_function=observe)
    try:
        (REPO_ROOT / "sdr/build").mkdir(parents=True, exist_ok=True)
        runner.setup()
        require_preparation_time(target)
        runner.run()
        runner.wait()
        prefix = runner.stop(timeout=15)
    except BaseException:
        cleanup_failed_run(runner)
        raise
    if confirmations != {"first", "complete"}:
        raise RuntimeError("Scheduled TX acknowledgement confirmation is missing; transmission is not verified.")
    print(f"Saved TX configuration and log: {prefix}")


def main(argv=None):
    """Validate/preview the requested start without silently rolling to tomorrow."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--at", required=True, help="Time today or full ISO date/time; UTC unless a zone/offset is supplied.")
    parser.add_argument("--timezone", default="UTC", help="IANA time zone, e.g. America/Denver (default: UTC).")
    parser.add_argument("--config", type=Path, default=Path("config/Tx_B206mini.yaml"), help="TX-only YAML; use data/pi_setup/tx.yaml for the prepared Pi baseline.")
    parser.add_argument("--dry-run", action="store_true", help="Validate only; no files, build, or radio access.")
    args = parser.parse_args(argv)
    try:
        target = parse_start_time(args.at, args.timezone)
        require_preparation_time(target)
        config = load_tx_config(args.config)
        print("Requested UTC:", target.isoformat())
        print("Requested local:", target.astimezone(ZoneInfo(args.timezone)).isoformat())
        print("TX gain (dB):", config["RF0"]["tx_gain"])
        print("Timing uses the Pi clock plus an estimated USB clock mapping, not GPS/PPS.")
        if args.dry_run:
            return 0
        if Path.cwd().resolve() != REPO_ROOT:
            raise ValueError("Run TxRunHighNoon.py from the repository root.")
        runtime_path = prepare_runtime_config(config, target)
        print("Saved scheduled configuration:", runtime_path)
        run_transmission(runtime_path, target)
    except (ValueError, KeyError, TypeError, OSError, RuntimeError) as exc:
        parser.exit(1, f"Error: {exc}\n")
    except KeyboardInterrupt:
        print("Scheduled TX cancelled; queued bursts may already have reached the SDR.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
