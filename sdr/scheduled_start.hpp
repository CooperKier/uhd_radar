#pragma once

#include <cmath>
#include <stdexcept>

/** @brief One host/SDR clock observation, with the USB query bracketed by UTC. */
struct HostTimeSample {
    double before_utc;
    double after_utc;
    double device_time;
};

/** @brief Map host UTC to device time; the query span is not total UTC error. */
inline double scheduledDeviceTime(double target_utc, const HostTimeSample& sample) {
    if (!std::isfinite(target_utc) || !std::isfinite(sample.before_utc) ||
        !std::isfinite(sample.after_utc) || !std::isfinite(sample.device_time)) {
        throw std::invalid_argument("Scheduled time and clock observations must be finite.");
    }
    double span = sample.after_utc - sample.before_utc;
    if (span < 0 || span > 0.05) {
        throw std::runtime_error("Host/SDR clock query is unreliable; check host clock and USB load.");
    }
    if (target_utc - sample.after_utc < 2.0) {
        throw std::runtime_error("Scheduled deadline is too close or has passed; no start is queued.");
    }
    return sample.device_time + (target_utc - (sample.before_utc + span / 2));
}

/** @brief Accept the first packet only if its hardware timestamp matches the target. */
inline bool scheduledTimestampMatches(double actual, double expected, double sample_rate) {
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::isfinite(sample_rate) && sample_rate > 0 &&
           std::abs(actual - expected) <= 1.0 / sample_rate;
}
