#pragma once

#include "../shared/vr_types.h"

// VR interaction for player 1: trigger at a hip draws/holsters the saber into
// that hand, swinging it damages what it hits, and gripping at a LEGO pile
// builds it. Looks up the game functions it calls (logs if any is missing).
void InstallVrInteraction();

// Call once per frame from Present, after VrLatchTracking and CameraOnPresent
// and before VrControlsOnPresent (it holds buttons through the virtual pad).
void VrInteractionOnPresent(unsigned frame);

// The controller (0 left, 1 right) the saber is in: the one that last drew
// it, right until one has.
int VrInteractionSaberHand();

// True if that controller (0 left, 1 right) is at its hip (the holster spot).
bool VrInteractionHandAtHip(const VrTrackingState &tracking, int side);
