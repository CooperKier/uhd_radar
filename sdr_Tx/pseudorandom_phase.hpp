#pragma once
#include <random>

// Same seed and phase sequence as the original transmitter.
inline std::mt19937 random_generator_tx(0);
float getNextPhase();
