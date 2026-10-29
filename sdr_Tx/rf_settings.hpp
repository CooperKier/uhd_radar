#pragma once
#include "common.hpp"
#include <yaml-cpp/yaml.h>

void setRfParams(usrp::multi_usrp::sptr usrp, const YAML::Node& rf, size_t channel);
