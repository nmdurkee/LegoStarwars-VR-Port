#pragma once

// Hooks the point where a character's animated joint matrices are final
// (StoreLocatorCoordinates) to put the player's hands on the controllers and
// hide the rest of the body (VR first person), and log the skeleton. Requires
// MinHook to be initialized.
void InstallSkeletonHook();

// Per-frame hotkeys (F6 dump, F11 hand test pose). Call from Present.
void SkeletonOnPresent();

// The player's drawn saber blade (world space, in the VR hand) as drawn this
// frame: hilt and tip. False if the character has no blade locators or the
// player wasn't drawn this frame. Call before SkeletonOnPresent.
bool SkeletonBladeWorld(float hilt[3], float tip[3]);

// The player character's neck height above its feet (game units, smoothed);
// false if the player hasn't been drawn recently.
bool SkeletonNeckHeight(float *height);

// True if the player's character was drawn this frame (it exists, the game is
// rendering it). Call before SkeletonOnPresent.
bool SkeletonPlayerDrawn();
