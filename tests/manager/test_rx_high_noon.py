"""Check scheduled RX deadlines, clock interpretation, and runner orchestration."""
from datetime import datetime, timedelta, timezone
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest
from ruamel.yaml import YAML

import RxRunHighNoon as scheduled


NOW = datetime(2026, 10, 20, 10, tzinfo=timezone.utc)
ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("value, zone, expected", [
    ("12:00:00", "UTC", "2026-10-20T12:00:00+00:00"),
    ("12:00:00", "America/Denver", "2026-10-20T18:00:00+00:00"),
    ("12:00:00-06:00", "UTC", "2026-10-20T18:00:00+00:00"),
    ("2026-10-21T12:00:00Z", "America/Denver", "2026-10-21T12:00:00+00:00"),
    ("2026-11-01T01:30:00-06:00", "America/Denver", "2026-11-01T07:30:00+00:00"),
    ("2026-10-21T12:00:00.125", "UTC", "2026-10-21T12:00:00.125000+00:00"),
])
def test_time_interpretation(value, zone, expected):
    assert scheduled.parse_start_time(value, zone, now=NOW).isoformat() == expected


@pytest.mark.parametrize("value", ["2026-03-08T02:30:00", "2026-11-01T01:30:00"])
def test_daylight_saving_requires_an_explicit_offset(value):
    with pytest.raises(ValueError, match="Ambiguous or nonexistent"):
        scheduled.parse_start_time(value, "America/Denver", now=NOW)


def test_past_time_does_not_roll_over_to_tomorrow():
    target = scheduled.parse_start_time("09:00:00", now=NOW)
    assert target.date() == NOW.date()
    with pytest.raises(ValueError, match="30 seconds"):
        scheduled.require_preparation_time(target, now=NOW)
    with pytest.raises(ValueError):
        scheduled.require_preparation_time(NOW + timedelta(seconds=29), now=NOW)
    scheduled.require_preparation_time(NOW + timedelta(seconds=30), now=NOW)


def test_runtime_configuration_preserves_source_and_separates_runs(tmp_path):
    source = ROOT / "config/Rx_B206mini.yaml"
    original = source.read_bytes()
    outputs = []
    for _ in range(2):
        config = scheduled.load_rx_config(source)
        outputs.append(scheduled.prepare_runtime_config(config, NOW, repo=tmp_path))
    assert outputs[0].parent != outputs[1].parent
    saved = YAML(typ="safe").load(outputs[0])
    assert saved["CHIRP"]["rx_start_utc"] == NOW.timestamp()
    assert saved["RF0"]["transmit"] is False
    assert saved["FILES"]["max_chirps_per_file"] == -1
    assert tmp_path / saved["FILES"]["output_dir"] == outputs[0].parent
    assert saved["SCHEDULE"]["requested_utc"] == NOW.isoformat()
    assert source.read_bytes() == original


@pytest.mark.parametrize("section, key, value", [
    ("RF0", "transmit", True), ("RF0", "receive", False),
    ("DEVICE", "clk_ref", "external"), ("CHIRP", "rx_start_utc", 123),
    ("CHIRP", "rx_start_tolerance", -1e-6),
    ("CHIRP", "rx_start_tolerance", float("nan")),
    ("CHIRP", "rx_start_tolerance", float("inf")),
])
def test_rejects_incompatible_configurations(section, key, value, tmp_path):
    config = scheduled.load_rx_config(ROOT / "config/Rx_B206mini.yaml")
    config[section][key] = value
    source = tmp_path / "bad.yaml"
    with source.open("w") as stream:
        YAML().dump(config, stream)
    with pytest.raises(ValueError):
        scheduled.load_rx_config(source)


def test_dry_run_has_no_build_device_access_or_output_files(tmp_path, monkeypatch, capsys):
    monkeypatch.chdir(tmp_path)
    def forbidden(*args, **kwargs):
        pytest.fail("Dry run must not write a configuration or initialize the runner")
    monkeypatch.setattr(scheduled, "prepare_runtime_config", forbidden)
    monkeypatch.setattr(scheduled, "run_capture", forbidden)
    future = datetime.now(timezone.utc) + timedelta(days=1)
    assert scheduled.main(["--at", future.isoformat(), "--config",
                           str(ROOT / "config/Rx_B206mini.yaml"), "--dry-run"]) == 0
    assert "Requested UTC:" in capsys.readouterr().out
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize("mode", ["success", "late_build", "unconfirmed", "interrupt"])
def test_build_precedes_deadline_check_and_verified_capture(mode, monkeypatch):
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
            if mode != "unconfirmed":
                self.observe("[SCHEDULE] First RX hardware timestamp: 10.000000000\n")

        def wait(self):
            calls.append("wait")
            if mode == "interrupt":
                raise KeyboardInterrupt()

        def stop(self, timeout):
            calls.append("stop")
            self.is_running = False
            return "saved_capture"

    def check_deadline(target):
        calls.append("deadline")
        if mode == "late_build":
            raise ValueError("deadline missed while building")

    monkeypatch.setitem(sys.modules, "runRx", SimpleNamespace(RadarProcessRunner=FakeRunner))
    monkeypatch.setattr(scheduled, "require_preparation_time", check_deadline)
    config_path = scheduled.REPO_ROOT / "data/scheduled_rx/test.yaml"
    if mode == "late_build":
        with pytest.raises(ValueError, match="deadline missed"):
            scheduled.run_capture(config_path, NOW)
        assert calls == ["setup", "deadline"]
    elif mode == "interrupt":
        with pytest.raises(KeyboardInterrupt):
            scheduled.run_capture(config_path, NOW)
        assert calls == ["setup", "deadline", "run", "wait", "stop"]
    elif mode == "unconfirmed":
        with pytest.raises(RuntimeError, match="No scheduled first-packet confirmation"):
            scheduled.run_capture(config_path, NOW)
    else:
        scheduled.run_capture(config_path, NOW)
        assert calls == ["setup", "deadline", "run", "wait", "stop"]
