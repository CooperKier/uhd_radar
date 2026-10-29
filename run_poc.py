"""Prepare or run the separate TX/RX shared-PPS bench PoC."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import threading
import time

from run_tx import stopProcess

ROOT = Path(__file__).resolve().parent


def binaryFor(role):
    """Keep receiver and transmitter executables in independent build directories."""
    directory, executable = ("sdr_Tx", "radar_tx_poc") if role == "tx" else ("sdr_Rx", "radar_rx")
    return ROOT / directory / "build" / executable


def commandFor(role, config, output_dir, device_args=None, validate=False):
    """Construct an explicit role command; runtime device selection does not edit the shared config."""
    command = [str(binaryFor(role)), str(config), str(ROOT)]
    if role == "rx":
        command.append(str(output_dir))
    if device_args:
        command.extend(["--device-args", device_args])
    if validate:
        command.append("--validate")
    return command


def prepare(role, config):
    """Build the selected role and generate the same cached reference chirp on either Pi."""
    binary = binaryFor(role)
    subprocess.run(["cmake", "-S", str(binary.parent.parent), "-B", str(binary.parent),
                    "-DCMAKE_BUILD_TYPE=Release"], check=True)
    subprocess.run(["cmake", "--build", str(binary.parent), "--parallel", "2",
                    "--target", binary.name], check=True)
    from preprocessing.generate_chirp import generate_from_yaml_filename
    generate_from_yaml_filename(str(config))
    subprocess.run(commandFor(role, config, ROOT / "data", validate=True), check=True)


def supervise(role, config, output_dir, device_args=None, gps="none"):
    """Tee radio status to the terminal/log and stop GPS and radio processes together."""
    binary = binaryFor(role)
    if not binary.is_file():
        raise RuntimeError(f"Missing {binary.name}; run --prepare first.")
    subprocess.run(commandFor(role, config, output_dir, device_args, validate=True), check=True)
    gps_commands = []
    if gps == "gpsd":
        gps_commands = [
            ["gpspipe", "--json", "-uu", "--output", str(output_dir / "gpspipe_stdout.log")],
            ["gpxlogger", "--reconnect", "-m2", "-f", str(output_dir / "track.gpx"), "-e", "sockets"],
        ]
        if any(shutil.which(command[0]) is None for command in gps_commands):
            raise RuntimeError("Install gpsd-clients for --gps gpsd.")
    output_dir.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(config, output_dir / "config.yaml")
    processes = []
    stopped = False
    result = 1
    reader = None
    reader_errors = []
    started = time.perf_counter()
    started_utc = datetime.now(timezone.utc).isoformat()

    def requestStop(signum, frame):
        nonlocal stopped
        stopped = True

    def readOutput(process, logfile):
        try:
            for line in iter(process.stdout.readline, ""):
                print(line, end="", flush=True)
                logfile.write(line)
                logfile.flush()
        except Exception as error:
            reader_errors.append(error)

    handlers = {sig: signal.signal(sig, requestStop) for sig in (signal.SIGINT, signal.SIGTERM)}
    print(f"[{role.upper()}] Run directory: {output_dir}", flush=True)
    print("[SYNC] Keep generator PPS output DISABLED until BOTH terminals report ARMED.", flush=True)
    try:
        with (output_dir / "radio.log").open("w") as logfile, (output_dir / "gps_processes.log").open("w") as gps_log:
            try:
                for command in gps_commands:
                    processes.append(subprocess.Popen(command, cwd=ROOT, stdout=gps_log,
                                                      stderr=subprocess.STDOUT, start_new_session=True))
                radio = subprocess.Popen(commandFor(role, config, output_dir, device_args), cwd=ROOT,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True, bufsize=1, start_new_session=True)
                processes.append(radio)
                reader = threading.Thread(target=readOutput, args=(radio, logfile), daemon=True)
                reader.start()
                while True:
                    if stopped:
                        result = 130
                        break
                    if reader_errors:
                        raise RuntimeError(f"Radio log reader failed: {reader_errors[0]}")
                    for process in processes[:-1]:
                        if process.poll() is not None:
                            raise RuntimeError("GPS logger exited; see gps_processes.log.")
                    if radio.poll() is not None:
                        result = radio.returncode
                        break
                    time.sleep(0.05)
            finally:
                for process in reversed(processes):
                    stopProcess(process)
                if reader:
                    reader.join(timeout=3)
                if reader_errors:
                    result = 1
    finally:
        elapsed = time.perf_counter() - started
        try:
            (output_dir / "run.json").write_text(json.dumps({
                "role": role, "started_utc": started_utc,
                "finished_utc": datetime.now(timezone.utc).isoformat(),
                "duration_seconds": elapsed, "returncode": result,
                "config": str(config), "device_args_override": device_args, "gps_mode": gps,
            }, indent=2) + "\n")
        finally:
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
            print(f"[{role.upper()}] Total acquisition run time: {elapsed:.3f} seconds", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=("tx", "rx"))
    parser.add_argument("yaml_file", nargs="?", default="config/poc_shared_pps.yaml")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--prepare", action="store_true", help="Build, generate and validate; no hardware opened")
    mode.add_argument("--validate", action="store_true", help="Validate without hardware")
    parser.add_argument("--device-args", help="Select the local USRP, e.g. type=b200,serial=12345")
    parser.add_argument("--output-dir", type=Path, help="New run directory; existing directories are refused")
    parser.add_argument("--gps", choices=("none", "gpsd"), default="none",
                        help="Optional location logging; PPS time comes from the generator")
    parser.add_argument("--no-process", action="store_true", help="RX: retain raw capture without automatic stacking")
    args = parser.parse_args()
    config = Path(args.yaml_file).resolve()
    if not config.is_file():
        parser.error(f"Missing config: {config}")
    try:
        if args.prepare:
            prepare(args.role, config)
            return 0
        output = args.output_dir or ROOT / "data" / datetime.now(timezone.utc).strftime(f"poc_{args.role}_%Y%m%dT%H%M%S_%fZ")
        output = output.resolve()
        if args.validate:
            subprocess.run(commandFor(args.role, config, output, args.device_args, validate=True), check=True)
            return 0
        result = supervise(args.role, config, output, args.device_args, args.gps)
        if result == 0 and args.role == "rx" and not args.no_process:
            from sdr_common.poc import processCapture
            began = time.perf_counter()
            processCapture(output)
            print(f"[RX] Processing time: {time.perf_counter() - began:.3f} seconds")
        return result
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"PoC runner: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
