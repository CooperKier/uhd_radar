#!/usr/bin/env python3
"""Set the Pi's Linux system clock manually, or enable and verify network time.

Uses the Python standard library and Ubuntu's timedatectl/date commands.
Run clock changes with sudo before starting either High Noon runner.
"""
import argparse
from datetime import datetime, timezone
import os
import subprocess
import sys
import time

from high_noon_time import parse_start_time


def command(arguments):
    """Run a bounded, shell-free system command and retain useful error details."""
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=15)
    if result.returncode:
        raise RuntimeError(f"{' '.join(arguments)}: {result.stderr.strip() or result.stdout.strip()}")
    return result.stdout.strip()


def parse_clock_time(value, zone):
    """Require the date too, since a new Pi may have an incorrect current date."""
    if "T" not in value and " " not in value:
        raise ValueError("Include the full date and time, e.g. YYYY-MM-DDTHH:MM:SSZ.")
    return parse_start_time(value, zone)


def set_manual_time(target):
    """Disable managed NTP, set UTC, and verify the result against elapsed time."""
    ntp_was_enabled = command(["timedatectl", "show", "--property=NTP", "--value"]) == "yes"
    command(["timedatectl", "set-ntp", "false"])
    started = time.monotonic()
    try:
        command(["date", "--utc", "--set", target.isoformat()])
    except (OSError, RuntimeError, subprocess.TimeoutExpired):
        if ntp_was_enabled:
            try:
                command(["timedatectl", "set-ntp", "true"])
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
                print(f"Could not restore NTP after failure: {exc}", file=sys.stderr)
        raise
    current = datetime.now(timezone.utc)
    if abs((current - target).total_seconds()) > 2.0 + time.monotonic() - started:
        raise RuntimeError("Clock verification failed after setting it; inspect date -u and timedatectl status.")
    print("Pi UTC:", current.isoformat())
    print("Manual setting verified. Network time remains disabled; use --ntp to re-enable it later.")


def synchronize_network_time(timeout):
    """Enable the configured network time service and wait for reported sync."""
    command(["timedatectl", "set-ntp", "true"])
    deadline = time.monotonic() + timeout
    while True:
        if command(["timedatectl", "show", "--property=NTPSynchronized", "--value"]) == "yes":
            print("Network time reports synchronized. Pi UTC:", datetime.now(timezone.utc).isoformat())
            return
        if time.monotonic() >= deadline:
            raise RuntimeError("Network time did not synchronize before timeout; check internet/time service. NTP remains enabled.")
        time.sleep(0.5)


def main(argv=None):
    """Preview without privileges; change the clock only for an explicit request."""
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--at", help="Full date/time; defaults to UTC unless a zone/offset is supplied. Disables NTP.")
    mode.add_argument("--ntp", action="store_true", help="Enable network time and wait for synchronization; needs network access.")
    parser.add_argument("--timezone", default="UTC", help="IANA zone for a manual time without an offset; default UTC.")
    parser.add_argument("--timeout", type=float, default=30, help="Seconds to wait for network time (default 30).")
    parser.add_argument("--dry-run", action="store_true", help="Preview only; do not invoke system commands or change the clock.")
    args = parser.parse_args(argv)
    try:
        target = parse_clock_time(args.at, args.timezone) if args.at else None
        if not 0 < args.timeout <= 300:
            raise ValueError("--timeout must be between 0 and 300 seconds.")
        if target is not None:
            print("Requested Pi UTC:", target.isoformat())
            print("Manual mode disables network time and sets the supplied instant when the command runs.")
        else:
            print("Enable the configured network time service and wait for synchronization.")
        if args.dry_run:
            return 0
        if not hasattr(os, "geteuid") or os.geteuid() != 0:
            raise PermissionError("Run with sudo python3 set_pi_time.py ... to change the Pi clock.")
        if target is not None:
            set_manual_time(target)
        else:
            synchronize_network_time(args.timeout)
    except (ValueError, KeyError, OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        parser.exit(1, f"Error: {exc}\n")
    except KeyboardInterrupt:
        print("Interrupted; inspect date -u and timedatectl status before scheduling.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
