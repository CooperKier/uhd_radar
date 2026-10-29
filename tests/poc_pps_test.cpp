/** @file Exercise the real PPS arming state transitions with a simulated hardware clock. */
#include "shared_pps.hpp"
#include <memory>

struct FakeRadio {
  double seconds = 0, origin = 0, base = 0, last_pps = -1, armed_at = 0, latency = 0;
  bool running = false, pulse_after_arm = true, armed = false, pulsed = false, cancel = false;
  volatile std::sig_atomic_t stopped = 0;
  void set_time_now(uhd::time_spec_t value) { base = value.get_real_secs(); origin = seconds; }
  uhd::time_spec_t get_time_now() const { return uhd::time_spec_t(base + seconds - origin); }
  uhd::time_spec_t get_time_last_pps() const { return uhd::time_spec_t(last_pps); }
  void set_time_next_pps(uhd::time_spec_t value) {
    if (value.get_real_secs() != 0) throw std::runtime_error("Expected experiment epoch zero.");
    armed = true; armed_at = seconds;
  }
  void advance(std::chrono::milliseconds interval) {
    const double before = seconds;
    seconds += interval.count() / 1000.0;
    if (running && std::floor(seconds) > std::floor(before)) last_pps = get_time_now().get_real_secs();
    if (armed && cancel) stopped = 1;
    if (armed && pulse_after_arm && !pulsed && seconds - armed_at >= 0.2) {
      base = latency; origin = seconds; last_pps = 0; pulsed = true;
    }
  }
};

/** Run the same arming logic with simulated elapsed time, not wall-clock sleeps. */
static void run(const std::shared_ptr<FakeRadio>& radio, const poc::Plan& plan) {
  poc::detail::armSharedPpsWithClock(radio, plan, radio->stopped,
      [&] { return std::chrono::steady_clock::time_point{} +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(radio->seconds)); },
      [&](std::chrono::milliseconds interval) { radio->advance(interval); });
}

/** Expected failures must be explicit, rather than silently proceeding with mismatched epochs. */
static void expectFailure(const std::shared_ptr<FakeRadio>& radio, const poc::Plan& plan, const std::string& text) {
  try { run(radio, plan); }
  catch (const std::runtime_error& error) {
    if (std::string(error.what()).find(text) != std::string::npos) return;
    throw;
  }
  throw std::runtime_error("Expected arming failure: " + text);
}

int main(int argc, char* argv[]) {
  try {
    if (argc != 2) throw std::runtime_error("Pass the PoC config path.");
    poc::Plan plan(YAML::LoadFile(argv[1]));
    plan.arm_timeout = 1;
    auto good = std::make_shared<FakeRadio>();
    run(good, plan);
    if (!good->armed || !good->pulsed || good->get_time_now().get_real_secs() > 0.1)
      throw std::runtime_error("First shared edge did not establish time zero.");
    auto running = std::make_shared<FakeRadio>(); running->running = true;
    expectFailure(running, plan, "already running");
    if (running->armed) throw std::runtime_error("Armed despite an already-running PPS source.");
    auto missing = std::make_shared<FakeRadio>(); missing->pulse_after_arm = false;
    expectFailure(missing, plan, "PPS timeout");
    auto cancelled = std::make_shared<FakeRadio>(); cancelled->cancel = true;
    expectFailure(cancelled, plan, "Stopped while waiting");
    auto late = std::make_shared<FakeRadio>(); late->latency = 10;
    expectFailure(late, plan, "Missed scheduling deadline");
    std::cout << "Five PPS arming scenarios passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
