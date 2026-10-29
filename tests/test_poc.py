"""Exercise real C++ framing through simulated RF and the Python RX/export path."""
import csv
import json
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
from ruamel.yaml import YAML

import run_poc
from sdr_common.poc import Protocol, loadYaml, processCapture


ROOT = Path(__file__).resolve().parents[1]
TX = ROOT / "sdr_Tx" / "build" / "radar_tx_poc"
RX = ROOT / "sdr_Rx" / "build" / "radar_rx"


class TestRoleCommands(unittest.TestCase):
    def test_rx_cannot_select_tx_executable(self):
        command = run_poc.commandFor("rx", Path("shared.yaml"), Path("capture"), "serial=RX", True)
        self.assertEqual(Path(command[0]).parent.parent.name, "sdr_Rx")
        self.assertEqual(Path(command[0]).name, "radar_rx")
        self.assertEqual(command[3], "capture")
        self.assertIn("--validate", command)

    def test_tx_selects_framed_transmitter(self):
        command = run_poc.commandFor("tx", Path("shared.yaml"), Path("unused"))
        self.assertEqual(Path(command[0]).name, "radar_tx_poc")
        self.assertNotIn("unused", command)


class TestPocSupervisor(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.config = self.directory / "shared.yaml"
        self.config.write_text("shared config snapshot\n")
        self.binary = self.directory / "radio"
        self.binary.touch()
        self.output = self.directory / "run"
        for mocker in (patch.object(run_poc, "binaryFor", return_value=self.binary),
                       patch.object(run_poc.subprocess, "run"),
                       patch.object(run_poc.shutil, "which", return_value="/usr/bin/gpspipe")):
            mocker.start()
            self.addCleanup(mocker.stop)

    @staticmethod
    def process(returncode=0):
        from unittest.mock import Mock
        process = Mock()
        process.returncode = returncode
        process.poll.return_value = returncode
        process.stdout = io.StringIO("[SYNC] ARMED\n")
        return process

    def test_bench_run_does_not_require_gps(self):
        radio = self.process()
        with patch.object(run_poc.subprocess, "Popen", return_value=radio) as spawn:
            result = run_poc.supervise("rx", self.config, self.output)
        self.assertEqual(result, 0)
        self.assertEqual(spawn.call_count, 1)
        self.assertIn("ARMED", (self.output / "radio.log").read_text())
        self.assertEqual(json.loads((self.output / "run.json").read_text())["role"], "rx")

    def test_gps_failure_stops_radio(self):
        gps, track, radio = self.process(7), self.process(None), self.process(None)
        with patch.object(run_poc.subprocess, "Popen", side_effect=[gps, track, radio]):
            with self.assertRaisesRegex(RuntimeError, "GPS logger exited"):
                run_poc.supervise("tx", self.config, self.output, gps="gpsd")
        radio.terminate.assert_called_once()
        track.terminate.assert_called_once()

    def test_existing_run_is_not_overwritten(self):
        self.output.mkdir()
        (self.output / "sentinel").write_text("keep this run")
        with patch.object(run_poc.subprocess, "Popen") as spawn:
            with self.assertRaises(FileExistsError):
                run_poc.supervise("rx", self.config, self.output)
        spawn.assert_not_called()
        self.assertEqual((self.output / "sentinel").read_text(), "keep this run")


@unittest.skipUnless(TX.is_file() and RX.is_file(), "Build both shared-PPS PoC executables first")
class TestPocSignalPath(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.config = loadYaml(ROOT / "config" / "poc_shared_pps.yaml")
        self.config["FILES"]["output_dir"] = str(self.directory)
        self.config["FILES"]["chirp_loc"] = "chirp.bin"
        self.config["SYNC"]["pulses_per_frame"] = 8
        self.config["CHIRP"]["num_pulses"] = 34
        self.config["CHIRP"]["num_presums"] = 5
        self.config_path = self.directory / "settings.yaml"

    def writeConfig(self):
        with self.config_path.open("w") as output:
            YAML().dump(self.config, output)
        p = Protocol(self.config)
        time = np.arange(p.tx_samples) / p.rate
        bandwidth = float(self.config["GENERATE"]["chirp_bandwidth"])
        duration = float(self.config["CHIRP"]["tx_duration"])
        self.chirp = (np.exp(2j * np.pi * (-bandwidth / 2 * time + bandwidth * time ** 2 / (2 * duration)))
                      * np.hamming(p.tx_samples)).astype("<c8")
        self.chirp.tofile(self.directory / "chirp.bin")
        return p

    def makeCapture(self, *, cfo=0, phase=0, noise=0, delay=7, corrupt=(), seed=None, clock_ppm=0):
        if seed is not None:
            self.config["SYNC"]["phase_seed"] = seed
            self.config["CHIRP"]["phase_dithering"] = True
        p = self.writeConfig()
        emitted = self.directory / "emitted.bin"
        subprocess.run([str(TX), str(self.config_path), str(ROOT), "--emit", str(emitted)],
                       check=True, capture_output=True, timeout=20)
        transmitted = np.fromfile(emitted, dtype="<c8")
        self.assertEqual(len(transmitted), p.frames * p.frame_samples)
        for index in corrupt:
            start = index * p.frame_samples + len(p.preamble)
            transmitted[start:start + p.header_samples] = 0
        if clock_ppm:
            ratio = 1 + clock_ppm * 1e-6
            sample_times = np.arange(int(len(transmitted) * ratio)) / ratio
            positions = np.arange(len(transmitted))
            transmitted = (np.interp(sample_times, positions, transmitted.real)
                           + 1j * np.interp(sample_times, positions, transmitted.imag)).astype(np.complex64)
        lead = int(round(float(p.sync["capture_lead"]) * p.rate))
        tail = int(round(float(p.sync["capture_tail"]) * p.rate))
        raw = np.zeros(lead + len(transmitted) + tail, dtype=np.complex64)
        raw[lead + delay:lead + delay + len(transmitted)] = transmitted
        positions = np.arange(len(raw))
        raw *= (0.65 * np.exp(1j * (phase + 2 * np.pi * cfo * positions / p.rate))).astype(np.complex64)
        if noise:
            rng = np.random.default_rng(17)
            raw += (noise * (rng.standard_normal(len(raw)) + 1j * rng.standard_normal(len(raw)))).astype(np.complex64)
        capture = self.directory / "capture"
        capture.mkdir()
        raw.astype("<c8").tofile(capture / "rx_iq.bin")
        with (capture / "config.yaml").open("w") as output:
            YAML().dump(self.config, output)
        metadata = {"timebase": "first_shared_pps_zero", "sample_rate": p.rate,
                    "first_device_time": p.start_time - lead / p.rate,
                    "samples": len(raw), "complete": True}
        with (capture / "capture.yaml").open("w") as output:
            YAML().dump(metadata, output)
        with (capture / "rx_timestamps.csv").open("w") as output:
            writer = csv.writer(output)
            writer.writerow(["sample_index", "device_time_s", "sample_count"])
            writer.writerow([0, metadata["first_device_time"], len(raw)])
        return capture, p

    def assertChirpRecovered(self, output, p, tolerance=0.002):
        with np.load(output, allow_pickle=False) as saved:
            iq = saved["stacked_iq"]
            expected = np.zeros(p.rx_samples, dtype=np.complex64)
            expected[:p.tx_samples] = 0.65 * float(p.sync["amplitude"]) * self.chirp
            self.assertLess(np.max(np.abs(iq - expected)), tolerance)
            self.assertEqual(saved["chirp"].shape, self.chirp.shape)
            report = json.loads(str(saved["metadata_json"]))
            self.assertTrue(report["phase_corrected"])

    def test_actual_cpp_frames_decode_and_preserve_partial_stack(self):
        capture, p = self.makeCapture()
        output, report = processCapture(capture)
        self.assertEqual(report["valid_pulses"], 34)
        self.assertEqual(report["stack_counts"], [5, 5, 5, 5, 5, 5, 4])
        self.assertEqual(len(report["headers"]), p.frames)
        self.assertChirpRecovered(output, p)

    def test_frequency_phase_noise_and_legacy_dithering_are_corrected(self):
        capture, p = self.makeCapture(cfo=4500, phase=0.83, noise=0.0001, seed=0)
        output, report = processCapture(capture)
        self.assertTrue(report["dithering_removed"])
        self.assertLess(max(abs(h["cfo_hz"] - 4500) for h in report["headers"]), 3)
        self.assertChirpRecovered(output, p, 0.005)

    def test_default_length_frames_remain_coherent_with_large_carrier_offset(self):
        self.config["SYNC"]["pulses_per_frame"] = 50
        self.config["CHIRP"]["num_pulses"] = 103
        self.config["CHIRP"]["num_presums"] = 10
        capture, p = self.makeCapture(cfo=12000, phase=-0.9, noise=0.002, seed=0)
        output, report = processCapture(capture)
        self.assertEqual(report["valid_pulses"], 103)
        self.assertEqual(report["stack_counts"][-1], 3)
        self.assertLess(max(abs(h["cfo_hz"] - 12000) for h in report["headers"]), 1)
        self.assertChirpRecovered(output, p, 0.02)

    def test_lost_frame_does_not_shift_phase_sequence(self):
        capture, p = self.makeCapture(cfo=-2200, phase=-1.1, seed=1234, corrupt=(1,))
        output, report = processCapture(capture)
        self.assertEqual(report["valid_pulses"], 26)
        self.assertEqual(report["rejected_frames"][0]["frame_index"], 1)
        self.assertEqual(report["stack_counts"], [5, 3, 4, 5, 5, 4])
        self.assertChirpRecovered(output, p)

    def test_invalid_crc_headers_produce_no_stacked_export(self):
        capture, p = self.makeCapture(corrupt=range(5))
        with self.assertRaisesRegex(ValueError, "No valid preamble"):
            processCapture(capture)
        self.assertFalse((capture / "stacked_iq.npz").exists())
        self.assertTrue((capture / "rx_iq.bin").exists())

    def test_sample_clock_drift_is_tracked_across_frames(self):
        capture, p = self.makeCapture(clock_ppm=30, cfo=1500)
        output, report = processCapture(capture)
        self.assertEqual(report["valid_pulses"], p.total_pulses)
        self.assertTrue(any(abs(h["sample_clock_ratio"] - 1) > 1e-6 for h in report["headers"]))
        with np.load(output, allow_pickle=False) as saved:
            waveform = saved["stacked_iq"].mean(axis=0)[:p.tx_samples]
            coherence = abs(np.vdot(self.chirp, waveform)) / (np.linalg.norm(self.chirp) * np.linalg.norm(waveform))
            self.assertGreater(coherence, 0.95)

    def test_plot_needs_only_the_export(self):
        capture, p = self.makeCapture()
        output, report = processCapture(capture)
        figure = self.directory / "radargram.png"
        import sys
        subprocess.run([sys.executable, str(ROOT / "postprocessing" / "plot_poc.py"),
                        str(output), "--save", str(figure)], check=True, capture_output=True, timeout=30)
        self.assertGreater(figure.stat().st_size, 1000)

    def test_incomplete_capture_is_rejected(self):
        capture, p = self.makeCapture()
        metadata = loadYaml(capture / "capture.yaml")
        metadata.update(complete=False, error="RX overflow")
        with (capture / "capture.yaml").open("w") as output:
            YAML().dump(metadata, output)
        with self.assertRaisesRegex(ValueError, "RX overflow"):
            processCapture(capture)

    def test_timestamp_gap_is_rejected(self):
        capture, p = self.makeCapture()
        (capture / "rx_timestamps.csv").write_text("sample_index,device_time_s,sample_count\n1,4.99,100\n")
        with self.assertRaisesRegex(ValueError, "timestamp discontinuity"):
            processCapture(capture)

    def test_different_run_id_is_rejected(self):
        capture, p = self.makeCapture()
        config = loadYaml(capture / "config.yaml")
        config["SYNC"]["run_id"] += 1
        with (capture / "config.yaml").open("w") as output:
            YAML().dump(config, output)
        with self.assertRaisesRegex(ValueError, "run_id"):
            processCapture(capture)

    def test_validators_never_open_hardware(self):
        self.writeConfig()
        for command in (
            [str(TX), str(self.config_path), str(ROOT), "--validate"],
            [str(RX), str(self.config_path), str(ROOT), str(self.directory / "unused"), "--validate"],
        ):
            result = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("Creating a usrp", result.stdout)
        self.assertFalse((self.directory / "unused").exists())

    def test_receiver_rejects_unshared_time_config(self):
        self.config["DEVICE"]["time_ref"] = "internal"
        self.writeConfig()
        result = subprocess.run([str(RX), str(self.config_path), str(ROOT), str(self.directory), "--validate"],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("external PPS", result.stderr)


if __name__ == "__main__":
    unittest.main()
