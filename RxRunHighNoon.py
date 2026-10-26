#!/usr/bin/env python3
"""Schedule RX against the Pi's wall clock using UHD hardware timestamps.

This is host-clock scheduling, not GPS/PPS synchronization. UTC error includes
host-clock error, USB clock-query uncertainty, and subsequent oscillator drift.
"""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import sys
import uuid
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from ruamel.yaml import YAML
from high_noon_time import parse_start_time

REPO_ROOT = Path(__file__).resolve().parent
MIN_PREPARATION_SECONDS = 30


def require_preparation_time(target, now=None):
    """Refuse past/near deadlines instead of silently scheduling tomorrow or late."""
    now = now or datetime.now(timezone.utc)
    if (target - now).total_seconds() < MIN_PREPARATION_SECONDS:
        raise ValueError("Choose a start at least 30 seconds ahead; allow longer for the first build.")


def load_rx_config(path):
    """Read an RX-only internal-clock configuration without changing the source."""
    with Path(path).open() as stream:
        config = YAML(typ="safe").load(stream)
    rf = config["RF0"]
    if rf.get("transmit", True) or not rf.get("receive", True):
        raise ValueError("Scheduled acquisition requires transmit: false and receive: true in RF0.")
    if config["DEVICE"]["clk_ref"] != "internal":
        raise ValueError("This host-clock scheduler currently requires clk_ref: internal.")
    if any(key in config["CHIRP"] for key in ("rx_start_utc", "tx_start_utc")):
        raise ValueError("The source configuration already contains a scheduled time; use the original RX config.")
    return config


def prepare_runtime_config(config, target, repo=REPO_ROOT):
    """Write an isolated run config/output folder, preserving the original YAML."""
    stamp = target.strftime("%Y%m%dT%H%M%SZ") + "_" + uuid.uuid4().hex[:8]
    run_dir = repo / "data" / "scheduled_rx" / stamp
    run_dir.mkdir(parents=True, exist_ok=False)
    config["CHIRP"]["rx_start_utc"] = target.timestamp()
    config["FILES"].update(output_dir=str(run_dir.relative_to(repo)),
                           max_chirps_per_file=-1)
    config["RUN_MANAGER"].update(save_gps=False, final_save_loc=None, save_partial_files=False)
    config["SCHEDULE"] = {"requested_utc": target.isoformat(), "time_basis": "Pi system clock; no GPS/PPS"}
    destination = run_dir / "scheduled_rx.yaml"
    with destination.open("x") as stream:
        YAML().dump(config, stream)
    return destination


def cleanup_failed_run(runner):
    """Clean up an interrupted/failed runner without replacing its original error."""
    try:
        if runner.is_running:
            runner.stop(timeout=15)
        else:
            for process in (runner.gpspipe_process, runner.gpxlogger_process):
                if process is not None:
                    if process.poll() is None:
                        process.kill()
                    process.wait()
    except Exception as exc:
        print(f"Cleanup: {exc}", file=sys.stderr)


def run_capture(config_path, target):
    """Build first, initialize early, and let the radio queue the timed RX command."""
    from runRx import RadarProcessRunner

    first_packet = []

    def observe(line):
        if line.startswith("[SCHEDULE] First RX hardware timestamp:"):
            first_packet.append(line.strip())
    runner = RadarProcessRunner(str(config_path.relative_to(REPO_ROOT)),
                               log_processing_function=observe)
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
    if not first_packet:
        raise RuntimeError("No scheduled first-packet confirmation; this run is not verified as scheduled.")
    print(first_packet[0])
    print(f"Saved capture: {prefix}")


def make_parser():
    """Describe the clock convention and expose a hardware-free preview."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--at", required=True, help="Time today (HH:MM:SS) or full ISO date/time; past times fail.")
    parser.add_argument("--timezone", default="UTC", help="IANA zone for times without an offset (default: UTC).")
    parser.add_argument("--config", type=Path, default=Path("config/Rx_B206mini.yaml"), help="RX-only configuration; use data/pi_setup/rx.yaml for the prepared Pi baseline.")
    parser.add_argument("--dry-run", action="store_true", help="Validate and display the target without building, writing files, or accessing a radio.")
    return parser


def main(argv=None):
    """Validate the request before any build or device access."""
    parser = make_parser()
    args = parser.parse_args(argv)
    try:
        target = parse_start_time(args.at, args.timezone)
        require_preparation_time(target)
        config = load_rx_config(args.config)
        print("Requested UTC:", target.isoformat())
        print("Requested local:", target.astimezone(ZoneInfo(args.timezone)).isoformat())
        print("Timing uses the Pi clock plus an estimated USB clock mapping, not GPS/PPS.")
        if args.dry_run:
            return 0
        if Path.cwd().resolve() != REPO_ROOT:
            raise ValueError("Run RxRunHighNoon.py from the repository root.")
        runtime_path = prepare_runtime_config(config, target)
        print("Saved scheduled configuration:", runtime_path)
        run_capture(runtime_path, target)
    except (ValueError, KeyError, ZoneInfoNotFoundError, OSError, RuntimeError) as exc:
        parser.exit(1, f"Error: {exc}\n")
    except KeyboardInterrupt:
        print("Scheduled RX cancelled; inspect the log before using any partial data.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
