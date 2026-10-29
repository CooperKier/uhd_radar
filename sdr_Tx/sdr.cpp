#include "sdr.hpp"
#include "rf_settings.hpp"
#include <chrono>
#include <cmath>
#include <stdexcept>

/** Retain the YAML interface while restricting this executable to one transmitter. */
Sdr::Sdr(const YAML::Node& config) : config(config) {
  const auto dev = config["DEVICE"];
  const string channels = dev["tx_channels"].as<string>("0");
  size_t parsed = 0;
  channel = stoul(channels, &parsed);
  if (channels.find('-') != string::npos || parsed != channels.size()) {
    throw invalid_argument("TX-only mode requires a single DEVICE.tx_channels value.");
  }
  if (dev["cpu_format"].as<string>("fc32") != "fc32") {
    throw invalid_argument("TX-only mode currently requires cpu_format: fc32.");
  }
  if (!config["RF0"]["transmit"].as<bool>(true)) {
    throw invalid_argument("RF0.transmit is false; refusing to transmit.");
  }
  const auto rf = config["RF0"];
  const double clock_rate = dev["clk_rate"].as<double>();
  const double frequency = rf["freq"].as<double>();
  const double gain = rf["tx_gain"].as<double>();
  const double bandwidth = rf["bw"].as<double>(0);
  const double lo_offset = rf["lo_offset"].as<double>(0);
  if (!isfinite(clock_rate) || clock_rate <= 0 || !isfinite(frequency) || frequency <= 0 ||
      !isfinite(gain) || !isfinite(bandwidth) || bandwidth < 0 || !isfinite(lo_offset)) {
    throw invalid_argument("Invalid TX clock, frequency, gain, bandwidth or LO offset.");
  }
}

/** Configure clocks, TX RF and optional amplifier ATR. GPS logging is a separate process. */
void Sdr::setupUsrp() {
  const auto dev = config["DEVICE"];
  usrp = usrp::multi_usrp::make(dev["device_args"].as<string>("type=b200"));
  const string clock = dev["clk_ref"].as<string>("internal");
  usrp->set_clock_source(clock);
  usrp->set_time_source(dev["time_ref"].as<string>(clock));
  usrp->set_tx_subdev_spec(dev["subdev"].as<string>("A:A"));
  usrp->set_master_clock_rate(dev["clk_rate"].as<double>());
  if (channel >= usrp->get_tx_num_channels()) throw runtime_error("Invalid TX channel.");
  if (clock != "internal") {
    const auto names = usrp->get_mboard_sensor_names(0);
    for (const string sensor : {string("ref_locked"), string("gps_locked")}) {
      if (sensor == "gps_locked" && clock != "gpsdo") continue;
      if (find(names.begin(), names.end(), sensor) == names.end()) continue;
      const auto deadline = chrono::steady_clock::now() + chrono::seconds(30);
      while (!usrp->get_mboard_sensor(sensor, 0).to_bool()) {
        if (chrono::steady_clock::now() >= deadline) throw runtime_error("Timeout waiting for " + sensor);
        this_thread::sleep_for(chrono::milliseconds(20));
      }
    }
  }
  if (clock == "gpsdo") {
    const int64_t gps_time = usrp->get_mboard_sensor("gps_time", 0).to_int();
    usrp->set_time_next_pps(time_spec_t(gps_time + 1), 0);
    this_thread::sleep_for(chrono::seconds(2));
    if (usrp->get_time_last_pps(0).get_full_secs() !=
        usrp->get_mboard_sensor("gps_time", 0).to_int()) {
      throw runtime_error("USRP time did not synchronize to GPS time.");
    }
  } else {
    // Relative timed TX needs no PPS edge or one-second startup sleep.
    usrp->set_time_now(time_spec_t(0.0));
  }
  setRfParams(usrp, config["RF0"], channel);

  const auto gpio = config["GPIO"];
  if (gpio) {
    const int pin = gpio["pwr_amp_pin"].as<int>(-1);
    if (pin != -1) {
      if (pin < 2 || pin > 31) throw invalid_argument("Invalid amplifier DB15 pin.");
      const uint32_t mask = uint32_t(1) << (pin - 2);
      const string bank = gpio["gpio_bank"].as<string>("FP0");
      usrp->set_gpio_attr(bank, "CTRL", mask, mask);
      usrp->set_gpio_attr(bank, "DDR", mask, mask);
      usrp->set_gpio_attr(bank, "ATR_0X", 0, mask);
      // Assert the amplifier during TX-only activity.
      usrp->set_gpio_attr(bank, "ATR_TX", mask, mask);
      usrp->set_gpio_attr(bank, "ATR_XX", mask, mask);
    }
    const int ref_out = gpio["ref_out"].as<int>(-1);
    if (ref_out == 0 || ref_out == 1) usrp->set_clock_source_out(ref_out == 1);
  }
  stream_args_t args("fc32", dev["otw_format"].as<string>("sc16"));
  args.channels = {channel};
  tx_stream = usrp->get_tx_stream(args);
}
