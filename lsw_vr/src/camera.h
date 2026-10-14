#pragma once

#include "shared/vr_types.h"

// Hooks the engine's NuCameraSetEx. Requires MinHook to be initialized.
void InstallCameraHook();

// Per-frame work (hotkeys, periodic camera log). Call from Present.
void CameraOnPresent(unsigned frame);

// What the game camera was rendered with since the last call (head pose and
// field of view, or has_pose = 0), for the frame being presented.
VrFrameMeta CameraTakeFrameMeta();

// Yaw (radians, game convention: forward = (sin, 0, cos)) of the game camera
// as last rendered, including head tracking. False until the camera was set.
bool CameraViewYaw(float *radians);

// Turns the VR first-person view (snap or smooth turn).
void CameraTurn(float radians);

// Options from lsw_vr_settings.txt: smooth_turn=1 (default) turns smoothly
// with the right stick, 0 in 30 degree snaps; gun_aim=head fires where the
// head looks instead of along the gun.
bool CameraSmoothTurn();

// Room scale: true while the (hidden) character should walk after the head,
// which has moved well away from it in the playspace; *yaw = which way (game
// yaw, radians).
bool CameraRoomCatchUp(float *yaw);
bool CameraGunAimFromHead();

// True while the game camera sits at the player's eyes (first person). Only
// from this frame's camera set on; before it (the game's update) false.
bool CameraFirstPersonActive();

// First person this frame or last: stable through the whole frame, for code
// that also runs in the game's update, before the camera is set.
bool CameraFirstPersonRecent();

// Maps a tracked pose (head or controller, host LOCAL space) into the game
// world the way the camera does: position in game units, axes as rows
// right/up/forward. False without head tracking.
bool CameraTrackingToWorld(const VrPose &pose, float position[3], float axes[9]);

// Game units per real meter ([ and ] adjust it).
float CameraWorldScale();

// Per-frame cleanup; call at the end of Present, after everything that reads
// camera state this frame.
void CameraEndFrame();

// True while a game menu (titles, pause, shop, character/level select...) is
// open: the frame goes on the floating screen and VR interaction pauses.
bool CameraMenuOpen();

// The view matrix the renderer uploads (shader constant c12, transposed),
// used to measure whether frames are drawn with this or last frame's camera.
void CameraOnShaderView(const float *upload);

// True while the user holds the calibration button (left thumbstick click):
// the sticks adjust world scale and stereo strength instead of moving.
bool CameraCalibrating();

// Settings (simulation commands and calibration).
void CameraSetWorldScale(float scale);
void CameraSetStereoStrength(float strength);
void CameraSetStereo(bool stereo);
void CameraSetFirstPerson(bool on);
// One line describing the camera state, for diagnostics.
void CameraDescribe(char *out, size_t size);
// Desktop window as a full-screen view of the left eye (set from the tuner).
bool CameraPcFullscreen();
// Eye placement relative to the character's neck (game units).
void CameraSetEye(float lift, float forward);
