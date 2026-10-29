"""TX launcher lifecycle tests, plus hardware-free validation of a built binary."""

import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("run_tx", ROOT / "run_tx.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)
TX_BINARY = ROOT / "sdr_Tx" / "build" / "radar_tx"


class FakeProcess:
    """Track subprocess cleanup without GPS utilities or a connected radio."""

    def __init__(self, returncode=None):
        self.returncode = returncode
        self.terminated = False

    def poll(self):
        return self.returncode

    def terminate(self):
        self.terminated = True
        self.returncode = 0

    def wait(self, timeout=None):
        return self.returncode


class TestRunTx(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.config = self.directory / "settings.yaml"
        self.config.write_text("config snapshot\n")
        (self.directory / "radar_tx").touch()
        (self.directory / "gps_logger").touch()
        self.log_dir = self.directory / "logs"
        for mocker in (
            patch.object(RUNNER, "BUILD", self.directory),
            patch.object(RUNNER.subprocess, "run"),
            patch.object(RUNNER.shutil, "which", return_value="/usr/bin/gpspipe"),
        ):
            mocker.start()
            self.addCleanup(mocker.stop)

    def test_success_preserves_gps_logs_and_config(self):
        gps, track, tx = FakeProcess(), FakeProcess(), FakeProcess(0)
        with patch.object(RUNNER.subprocess, "Popen", side_effect=[gps, track, tx]) as spawn:
            result = RUNNER.runTx(self.config, "gpsd", "/dev/ttyACM0", self.log_dir)
        self.assertEqual(result, 0)
        self.assertTrue(gps.terminated and track.terminated)
        self.assertEqual((self.log_dir / "config.yaml").read_text(), self.config.read_text())
        self.assertEqual(json.loads((self.log_dir / "run.json").read_text())["returncode"], 0)
        commands = [call.args[0] for call in spawn.call_args_list]
        self.assertEqual(commands[0][0], "gpspipe")
        self.assertEqual(commands[1][0], "gpxlogger")
        self.assertEqual(Path(commands[2][0]).name, "radar_tx")

    def test_gps_failure_stops_running_transmitter(self):
        gps, track, tx = FakeProcess(7), FakeProcess(), FakeProcess()
        with patch.object(RUNNER.subprocess, "Popen", side_effect=[gps, track, tx]):
            with self.assertRaisesRegex(RuntimeError, "GPS logger exited"):
                RUNNER.runTx(self.config, "gpsd", "/dev/ttyACM0", self.log_dir)
        self.assertTrue(tx.terminated)
        self.assertTrue(track.terminated)
        self.assertEqual(json.loads((self.log_dir / "run.json").read_text())["returncode"], 1)

    def test_failed_second_gps_spawn_cleans_up_first(self):
        gps = FakeProcess()
        with patch.object(RUNNER.subprocess, "Popen", side_effect=[gps, OSError("spawn failed")]):
            with self.assertRaises(OSError):
                RUNNER.runTx(self.config, "gpsd", "/dev/ttyACM0", self.log_dir)
        self.assertTrue(gps.terminated)

    def test_serial_mode_does_not_start_gpsd_tools(self):
        gps, tx = FakeProcess(), FakeProcess(0)
        with patch.object(RUNNER.subprocess, "Popen", side_effect=[gps, tx]) as spawn:
            result = RUNNER.runTx(self.config, "serial", "/dev/ttyUSB0", self.log_dir)
        self.assertEqual(result, 0)
        command = spawn.call_args_list[0].args[0]
        self.assertEqual(Path(command[0]).name, "gps_logger")
        self.assertEqual(command[1], "/dev/ttyUSB0")
        self.assertTrue(gps.terminated)

    def test_signal_stops_both_tx_and_gps(self):
        gps, tx = FakeProcess(), FakeProcess()
        handlers = {}

        def register(sig, handler):
            handlers[sig] = handler

        def spawn(command, **kwargs):
            if Path(command[0]).name == "radar_tx":
                handlers[RUNNER.signal.SIGINT](RUNNER.signal.SIGINT, None)
                return tx
            return gps

        with patch.object(RUNNER.signal, "signal", side_effect=register):
            with patch.object(RUNNER.subprocess, "Popen", side_effect=spawn):
                result = RUNNER.runTx(self.config, "serial", "/dev/ttyACM0", self.log_dir)
        self.assertEqual(result, 130)
        self.assertTrue(tx.terminated and gps.terminated)


@unittest.skipUnless(TX_BINARY.is_file(), "Build sdr_Tx before running C++ validation tests")
class TestTxConfiguration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.config = self.directory / "tx.yaml"
        self.text = """
DEVICE:
  tx_channels: '0'
  cpu_format: fc32
  clk_rate: 32000000
RF0:
  tx_rate: 1000000
  freq: 450000000
  tx_gain: 0
  transmit: true
CHIRP:
  tx_duration: 0.000002
  pulse_rep_int: 0.001
  num_pulses: 2
FILES:
  output_dir: '.'
  chirp_loc: chirp.bin
"""
        (self.directory / "chirp.bin").write_bytes(struct.pack("ffff", 1, 0, 0, 1))

    def validate(self, text):
        self.config.write_text(text)
        return subprocess.run([str(TX_BINARY), str(self.config), str(self.directory), "--validate"],
                              capture_output=True, text=True, timeout=10)

    def test_minimal_config_requires_no_receive_or_second_frontend(self):
        result = self.validate(self.text)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_short_waveform_is_rejected(self):
        (self.directory / "chirp.bin").write_bytes(b"short")
        self.assertNotEqual(self.validate(self.text).returncode, 0)

    def test_overlapping_pulses_are_rejected(self):
        self.assertNotEqual(self.validate(self.text.replace("0.001", "0.000001")).returncode, 0)

    def test_disabled_transmitter_is_rejected(self):
        self.assertNotEqual(self.validate(self.text.replace("transmit: true", "transmit: false")).returncode, 0)


if __name__ == "__main__":
    unittest.main()
