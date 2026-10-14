#pragma once

// Touch controllers as a virtual gamepad for player 1 (movement relative to
// where you look, smooth or snap turning, buttons). Requires MinHook to be initialized.
void InstallVrControlsHook();

// Computes this frame's virtual pad from the latched tracking. Call once per
// frame from Present, after VrLatchTracking.
void VrControlsOnPresent(unsigned frame);

// Holds game buttons for this frame on behalf of a gesture (e.g. grip at a
// build pile holds SPECIAL). Call before VrControlsOnPresent each frame.
void VrControlsHold(unsigned mask);

// The game's SPECIAL (use / build / force) button mask, or 0 if unavailable.
unsigned VrControlsSpecialMask();

// The game's ACTION (attack / shoot) button mask, or 0 if unavailable.
unsigned VrControlsActionMask();

// True while the left stick is moving the player (last computed frame).
bool VrControlsMoving();
