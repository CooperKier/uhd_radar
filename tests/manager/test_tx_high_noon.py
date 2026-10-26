"""Check TX-only validation, isolated output, deadlines, and acknowledgement logs."""
from datetime import datetime, timedelta, timezone
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest
from ruamel.yaml import YAML

import TxRunHighNoon as scheduled

ROOT = Path(__file__).resolve().parents[2]
TARGET = datetime(2026, 10, 26, 18, tzinfo=timezone.utc)


@pytest.mark.parametrize("section, key, value", [
    ("RF0", "transmit", False), ("RF0", "receive", True),
    ("DEVICE", "clk_ref", "external"), ("DEVICE", "cpu_format", "sc16"),
    ("CHIRP", "rx_start_utc", 123), ("CHIRP", "tx_start_utc", 123),
    ("CHIRP", "num_pulses", -1), ("CHIRP", "num_pulses", 0),
    ("CHIRP", "pulse_rep_int", 0.000001), ("CHIRP", "tx_duration", float("nan")),
])
def test_rejects_incompatible_tx_configuration(section, key, value, tmp_path):
    config = scheduled.load_tx_config(ROOT / "config/Tx_B206mini.yaml")
    config[section][key] = value
    source = tmp_path / "bad.yaml"
    with source.open("w") as stream:
        YAML().dump(config, stream)
    with pytest.raises(ValueError):
        scheduled.load_tx_config(source)


def test_tx_run_preserves_original_and_keeps_chirp_and_logs_together(tmp_path):
    source = ROOT / "config/Tx_B206mini.yaml"
    before = source.read_bytes()
    paths = [scheduled.prepare_runtime_config(scheduled.load_tx_config(source), TARGET, repo=tmp_path)
             for _ in range(2)]
    saved = YAML(typ="safe").load(paths[0])
    assert paths[0].parent != paths[1].parent
    assert saved["CHIRP"]["tx_start_utc"] == TARGET.timestamp()
    assert saved["RF0"]["receive"] is False
    assert saved["GENERATE"]["out_file"] == saved["FILES"]["chirp_loc"] == "chirp.bin"
    assert saved["GENERATE"]["show_plot"] is False
    assert tmp_path / saved["FILES"]["output_dir"] == paths[0].parent
    assert source.read_bytes() == before


def test_tx_dry_run_does_not_build_write_or_transmit(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    def forbidden(*args, **kwargs):
        pytest.fail("Dry run must not build, write, or transmit")
    monkeypatch.setattr(scheduled, "prepare_runtime_config", forbidden)
    monkeypatch.setattr(scheduled, "run_transmission", forbidden)
    target = datetime.now(timezone.utc) + timedelta(days=1)
    assert scheduled.main(["--at", target.isoformat(), "--config",
                           str(ROOT / "config/Tx_B206mini.yaml"), "--dry-run"]) == 0
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize("mode", ["success", "late_build", "missing_first", "missing_complete", "interrupt", "radar_error"])
def test_tx_requires_early_build_and_burst_confirmations(mode, monkeypatch):
    calls = []
    class FakeRunner:
        def __init__(self, filename, log_processing_function):
            self.observe = log_processing_function
            self.is_running = False
            self.gpspipe_process = self.gpxlogger_process = None

        def setup(self):
            calls.append("setup")

        def run(self):
            calls.append("run")
            self.is_running = True

        def wait(self):
            calls.append("wait")
            if mode == "interrupt":
                raise KeyboardInterrupt()
            if mode != "missing_first":
                self.observe("[SCHEDULE] First TX burst acknowledged; target device time: 10.0\n")
            if mode != "missing_complete":
                self.observe("[SCHEDULE] TX complete; acknowledged bursts: 1000\n")

        def stop(self, timeout):
            calls.append("stop")
            self.is_running = False
            if mode == "radar_error":
                raise RuntimeError("Radar exited with status 255")
            return "saved"

    def deadline(target):
        calls.append("deadline")
        if mode == "late_build":
            raise ValueError("missed deadline")

    monkeypatch.setitem(sys.modules, "runTx", SimpleNamespace(RadarProcessRunner=FakeRunner))
    monkeypatch.setattr(scheduled, "require_preparation_time", deadline)
    path = scheduled.REPO_ROOT / "data/scheduled_tx/test.yaml"
    if mode == "success":
        scheduled.run_transmission(path, TARGET)
    elif mode == "late_build":
        with pytest.raises(ValueError, match="missed deadline"):
            scheduled.run_transmission(path, TARGET)
        assert calls == ["setup", "deadline"]
    elif mode == "interrupt":
        with pytest.raises(KeyboardInterrupt):
            scheduled.run_transmission(path, TARGET)
        assert calls[-1] == "stop"
    elif mode == "radar_error":
        with pytest.raises(RuntimeError, match="status 255"):
            scheduled.run_transmission(path, TARGET)
    else:
        with pytest.raises(RuntimeError, match="confirmation is missing"):
            scheduled.run_transmission(path, TARGET)
