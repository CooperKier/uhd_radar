"""Prepare or run the B205 RX-only / B206 TX-only shared-PPS radar."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "bistatic" / "build"


def positiveLoops(value):
    """A supplied limit counts completed local radar cycles; omission means forever."""
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("--loops must be a positive integer")
    return number


def commandFor(role, config, output, loops=None, device_args=None, validate=False):
    """Construct an explicit role command without editing the shared experiment YAML."""
    command = [str(BUILD / f"bistatic_{role}"), str(config), str(ROOT), str(output)]
    if loops is not None:
        command.extend(["--loops", str(loops)])
    if device_args:
        command.extend(["--device-args", device_args])
    if validate:
        command.append("--validate")
    return command


def prepare(role, config):
    """Compile one role, regenerate its chirp, and validate without opening hardware."""
    subprocess.run(["cmake", "-S", str(ROOT / "bistatic"), "-B", str(BUILD),
                    "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON", "-DBISTATIC_BUILD_RADIO=ON"], check=True)
    subprocess.run(["cmake", "--build", str(BUILD), "--parallel", "2",
                    "--target", f"bistatic_{role}", "test_bistatic_protocol"], check=True)
    subprocess.run(["ctest", "--test-dir", str(BUILD), "--output-on-failure"], check=True)
    from preprocessing.generate_chirp import generate_from_yaml_filename
    generate_from_yaml_filename(str(config))
    subprocess.run(commandFor(role, config, ROOT / "data", validate=True), check=True)


def runStation(role, config, output, loops=None, device_args=None):
    """Stream status to a persistent log and forward termination to the radio process."""
    command = commandFor(role, config, output, loops, device_args)
    subprocess.run(commandFor(role, config, output, loops, device_args, validate=True), check=True)
    output.mkdir(parents=True, exist_ok=False)
    # Preserve bytes, including aliases/comments, as the top-level experiment record.
    (output / "config.yaml").write_bytes(config.read_bytes())
    process = None
    stopped = False
    started = datetime.now(timezone.utc).isoformat()
    result = 1
    kill_timer = None

    def signalProcess(signum):
        if process is not None and process.poll() is None:
            try:
                os.killpg(process.pid, signum)
            except ProcessLookupError:
                pass

    def requestStop(signum, frame):
        nonlocal stopped, kill_timer
        stopped = True
        signalProcess(signal.SIGINT)
        if kill_timer is None:
            kill_timer = threading.Timer(5, signalProcess, args=(signal.SIGKILL,))
            kill_timer.daemon = True
            kill_timer.start()

    handlers = {sig: signal.signal(sig, requestStop) for sig in (signal.SIGINT, signal.SIGTERM)}
    print(f"[{role.upper()}] Output: {output}", flush=True)
    print("Keep the shared 1 Hz PPS running. RF sync and radar travel from TX to RX only.", flush=True)
    try:
        with (output / "radio.log").open("w") as log:
            if stopped:
                result = 130
                return 130
            process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, bufsize=1, start_new_session=True)
            if stopped:
                signalProcess(signal.SIGINT)
            for line in process.stdout:
                print(line, end="", flush=True)
                log.write(line)
                log.flush()
            result = process.wait()
            if stopped:
                result = 130
    finally:
        if kill_timer is not None:
            kill_timer.cancel()
        if process is not None:
            if process.poll() is None:
                signalProcess(signal.SIGINT)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    signalProcess(signal.SIGKILL)
                    process.wait()
            if process.stdout:
                process.stdout.close()
        (output / "run.json").write_text(json.dumps({
            "role": role, "config": str(config), "device_args_override": device_args,
            "loops": loops, "started_utc": started, "finished_utc": datetime.now(timezone.utc).isoformat(),
            "returncode": result,
        }, indent=2) + "\n")
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=("rx", "tx"))
    parser.add_argument("config", nargs="?", type=Path, default=ROOT / "config" / "default.yaml")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--prepare", action="store_true", help="Build, generate, test and validate without hardware")
    mode.add_argument("--validate", action="store_true", help="Validate cached waveform/config without hardware")
    parser.add_argument("--loops", type=positiveLoops, help="Completed local cycles; omitted means forever")
    parser.add_argument("--device-args", help="Override UHD selection, e.g. type=b200,serial=12345")
    parser.add_argument("--output-dir", type=Path, help="New output directory; existing directories are refused")
    args = parser.parse_args(argv)
    config = args.config.resolve()
    if not config.is_file():
        parser.error(f"Missing config: {config}")
    try:
        if args.prepare:
            prepare(args.role, config)
            return 0
        binary = BUILD / f"bistatic_{args.role}"
        if not binary.is_file():
            raise RuntimeError(f"Missing {binary.name}; run --prepare first")
        output = (args.output_dir or ROOT / "data" / datetime.now(timezone.utc).strftime(f"bistatic_{args.role}_%Y%m%dT%H%M%S_%fZ")).resolve()
        if args.validate:
            subprocess.run(commandFor(args.role, config, output, args.loops, args.device_args, validate=True), check=True)
            return 0
        return runStation(args.role, config, output, args.loops, args.device_args)
    except (OSError, ValueError, RuntimeError, ImportError, subprocess.CalledProcessError) as error:
        print(f"Bistatic radar: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
