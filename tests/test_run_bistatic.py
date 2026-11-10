"""Hardware-free runner and native configuration validation tests."""
from pathlib import Path
import json
import re
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from run_bistatic import BUILD, ROOT, commandFor, main, positiveLoops, runStation


class RunnerTests(unittest.TestCase):
    def test_loop_limit(self):
        self.assertEqual(positiveLoops("3"), 3)
        for value in ("0", "-1"):
            with self.assertRaises(Exception):
                positiveLoops(value)

    def test_role_and_no_default_loop_limit(self):
        command = commandFor("rx", Path("experiment.yaml"), Path("capture"))
        self.assertEqual(Path(command[0]).name, "bistatic_rx")
        self.assertNotIn("--loops", command)
        command = commandFor("tx", Path("experiment.yaml"), Path("capture"), 4, "serial=123", True)
        self.assertEqual(command[-5:], ["--loops", "4", "--device-args", "serial=123", "--validate"])

    def test_existing_output_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            marker = output / "existing.txt"
            marker.write_text("keep")
            with patch("run_bistatic.subprocess.run"):
                with self.assertRaises(FileExistsError):
                    runStation("rx", ROOT / "config" / "default.yaml", output)
            self.assertEqual(marker.read_text(), "keep")

    def test_validation_never_starts_a_radio_process(self):
        with patch("run_bistatic.Path.is_file", return_value=True), patch("run_bistatic.subprocess.run") as run, patch("run_bistatic.subprocess.Popen") as popen:
            self.assertEqual(main(["rx", "--validate"]), 0)
            self.assertIn("--validate", run.call_args.args[0])
            popen.assert_not_called()

    def test_child_failure_keeps_log_and_exit_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "run"
            command = [sys.executable, "-u", "-c", "print('radio failure'); raise SystemExit(7)"]
            with patch("run_bistatic.commandFor", return_value=command), patch("run_bistatic.subprocess.run"):
                result = runStation("tx", ROOT / "config" / "default.yaml", output, 2)
            self.assertEqual(result, 7)
            self.assertIn("radio failure", (output / "radio.log").read_text())
            report = json.loads((output / "run.json").read_text())
            self.assertEqual(report["returncode"], 7)
            self.assertEqual(report["loops"], 2)
            self.assertEqual((output / "config.yaml").read_bytes(), (ROOT / "config" / "default.yaml").read_bytes())

    def test_stop_signal_reaches_child_and_records_interruption(self):
        handlers = {}

        def register(signum, handler):
            handlers[signum] = handler
            return None

        def observeOutput(*args, **kwargs):
            if args and args[0] == "READY\n":
                handlers[signal.SIGTERM](signal.SIGTERM, None)

        script = "import signal,sys,time; signal.signal(signal.SIGINT, lambda s,f: sys.exit(0)); print('READY', flush=True); time.sleep(30)"
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "run"
            command = [sys.executable, "-u", "-c", script]
            with patch("run_bistatic.commandFor", return_value=command), patch("run_bistatic.subprocess.run"), \
                    patch("run_bistatic.signal.signal", side_effect=register), patch("builtins.print", side_effect=observeOutput):
                result = runStation("rx", ROOT / "config" / "default.yaml", output)
            self.assertEqual(result, 130)
            self.assertEqual(json.loads((output / "run.json").read_text())["returncode"], 130)
            self.assertIn("READY", (output / "radio.log").read_text())


