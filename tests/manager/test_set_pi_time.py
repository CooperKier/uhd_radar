"""Exercise Pi clock commands with mocks; never change the test machine's clock."""
from datetime import datetime, timedelta, timezone
from unittest.mock import Mock

import pytest

import set_pi_time as clock

TARGET = datetime(2026, 10, 26, 18, tzinfo=timezone.utc)


def test_manual_input_requires_date_and_honors_zone():
    with pytest.raises(ValueError, match="full date"):
        clock.parse_clock_time("12:00:00", "UTC")
    assert clock.parse_clock_time("2026-10-26T12:00:00", "America/Denver") == TARGET
    assert clock.parse_clock_time("2026-10-26T18:00:00Z", "America/Denver") == TARGET


@pytest.mark.parametrize("arguments", [["--at", TARGET.isoformat()], ["--ntp"]])
def test_preview_never_invokes_system_commands_or_requires_root(arguments, monkeypatch):
    commands = Mock(side_effect=AssertionError("Must not access the clock"))
    monkeypatch.setattr(clock, "command", commands)
    monkeypatch.setattr(clock.os, "geteuid", lambda: 1000)
    assert clock.main(arguments + ["--dry-run"]) == 0
    commands.assert_not_called()


def test_manual_change_needs_privileges(monkeypatch):
    commands = Mock()
    monkeypatch.setattr(clock, "command", commands)
    monkeypatch.setattr(clock.os, "geteuid", lambda: 1000)
    with pytest.raises(SystemExit) as exc:
        clock.main(["--at", TARGET.isoformat()])
    assert exc.value.code == 1
    commands.assert_not_called()


def test_manual_setting_disables_ntp_then_sets_and_verifies_utc(monkeypatch):
    commands = Mock(side_effect=["yes", "", ""])
    monkeypatch.setattr(clock, "command", commands)
    monkeypatch.setattr(clock, "datetime", Mock(now=Mock(return_value=TARGET + timedelta(seconds=0.1))))
    clock.set_manual_time(TARGET)
    assert [call.args[0] for call in commands.call_args_list] == [
        ["timedatectl", "show", "--property=NTP", "--value"],
        ["timedatectl", "set-ntp", "false"],
        ["date", "--utc", "--set", TARGET.isoformat()],
    ]


def test_failed_manual_setting_restores_previously_enabled_ntp(monkeypatch):
    commands = Mock(side_effect=["yes", "", RuntimeError("date denied"), ""])
    monkeypatch.setattr(clock, "command", commands)
    with pytest.raises(RuntimeError, match="date denied"):
        clock.set_manual_time(TARGET)
    assert commands.call_args.args[0] == ["timedatectl", "set-ntp", "true"]


def test_manual_setting_fails_when_clock_does_not_change(monkeypatch):
    monkeypatch.setattr(clock, "command", Mock(return_value=""))
    monkeypatch.setattr(clock, "datetime", Mock(now=Mock(return_value=TARGET + timedelta(days=1))))
    with pytest.raises(RuntimeError, match="verification failed"):
        clock.set_manual_time(TARGET)


def test_ntp_waits_for_reported_synchronization(monkeypatch):
    commands = Mock(side_effect=["", "no", "yes"])
    monkeypatch.setattr(clock, "command", commands)
    monkeypatch.setattr(clock.time, "sleep", Mock())
    clock.synchronize_network_time(30)
    assert commands.call_count == 3
    clock.time.sleep.assert_called_once_with(0.5)


def test_ntp_timeout_does_not_claim_synchronization(monkeypatch):
    commands = Mock(side_effect=["", "no"])
    monkeypatch.setattr(clock, "command", commands)
    monkeypatch.setattr(clock.time, "monotonic", Mock(side_effect=[0, 2]))
    with pytest.raises(RuntimeError, match="did not synchronize"):
        clock.synchronize_network_time(1)


def test_clock_command_preserves_error_details(monkeypatch):
    run = Mock(return_value=Mock(returncode=1, stderr="Operation not permitted", stdout=""))
    monkeypatch.setattr(clock.subprocess, "run", run)
    with pytest.raises(RuntimeError, match="Operation not permitted"):
        clock.command(["date", "--utc", "--set", TARGET.isoformat()])
    assert run.call_args.kwargs["timeout"] == 15
