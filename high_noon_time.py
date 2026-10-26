"""Interpret explicit clock times consistently for RX, TX, and Pi clock setup."""
from datetime import datetime, time as clock_time, timezone
from zoneinfo import ZoneInfo


def localize_time(value, zone):
    """Reject ambiguous/nonexistent DST times instead of guessing an offset."""
    candidates = set()
    for fold in (0, 1):
        utc = value.replace(tzinfo=zone, fold=fold).astimezone(timezone.utc)
        if utc.astimezone(zone).replace(tzinfo=None) == value:
            candidates.add(utc)
    if len(candidates) != 1:
        raise ValueError("Ambiguous or nonexistent local time; specify an explicit UTC offset.")
    return candidates.pop()


def parse_start_time(value, zone_name="UTC", now=None):
    """Parse an ISO date/time, or a time today, and return an aware UTC datetime."""
    zone = ZoneInfo(zone_name)
    now = now or datetime.now(timezone.utc)
    value = value.strip().replace("Z", "+00:00")
    try:
        if "T" in value or " " in value:
            target = datetime.fromisoformat(value)
        else:
            clock = clock_time.fromisoformat(value)
            today = now.astimezone(clock.tzinfo or zone).date()
            target = datetime.combine(today, clock)
    except ValueError as exc:
        raise ValueError("Use HH:MM:SS or YYYY-MM-DDTHH:MM:SS with an optional UTC offset.") from exc
    return target.astimezone(timezone.utc) if target.tzinfo else localize_time(target, zone)
