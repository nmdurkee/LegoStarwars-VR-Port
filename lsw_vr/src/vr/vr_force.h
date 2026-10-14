#pragma once

// Point-to-target Force for player 1: the left hand's pointing ray picks the
// Force target (props and characters) through the game's own target pickers.
// Requires MinHook to be initialized.
void InstallVrForce();

// Latches this frame's pointing ray and the left-trigger Force press. Call
// once per frame from Present, after VrLatchTracking and CameraOnPresent and
// before VrControlsOnPresent (it holds SPECIAL through the virtual pad).
void VrForceOnPresent(unsigned frame);
