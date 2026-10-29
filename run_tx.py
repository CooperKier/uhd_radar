"""Run cached TX pulses with independent GPS logging on the Raspberry Pi."""

import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "sdr_Tx" / "build"


def prepare(config):
    """Build only the TX sources and generate the waveform once, without transmitting."""
    subprocess.run(
        ["cmake", "-S", str(ROOT / "sdr_Tx"), "-B", str(BUILD),
         "-DCMAKE_BUILD_TYPE=Release"], check=True,
    )
    subprocess.run(["cmake", "--build", str(BUILD), "--parallel", "2"], check=True)
    # Scientific/plotting dependencies are loaded only during explicit preparation.
    from preprocessing.generate_chirp import generate_from_yaml_filename
    generate_from_yaml_filename(str(config))
    subprocess.run([str(BUILD / "radar_tx"), str(config), str(ROOT), "--validate"], check=True)


def stopProcess(process):
    """Allow TX and GPS to close normally, with a bounded fallback for stuck drivers."""
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def runTx(config, gps_mode, gps_device, log_dir):
    """Supervise separate TX and GPS processes and preserve GPS logs on all exits."""
    run_started = time.perf_counter()
    binary = BUILD / "radar_tx"
    if not binary.is_file():
        raise RuntimeError("TX executable missing. Run this command with --prepare first.")
    subprocess.run([str(binary), str(config), str(ROOT), "--validate"], check=True)
    if gps_mode == "gpsd":
        commands = [
            ["gpspipe", "--json", "-uu", "--output", str(log_dir / "gpspipe_stdout.log")],
            ["gpxlogger", "--reconnect", "-m2", "-f", str(log_dir / "track.gpx"), "-e", "sockets"],
        ]
        for command in commands:
            if shutil.which(command[0]) is None:
                raise RuntimeError(f"{command[0]} is missing; install gpsd-clients on the Pi.")
    else:
        logger = BUILD / "gps_logger"
        if not logger.is_file():
            raise RuntimeError("Serial GPS logger missing. Run --prepare first.")
        commands = [[str(logger), gps_device, str(log_dir / "gps_log.txt")]]

    stopped = False

    def requestStop(signum, frame):
        nonlocal stopped
        stopped = True

    processes = []
    result = 1
    log_dir.mkdir(parents=True, exist_ok=False)
    # Save the exact settings beside each run's GPS logs; no RX data archiving.
    shutil.copyfile(config, log_dir / "config.yaml")
    started = datetime.now(timezone.utc).isoformat()
    print(f"GPS logs: {log_dir}", flush=True)
    handlers = {sig: signal.signal(sig, requestStop) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        with ExitStack() as stack:
            gps_log = stack.enter_context((log_dir / "gps_processes.log").open("w"))
            for command in commands:
                processes.append(subprocess.Popen(command, cwd=ROOT, stdout=gps_log,
                                                  stderr=subprocess.STDOUT, start_new_session=True))
            # GPS runs concurrently: no fixed five-second sleep before TX.
            if stopped:
                result = 130
                return result
            tx = subprocess.Popen([str(binary), str(config), str(ROOT)], cwd=ROOT,
                                  start_new_session=True)
            processes.append(tx)
            while True:
                if stopped:
                    result = 130
                    break
                for process in processes[:-1]:
                    if process.poll() is not None:
                        raise RuntimeError(f"GPS logger exited ({process.returncode}); see {log_dir / 'gps_processes.log'}")
                if tx.poll() is not None:
                    result = tx.returncode
                    break
                time.sleep(0.05)
    finally:
        # Stop TX before GPS so the GPS record covers the full transmission.
        try:
            for process in reversed(processes):
                stopProcess(process)
            (log_dir / "run.json").write_text(json.dumps({
                "started_utc": started,
                "finished_utc": datetime.now(timezone.utc).isoformat(),
                "config": str(config), "gps_mode": gps_mode, "returncode": result,
            }, indent=2) + "\n")
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
            elapsed_seconds = time.perf_counter() - run_started
            print(f"[TX] Total run time: {elapsed_seconds:.3f} seconds", flush=True)
    return result


def main():
    """Provide a preparation step and a quick repeated-run path."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("yaml_file", nargs="?", default="config/tx_b206mini.yaml")
    parser.add_argument("--prepare", action="store_true", help="Build/generate/validate, then exit without TX")
    parser.add_argument("--validate", action="store_true", help="Validate config and cached waveform without hardware")
    parser.add_argument("--gps", choices=("gpsd", "serial"), default="gpsd")
    parser.add_argument("--gps-device", default="/dev/ttyACM0")
    parser.add_argument("--log-dir", type=Path, help="New directory for this run's GPS logs")
    args = parser.parse_args()
    config = Path(args.yaml_file).resolve()
    if not config.is_file():
        parser.error(f"Config does not exist: {config}")
    try:
        if args.prepare:
            prepare(config)
            return 0
        if args.validate:
            subprocess.run([str(BUILD / "radar_tx"), str(config), str(ROOT), "--validate"], check=True)
            return 0
        log_dir = args.log_dir or ROOT / "data" / datetime.now(timezone.utc).strftime("tx_%Y%m%dT%H%M%S_%fZ")
        return runTx(config, args.gps, args.gps_device, log_dir.resolve())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"TX runner: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
