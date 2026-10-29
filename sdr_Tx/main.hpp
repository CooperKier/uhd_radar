#pragma once
#include "chirp.hpp"
#include "sdr.hpp"
#include "pseudorandom_phase.hpp"
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/thread.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

void transmitWorker(Sdr& sdr, const Chirp& chirp,
                    const vector<complex<float>>& samples);
