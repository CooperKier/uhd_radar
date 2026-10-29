#include "pseudorandom_phase.hpp"

/** Return the next phase in the original TX dithering sequence. */
float getNextPhase() { return static_cast<float>(random_generator_tx()); }
