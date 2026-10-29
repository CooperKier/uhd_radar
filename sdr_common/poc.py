"""Headless decoder and bounded-memory IQ stacking for the shared-PPS PoC."""

import csv
import json
from pathlib import Path
import struct
import zlib

import numpy as np
from ruamel.yaml import YAML
from scipy import signal


HEADER_BODY = struct.Struct("<4sHH15I4d")
HEADER_BYTES = HEADER_BODY.size + 4
HEADER_FIELDS = (
    "run_id", "phase_seed", "frame_index", "first_pulse", "pulse_count",
    "total_pulses", "tx_samples", "rx_samples", "pri_samples", "sample_rate",
    "frame_samples", "preamble_samples", "header_sps", "payload_offset", "num_presums",
    "frequency", "frame_time", "chirp_bandwidth", "chirp_length",
)


def loadYaml(path):
    """Load a config/metadata file without retaining YAML-specific Python objects."""
    with Path(path).open() as source:
        return YAML(typ="safe").load(source)


class Protocol:
    """Shared bootstrap geometry; the CRC header must agree with this capture plan."""

    def __init__(self, config):
        self.config = config
        self.sync = config["SYNC"]
        self.rate = int(round(float(config["RF0"]["tx_rate"])))
        chirp = config["CHIRP"]
        self.tx_samples = int(round(self.rate * float(chirp["tx_duration"])))
        self.rx_samples = int(round(self.rate * float(chirp["rx_duration"])))
        self.pri_samples = int(round(self.rate * float(chirp["pulse_rep_int"])))
        self.total_pulses = int(chirp["num_pulses"])
        self.group = int(self.sync["pulses_per_frame"])
        self.stack_count = int(chirp.get("num_presums", 1))
        self.header_sps = int(self.sync.get("header_sps", 8))
        self.start_time = float(self.sync.get("start_time", 5))
        self.dithering = bool(chirp.get("phase_dithering", False))
        if min(self.rate, self.tx_samples, self.rx_samples, self.pri_samples,
               self.total_pulses, self.group, self.stack_count, self.header_sps) < 1:
            raise ValueError("Invalid PoC geometry")
        export_bytes = ((self.total_pulses + self.stack_count - 1) // self.stack_count) * self.rx_samples * 8
        if export_bytes > 256 * 1024 ** 2:
            raise ValueError("PoC stacked output exceeds 256 MiB; increase num_presums or shorten the capture")
        self.preamble = makePreamble(int(self.sync.get("preamble_sps", 4)))
        self.block = self.preamble[:len(self.preamble) // 4]
        self.header_samples = (HEADER_BYTES * 8 + 1) * self.header_sps
        self.payload_offset = len(self.preamble) + self.header_samples + 64
        self.frame_samples = self.payload_offset + self.group * self.pri_samples
        self.frames = (self.total_pulses + self.group - 1) // self.group


def makePreamble(samples_per_chip=4):
    """Reproduce the C++ fixed PN sequence; independent of the radar dither seed."""
    state = 0x1ACEB00C
    chips = []
    for _ in range(31):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        state &= 0xFFFFFFFF
        chips.append(1 if state & 1 else -1)
    return np.tile(np.repeat(np.asarray(chips, dtype=np.complex64), samples_per_chip), 4)


def unpackHeader(data):
    """Validate integrity/version before trusting any transmitted allocation or index."""
    if len(data) != HEADER_BYTES:
        raise ValueError("Wrong header length")
    body, checksum = data[:-4], struct.unpack("<I", data[-4:])[0]
    if zlib.crc32(body) != checksum:
        raise ValueError("Header CRC mismatch")
    magic, version, flags, *values = HEADER_BODY.unpack(body)
    if magic != b"ORCA" or version != 1 or flags & ~1:
        raise ValueError("Unsupported preamble/header protocol")
    result = dict(zip(HEADER_FIELDS, values))
    result.update(protocol_version=version, phase_dithering=bool(flags & 1))
    return result


def validateHeader(header, protocol, expected_index):
    """Require the received run and geometry to match the operator's shared config."""
    p = protocol
    expected = {
        "run_id": int(p.sync["run_id"]), "phase_seed": int(p.sync.get("phase_seed", 0)),
        "frame_index": expected_index, "first_pulse": expected_index * p.group,
        "pulse_count": min(p.group, p.total_pulses - expected_index * p.group),
        "total_pulses": p.total_pulses, "tx_samples": p.tx_samples,
        "rx_samples": p.rx_samples, "pri_samples": p.pri_samples, "sample_rate": p.rate,
        "frame_samples": p.frame_samples, "preamble_samples": len(p.preamble),
        "header_sps": p.header_sps, "payload_offset": p.payload_offset,
        "num_presums": p.stack_count, "phase_dithering": p.dithering,
    }
    for name, value in expected.items():
        if header[name] != value:
            raise ValueError(f"Header/config mismatch for {name}: {header[name]} != {value}")
    expected_floats = {
        "frame_time": p.start_time + expected_index * p.frame_samples / p.rate,
        "frequency": float(p.config["RF0"]["freq"]),
        "chirp_bandwidth": float(p.config["GENERATE"]["chirp_bandwidth"]),
        "chirp_length": float(p.config["GENERATE"]["chirp_length"]),
    }
    for name, value in expected_floats.items():
        if not np.isclose(header[name], value, rtol=1e-10, atol=1e-10):
            raise ValueError(f"Header/config mismatch for {name}")


def decodeAt(samples, protocol):
    """Estimate CFO/phase from repeated PN blocks and decode differential BPSK."""
    p = protocol
    needed = len(p.preamble) + p.header_samples
    if len(samples) < needed:
        raise ValueError("Truncated sync frame")
    sync = np.asarray(samples[:len(p.preamble)], dtype=np.complex128)
    block = len(p.block)
    product = np.vdot(sync[:-block], sync[block:])
    cfo = float(np.angle(product) * p.rate / (2 * np.pi * block))
    positions = np.arange(needed, dtype=np.float64)
    corrected = np.asarray(samples[:needed]) * np.exp(-2j * np.pi * cfo * positions / p.rate)
    reference = np.vdot(p.preamble, corrected[:len(p.preamble)])
    phase = float(np.angle(reference))
    symbols = corrected[len(p.preamble):needed].reshape(-1, p.header_sps).mean(axis=1)
    decisions = (np.real(symbols[1:] * symbols[:-1].conj()) < 0).astype(np.uint8)
    decoded = unpackHeader(np.packbits(decisions, bitorder="big").tobytes())
    # A CRC-valid header provides a longer known signal than the repeated PN
    # blocks. Refine residual CFO before extrapolating phase over the radar group.
    expected_symbols = np.r_[1.0, np.cumprod(1.0 - 2.0 * decisions)]
    residual_phase = np.unwrap(np.angle(symbols * expected_symbols))
    symbol_positions = len(p.preamble) + (np.arange(len(symbols)) + 0.5) * p.header_sps - 0.5
    slope, intercept = np.polyfit(symbol_positions, residual_phase, 1, w=np.abs(symbols))
    cfo += float(slope * p.rate / (2 * np.pi))
    phase = float(intercept)
    return decoded, cfo, phase


def findFrame(raw, protocol, first_device_time, frame_index):
    """Search only near one expected frame; CRC resolves the repeated-block ambiguity."""
    p = protocol
    radius = int(round(float(p.sync.get("search_seconds", 0.002)) * p.rate))
    threshold = float(p.sync.get("correlation_threshold", 0.45))
    if radius < 1 or not 0 < threshold <= 1:
        raise ValueError("Invalid preamble search window/threshold")
    frame_time = p.start_time + frame_index * p.frame_samples / p.rate
    expected = int(round((frame_time - first_device_time) * p.rate))
    low = max(0, expected - radius)
    high = min(len(raw), expected + radius + len(p.preamble) + p.header_samples)
    window = np.asarray(raw[low:high])
    if len(window) < len(p.preamble) + p.header_samples:
        raise ValueError("Capture is too short for the expected sync header")
    correlation = signal.correlate(window, p.block, mode="valid", method="fft")
    power = np.r_[0.0, np.cumsum(np.abs(window).astype(np.float64) ** 2)]
    energy = power[len(p.block):] - power[:-len(p.block)]
    # FFT roundoff in zero-filled pulse gaps must not become a huge normalized peak.
    active = energy > max(float(np.max(energy)) * 1e-6, 1e-20)
    score = np.zeros_like(energy)
    score[active] = np.abs(correlation[active]) / np.sqrt(energy[active] * len(p.block))
    last_candidate = len(window) - len(p.preamble) - p.header_samples
    score = score[:last_candidate + 1]
    peaks, _ = signal.find_peaks(score, height=threshold, distance=max(1, len(p.block) // 2))
    # Include exact boundary candidates, where find_peaks cannot recognize a peak.
    boundaries = [i for i in (0, len(score) - 1) if score[i] >= threshold]
    candidates = sorted(set(peaks.tolist() + boundaries), key=lambda i: -score[i])[:64]
    last_error = "no correlation peak above threshold"
    decoded_error = None
    for candidate in candidates:
        try:
            header, cfo, phase = decodeAt(window[candidate:], p)
        except ValueError as error:
            last_error = str(error)
            continue
        try:
            validateHeader(header, p, frame_index)
            header.update(sample_index=int(low + candidate), cfo_hz=cfo, phase_rad=phase,
                          correlation=float(score[candidate]),
                          arrival_device_time=first_device_time + (low + candidate) / p.rate)
            return header
        except ValueError as error:
            decoded_error = str(error)
    raise ValueError(f"Frame {frame_index}: {decoded_error or last_error}")


def validateTimestamps(capture_dir, metadata, sample_count):
    """Reject missing chunks instead of treating file offsets as contiguous device time."""
    cursor = 0
    first = float(metadata["first_device_time"])
    rate = float(metadata["sample_rate"])
    with (capture_dir / "rx_timestamps.csv").open() as source:
        for row in csv.DictReader(source):
            index, count = int(row["sample_index"]), int(row["sample_count"])
            time = float(row["device_time_s"])
            if index != cursor or count < 1 or not np.isfinite(time) or abs(time - first - index / rate) * rate > 2:
                raise ValueError("RX timestamp discontinuity; coherent processing refused")
            cursor += count
    if cursor != sample_count:
        raise ValueError("Timestamp log does not cover the IQ file")


def processCapture(capture_dir, output=None, chirp_root=None):
    """Decode frames and export aligned, phase-corrected complex averages in small buffers."""
    capture_dir = Path(capture_dir)
    config = loadYaml(capture_dir / "config.yaml")
    metadata = loadYaml(capture_dir / "capture.yaml")
    if not metadata.get("complete"):
        raise ValueError(f"Capture is incomplete: {metadata.get('error', 'unknown capture error')}")
    p = Protocol(config)
    path = capture_dir / "rx_iq.bin"
    size = path.stat().st_size
    if size % 8 or size // 8 != int(metadata["samples"]):
        raise ValueError("IQ file length disagrees with capture metadata")
    if float(metadata["sample_rate"]) != p.rate:
        raise ValueError("Capture sample rate disagrees with the shared config")
    validateTimestamps(capture_dir, metadata, size // 8)
    raw = np.memmap(path, dtype="<c8", mode="r")
    headers, rejected = [], []
    first_time = float(metadata["first_device_time"])
    if not np.isfinite(first_time):
        raise ValueError("Invalid first RX timestamp")
    for index in range(p.frames):
        try:
            headers.append(findFrame(raw, p, first_time, index))
        except ValueError as error:
            rejected.append({"frame_index": index, "reason": str(error)})
    if not headers:
        raise ValueError("No valid preamble/CRC headers found: " + rejected[0]["reason"])
    limit = float(p.sync.get("max_clock_error_ppm", 100)) * 1e-6
    if not np.isfinite(limit) or limit <= 0:
        raise ValueError("max_clock_error_ppm must be positive and finite")
    for index, header in enumerate(headers):
        if len(headers) == 1:
            ratio = 1.0
        else:
            left, right = (header, headers[index + 1]) if index + 1 < len(headers) else (headers[index - 1], header)
            ratio = (right["sample_index"] - left["sample_index"]) / (
                (right["frame_index"] - left["frame_index"]) * p.frame_samples)
        if abs(ratio - 1) > limit:
            raise ValueError("Measured frame timing exceeds max_clock_error_ppm")
        header["sample_clock_ratio"] = float(ratio)

    # Generate the legacy MT19937 sequence by pulse index, so missing frames do not shift it.
    phases = np.zeros(p.total_pulses, dtype=np.float32)
    if p.dithering:
        phases = np.random.RandomState(int(p.sync.get("phase_seed", 0))).randint(
            0, 2 ** 32, size=p.total_pulses, dtype=np.uint32).astype(np.float32)
    stacks, pulse_indices, counts, times = [], [], [], []
    accumulator = np.zeros(p.rx_samples, dtype=np.complex128)
    active_group, accumulated, first_pulse, first_pulse_time = None, 0, 0, 0.0

    def flush():
        if accumulated:
            stacks.append((accumulator / accumulated).astype(np.complex64))
            pulse_indices.append(first_pulse)
            counts.append(accumulated)
            times.append(first_pulse_time)

    for header in headers:
        start = header["sample_index"]
        ratio = header["sample_clock_ratio"]
        end = min(len(raw), int(np.ceil(start + p.frame_samples * ratio + 2)))
        frame = np.asarray(raw[start:end])
        source_positions = np.arange(len(frame))
        for pulse in range(header["pulse_count"]):
            number = header["first_pulse"] + pulse
            group = number // p.stack_count
            if group != active_group:
                flush()
                accumulator.fill(0)
                accumulated = 0
                active_group, first_pulse = group, number
                first_pulse_time = header["frame_time"] + (p.payload_offset + pulse * p.pri_samples) / p.rate
            positions = (p.payload_offset + pulse * p.pri_samples + np.arange(p.rx_samples)) * ratio
            if positions[-1] > len(frame) - 1:
                raise ValueError("Capture truncates a radar pulse")
            iq = np.interp(positions, source_positions, frame.real) + 1j * np.interp(positions, source_positions, frame.imag)
            rotation = header["phase_rad"] + 2 * np.pi * header["cfo_hz"] * positions / p.rate + float(phases[number])
            accumulator += iq * np.exp(-1j * rotation)
            accumulated += 1
    flush()
    # Keep the reference waveform with the export so the desktop can matched-filter it.
    root = Path(chirp_root) if chirp_root else Path(__file__).resolve().parents[1]
    chirp_path = root / config["FILES"].get("output_dir", "data") / config["FILES"]["chirp_loc"]
    chirp = np.fromfile(chirp_path, dtype="<c8")
    if len(chirp) != p.tx_samples:
        raise ValueError("Prepared reference chirp has the wrong length")
    report = {
        "protocol_version": 1, "timebase": metadata.get("timebase"),
        "config": config, "headers": headers, "rejected_frames": rejected,
        "valid_pulses": int(sum(counts)), "stacked_records": len(stacks),
        "stack_counts": counts, "phase_corrected": True,
        "dithering_removed": p.dithering,
        "timing_reference": "preamble arrival; pulse times are scheduled TX device times",
    }
    output = Path(output) if output else capture_dir / "stacked_iq.npz"
    if output.suffix != ".npz":
        output = Path(str(output) + ".npz")
    if output.exists():
        raise FileExistsError(f"Export already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(output, stacked_iq=np.asarray(stacks), pulse_indices=np.asarray(pulse_indices),
                        stack_counts=np.asarray(counts), device_time_s=np.asarray(times),
                        fast_time_s=np.arange(p.rx_samples) / p.rate, chirp=chirp,
                        metadata_json=np.asarray(json.dumps(report)))
    (capture_dir / "sync_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"[RX] Decoded {len(headers)}/{p.frames} frames; {sum(counts)} pulses -> {len(stacks)} stacked records.")
    print(f"[RX] Export: {output}")
    return output, report
