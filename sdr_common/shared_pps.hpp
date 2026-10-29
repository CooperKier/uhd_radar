/** @file Shared first-edge PPS initialization for the two-radio bench procedure. */
#pragma once
#include "poc_protocol.hpp"
#include <uhd/usrp/multi_usrp.hpp>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace poc {
namespace detail {
/** Clock injection permits testing arming, missing pulses, cancellation and deadlines without SDRs. */
template<class Radio, class Now, class Sleep>
inline void armSharedPpsWithClock(Radio radio, const Plan& plan,
                                  const volatile std::sig_atomic_t& stopped, Now now, Sleep sleep) {
  constexpr double kSentinel = 1000000;
  radio->set_time_now(uhd::time_spec_t(kSentinel));
  const auto last_pps = radio->get_time_last_pps();
  const auto quiet_until = now() + std::chrono::duration<double>(plan.quiet_seconds);
  while (now() < quiet_until) {
    if (stopped) throw std::runtime_error("Stopped before PPS arming.");
    if (radio->get_time_last_pps() != last_pps)
      throw std::runtime_error("PPS is already running. Disable generator output and restart BOTH processes.");
    sleep(std::chrono::milliseconds(10));
  }
  radio->set_time_next_pps(uhd::time_spec_t(0.0));
  std::cout << "[SYNC] ARMED: waiting for first shared PPS. Enable generator ONLY after BOTH radios report ARMED." << std::endl;
  const auto deadline = now() + std::chrono::duration<double>(plan.arm_timeout);
  while (radio->get_time_now().get_real_secs() > kSentinel / 2) {
    if (stopped) throw std::runtime_error("Stopped while waiting for PPS.");
    if (now() >= deadline) throw std::runtime_error("PPS timeout. Check generator output and REF cabling.");
    sleep(std::chrono::milliseconds(10));
  }
  if (radio->get_time_now().get_real_secs() >= plan.captureStart() - 0.25)
    throw std::runtime_error("Missed scheduling deadline after PPS. Restart BOTH processes with PPS disabled.");
  std::cout << "[SYNC] First PPS accepted; experiment time zero established. Scheduled TX start: "
            << plan.start_time << " s." << std::endl;
}
} // namespace detail

/** Arm only while PPS is disabled; both operators must see ARMED before enabling it. */
inline void armSharedPps(uhd::usrp::multi_usrp::sptr radio, const Plan& plan,
                         const volatile std::sig_atomic_t& stopped) {
  detail::armSharedPpsWithClock(radio, plan, stopped,
      [] { return std::chrono::steady_clock::now(); },
      [](std::chrono::milliseconds interval) { std::this_thread::sleep_for(interval); });
}
} // namespace poc
