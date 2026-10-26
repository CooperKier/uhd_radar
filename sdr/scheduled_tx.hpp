#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>

/** @brief The transport reports no message, a completed burst, or a TX fault. */
enum class ScheduledTxEvent { None, Ack, Error };

/** @brief Validate a finite, non-overlapping scheduled TX pulse train. */
inline void validateScheduledTx(double start, double period, double duration,
                                long pulses, std::size_t samples) {
    if (!std::isfinite(start) || start < 0 || !std::isfinite(period) ||
        !std::isfinite(duration) || duration <= 0 || period < duration ||
        pulses < 1 || samples == 0 || !std::isfinite(start + period * pulses)) {
        throw std::invalid_argument("Scheduled TX needs positive finite timing, non-overlapping bursts, num_pulses >= 1, and samples.");
    }
}

/** @brief Queue up to six timed bursts, requiring a complete send and ACK for each.
 * Transport supplies now(), stopped(), send(index, time), event(timeout), and
 * acknowledged(index, time). Timeouts use the SDR clock, independent of host UTC.
 */
template <class Transport>
long transmitScheduledBursts(Transport& transport, double start, double period,
                             double duration, long pulses, std::size_t samples) {
    validateScheduledTx(start, period, duration, pulses, samples);
    long sent = 0;
    long acknowledged = 0;
    while (acknowledged < pulses) {
        if (transport.stopped()) throw std::runtime_error("Scheduled TX cancelled.");
        while (sent < pulses && sent - acknowledged < 6) {
            if (transport.stopped()) throw std::runtime_error("Scheduled TX cancelled.");
            double target = start + period * sent;
            if (transport.now() >= target) {
                throw std::runtime_error("Scheduled TX deadline missed; refusing a late burst.");
            }
            if (transport.send(sent, target) != samples) {
                throw std::runtime_error("Scheduled TX send was incomplete; transmission failed.");
            }
            ++sent;
        }
        auto event = transport.event(0.05);
        if (event == ScheduledTxEvent::Error) {
            throw std::runtime_error("Scheduled TX reported an asynchronous UHD error.");
        }
        if (event == ScheduledTxEvent::Ack) {
            transport.acknowledged(acknowledged, start + period * acknowledged);
            ++acknowledged;
        } else if (transport.now() > start + period * acknowledged + duration + 2.0) {
            throw std::runtime_error("Scheduled TX timed out waiting for a burst acknowledgement.");
        }
    }
    return acknowledged;
}