@unittest.skipUnless((BUILD / "bistatic_rx").is_file(), "Build bistatic_rx first")
class NativeConfigTests(unittest.TestCase):
    def setUp(self):
        from ruamel.yaml import YAML
        from preprocessing.generate_chirp import generate_chirp
        import numpy as np
        self.yaml = YAML(typ="safe")
        self.settings = self.yaml.load((ROOT / "config" / "default.yaml").read_text())
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        (self.root / "data").mkdir()
        _, waveform = generate_chirp(self.settings)
        np.asarray(waveform, dtype="complex64").tofile(self.root / "data" / "chirp.bin")

    def tearDown(self):
        self.temporary.cleanup()

    def validate(self, settings=None, role="rx"):
        binary = BUILD / f"bistatic_{role}"
        if not binary.is_file():
            self.skipTest(f"Build {binary.name} first")
        config = self.root / "config.yaml"
        with config.open("w") as destination:
            self.yaml.dump(settings or self.settings, destination)
        return subprocess.run([str(binary), str(config), str(self.root), str(self.root / "unused"), "--validate"],
                              text=True, capture_output=True, timeout=10)

    def test_default_config_and_no_output_creation(self):
        result = self.validate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root / "unused").exists())

    def test_reject_unsupported_or_unsafe_capture_parameters(self):
        cases = [("CHIRP", "num_pulses", -1), ("CHIRP", "num_presums", 0),
                 ("CHIRP", "num_presums", -2), ("CHIRP", "num_presums", 3),
                 ("CHIRP", "num_presums", self.settings["CHIRP"]["num_pulses"] + 1),
                 ("DEVICE", "cpu_format", "sc16"), ("CHIRP", "time_offset", 0),
                 ("CHIRP", "tx_lead", -1), ("GPIO", "pwr_amp_pin", 1),
                 ("FILES", "max_chirps_per_file", 10), ("RF0", "rx_rate", 1e6),
                 ("RF0", "freq", 1e100), ("RF0", "rx_gain", float("nan")),
                 ("DEVICE", "clk_rate", 1e100)]
        for section, key, value in cases:
            with self.subTest(section=section, key=key):
                old = self.settings[section][key]
                self.settings[section][key] = value
                result = self.validate()
                self.assertNotEqual(result.returncode, 0)
                self.settings[section][key] = old

    def test_fingerprint_changes_with_experiment_not_serial(self):
        self.settings["CHIRP"]["num_presums"] = 1
        first = self.validate()
        self.settings["DEVICE"]["device_args"] = "type=b200,serial=other-radio"
        second = self.validate()
        self.settings["CHIRP"]["num_pulses"] = 99
        third = self.validate()
        fingerprints = [re.search(r"fingerprint=(\d+)", result.stdout).group(1) for result in (first, second, third)]
        self.assertEqual(fingerprints[0], fingerprints[1])
        self.assertNotEqual(fingerprints[0], fingerprints[2])

    def test_presums_match_between_roles_and_change_output_count(self):
        self.settings["CHIRP"]["num_pulses"] = 100
        self.settings["CHIRP"]["num_presums"] = 1
        unaveraged = self.validate()
        self.settings["CHIRP"]["num_presums"] = 10
        rx = self.validate()
        tx = self.validate(role="tx")
        for result in (unaveraged, rx, tx):
            self.assertEqual(result.returncode, 0, result.stderr)
        identity = lambda result: re.search(r"fingerprint=(\d+)", result.stdout).group(1)
        self.assertEqual(identity(rx), identity(tx))
        self.assertNotEqual(identity(unaveraged), identity(rx))
        self.assertIn("records/cycle=100", unaveraged.stdout)
        self.assertIn("records/cycle=10", rx.stdout)
        self.assertIn("without carrier-phase correction", rx.stdout)

    def test_waveform_wrong_length(self):
        (self.root / "data" / "chirp.bin").write_bytes(b"bad")
        result = self.validate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("waveform", result.stderr)

    def test_rx_config_needs_no_transmit_hardware_settings(self):
        before = self.validate()
        del self.settings["DEVICE"]["tx_channels"]
        for key in ("tx_rate", "tx_gain", "tx_ant"):
            del self.settings["RF0"][key]
        self.settings["RF0"]["transmit"] = False
        self.settings["BISTATIC"]["control_tx_gain"] = "unused"
        after = self.validate()
        self.assertEqual(after.returncode, 0, after.stderr)
        self.assertEqual(re.search(r"fingerprint=(\d+)", before.stdout).group(1),
                         re.search(r"fingerprint=(\d+)", after.stdout).group(1))

    def test_tx_config_needs_no_receive_hardware_settings(self):
        reference = self.validate()
        del self.settings["DEVICE"]["rx_channels"]
        for key in ("rx_rate", "rx_gain", "rx_ant"):
            del self.settings["RF0"][key]
        result = self.validate(role="tx")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(re.search(r"fingerprint=(\d+)", reference.stdout).group(1),
                         re.search(r"fingerprint=(\d+)", result.stdout).group(1))

    def test_tx_still_rejects_disabled_or_invalid_transmission(self):
        self.settings["RF0"]["transmit"] = False
        result = self.validate(role="tx")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("TX station", result.stderr)
        self.settings["RF0"]["transmit"] = True
        self.settings["RF0"]["tx_gain"] = float("nan")
        self.assertNotEqual(self.validate(role="tx").returncode, 0)


if __name__ == "__main__":
    unittest.main()
