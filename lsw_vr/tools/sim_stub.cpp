// The harness shares the capture code with the mod but not the game-only
// simulation (src/vr/sim.cpp): simulation is always off here.
#include "../src/vr/sim.h"

bool SimActive() {
    return false;
}

void SimOnFrame(const uint8_t *, uint32_t, uint32_t, const VrFrameMeta &) {
}
