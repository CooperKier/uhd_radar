#include "rf_settings.hpp"
#include <uhd/types/tune_request.hpp>
#include <chrono>
#include <cmath>
#include <stdexcept>

/** Configure only the selected transmitter and poll LO lock without a fixed delay. */
void setRfParams(usrp::multi_usrp::sptr usrp, const YAML::Node& rf, size_t channel) {
  const double rate = rf["tx_rate"].as<double>();
  if (!std::isfinite(rate) || rate <= 0) {
    throw std::invalid_argument("RF0.tx_rate must be positive and finite.");
  }
  usrp->set_tx_rate(rate, channel);
  uhd::tune_request_t tune(rf["freq"].as<double>(), rf["lo_offset"].as<double>(0));
  tune.args = device_addr_t(rf["tuning_args"].as<string>(""));
  usrp->set_tx_freq(tune, channel);
  usrp->set_tx_gain(rf["tx_gain"].as<double>(), channel);
  const double bandwidth = rf["bw"].as<double>(0);
  if (bandwidth > 0) usrp->set_tx_bandwidth(bandwidth, channel);
  usrp->set_tx_antenna(rf["tx_ant"].as<string>("TX/RX"), channel);

  const auto names = usrp->get_tx_sensor_names(channel);
  if (find(names.begin(), names.end(), "lo_locked") != names.end()) {
    const auto deadline = chrono::steady_clock::now() + chrono::seconds(2);
    while (!usrp->get_tx_sensor("lo_locked", channel).to_bool()) {
      if (chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error("Timed out waiting for TX LO lock.");
      }
      this_thread::sleep_for(chrono::milliseconds(5));
    }
  }
  if (abs(usrp->get_tx_rate(channel) - rate) > rate * 1e-6) {
    throw std::runtime_error("UHD could not provide the requested TX sample rate.");
  }
}
