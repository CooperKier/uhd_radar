"""Align and stack a shared-PPS PoC capture without displaying plots."""
import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sdr_common.poc import processCapture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        processCapture(args.capture_dir, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"PoC processing: {error}\n")


if __name__ == "__main__":
    main()
