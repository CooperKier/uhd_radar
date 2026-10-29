#pragma once
#include "common.hpp"
#include <yaml-cpp/yaml.h>

/** Single-channel TX device setup derived from sdr/Sdr, without a receive stream. */
class Sdr {
public:
  explicit Sdr(const YAML::Node& config);
  void setupUsrp();
  usrp::multi_usrp::sptr getUsrp() const { return usrp; }
  tx_streamer::sptr getTxStream() const { return tx_stream; }
  double getTxRate() const { return usrp->get_tx_rate(channel); }
private:
  YAML::Node config;
  usrp::multi_usrp::sptr usrp;
  tx_streamer::sptr tx_stream;
  size_t channel = 0;
};
