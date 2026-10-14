// Virtual gamepad: Touch controller input merged into player 1's pad.
//
// Hook point: NuPadRead (006d6020), which fills a NUPAD from the device each
// frame; ReadPad (0054ae40) then copies it into GamePad[port], and MovePlayer
// reads the stick from the NUPAD directly. Merging here means everything
// downstream sees a normal controller, and a physical gamepad keeps working.
//
// PC layouts (from ReadPad): GamePad[] @009253b0, 0x60 each, NUPAD* at +0.
// NUPAD +0x70 buttons held, +0x78 pressed, +0x92/+0x93 left stick x/y
// (bytes, centre 127.5, y down). Button masks are game globals (values match
// opensaga). GameCam @008021b4, input yaw (u16 angle) at +0x1fc: the game
// turns stick angles into world directions by adding it.
//
// Smooth locomotion: the left stick moves relative to where the HEAD looks
// (push left, go to the head's left), at once, in a straight line. The game
// was made for a third-person camera: it walks the character along its
// facing, which turns toward the stick at a limited rate, and stalls 0.2 s for
// a turn-around animation when the stick points more than ~150 degrees away
// from it. So whenever the stick moves the player (normal walking/running
// contexts), the pad hook also points the character (facing_angle +0x58,
// movement_facing_angle +0x5a) straight along the movement before MovePlayer
// reads the pad. The body is hidden in VR first person, so the snap is
// never seen. Player[0] @0093d7f0, character_context (s8) +0x7b5: -1 and 1 are
// the plain movement contexts (opensaga MovePlayer).

#include "vr_controls.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "../camera.h"
#include "../game.h"
#include "../log.h"
#include "sim.h"
#include "vr_frame.h"

namespace {
    constexpr uintptr_t kNuPadRead = 0x6d6020;
    // mov eax,[esp+4]; push eax
    constexpr unsigned char kNuPadReadPrologue[] = {0x8b, 0x44, 0x24, 0x04, 0x50};
    constexpr uintptr_t kGamePads = 0x9253b0;
    constexpr uintptr_t kGameCamPtr = 0x8021b4;
    constexpr uintptr_t kGameCamInputYaw = 0x1fc;
    constexpr uintptr_t kPadHeld = 0x70;
    constexpr uintptr_t kPadPressed = 0x78;
    constexpr uintptr_t kPadLeftX = 0x92;
    constexpr uintptr_t kPadLeftY = 0x93;
    constexpr uintptr_t kPadRightX = 0x90;
    constexpr uintptr_t kPadRightY = 0x91;
    constexpr uintptr_t kMaskStart = 0x802274;
    constexpr uintptr_t kMaskJump = 0x802278;
    constexpr uintptr_t kMaskAction = 0x80227c;
    constexpr uintptr_t kMaskSpecial = 0x802280;
    constexpr uintptr_t kMaskTag = 0x802284;
    constexpr uintptr_t kPlayers = 0x93d7f0;
    constexpr uintptr_t kObjFacing = 0x58;
    constexpr uintptr_t kObjMoveFacing = 0x5a;
    constexpr uintptr_t kObjContext = 0x7b5;

    constexpr float kPi = 3.14159265f;
    constexpr float kStickDeadzone = 0.15f;
    // The game ignores stick deflections under 42.5/127.5; start just above it.
    constexpr float kGameStickMinimum = 0.34f;
    constexpr float kSnapTurnRadians = 30.0f * kPi / 180.0f;
    constexpr float kSnapTurnPress = 0.7f;
    constexpr float kSnapTurnRelease = 0.3f;
    // Smooth turning: degrees per second at full deflection, eased in
    // (squared) past the deadzone so small corrections stay slow.
    constexpr float kSmoothTurnRate = 150.0f * kPi / 180.0f;
    constexpr float kTurnDeadzone = 0.2f;
    // Stick deflection for the room-scale catch-up walk (just past the
    // game's deadzone: its slowest walk).
    constexpr float kCatchUpStick = 0.36f;
    constexpr unsigned kLogEvery = 300;

    using NuPadReadFn = int(__cdecl *)(unsigned char *pad, int flag);
    NuPadReadFn g_real_pad_read = nullptr;

    // This frame's virtual pad (computed at Present, applied on every read).
    // The stick is either a direction in the world (VR locomotion: turned into
    // pad bytes at read time, against the game camera's yaw at that moment)
    // or raw pad bytes (simulation).
    struct VirtualPad {
        bool active = false;
        unsigned held = 0;
        unsigned pressed = 0;
        bool has_stick = false;
        unsigned char stick_x = 128;
        unsigned char stick_y = 128;
        bool has_move = false;
        bool face_move = false;  // also point the character along it (VR first person only)
        float move_yaw = 0.0f;    // world direction (game yaw, radians)
        float move_amount = 0.0f;  // game stick deflection, kGameStickMinimum..1
    };
    VirtualPad g_pad;
    bool g_pressed_pending = false;  // this frame's pressed edges not yet applied
    bool g_logged_no_device = false;
    unsigned g_gesture_held = 0;  // buttons held by gestures this frame
    unsigned g_prev_held = 0;
    bool g_snap_armed = true;
    LARGE_INTEGER g_last_present = {};
    bool g_moving = false;  // the left stick is moving the player this frame

    unsigned Mask(uintptr_t global) {
        return *GameAddress<const unsigned>(global);
    }

    unsigned char ToStickByte(float value) {
        return static_cast<unsigned char>(std::clamp(127.5f + value * 127.5f, 0.0f, 255.0f));
    }

    float GameCamInputYaw(bool *ok) {
        *ok = false;
        __try {
            const auto *game_cam = *GameAddress<const unsigned char *const>(kGameCamPtr);
            if (game_cam == nullptr) {
                return 0.0f;
            }
            const auto yaw = *reinterpret_cast<const unsigned short *>(game_cam + kGameCamInputYaw);
            *ok = true;
            return yaw * (2.0f * kPi / 65536.0f);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0.0f;
        }
    }

    // Left stick: a world direction relative to where the head looks.
    void MapLocomotion(const VrHand &left, VirtualPad *pad) {
        const float magnitude = std::min(1.0f, std::hypot(left.stick_x, left.stick_y));
        float view_yaw = 0.0f;
        if (magnitude < kStickDeadzone || !CameraViewYaw(&view_yaw)) {
            return;
        }
        pad->has_move = true;
        pad->move_yaw = view_yaw + std::atan2(left.stick_x, left.stick_y);
        pad->move_amount =
            kGameStickMinimum + (magnitude - kStickDeadzone) / (1.0f - kStickDeadzone) * (1.0f - kGameStickMinimum);
    }

    // The move direction as the game's stick (whose angle the game adds
    // GameCam's input yaw to), against the camera yaw right now.
    void WriteMoveStick(unsigned char *pad) {
        bool cam_ok = false;
        const float cam_yaw = GameCamInputYaw(&cam_ok);
        if (!cam_ok) {
            pad[kPadLeftX] = 128;
            pad[kPadLeftY] = 128;
            return;
        }
        const float stick_angle = g_pad.move_yaw - cam_yaw;
        pad[kPadLeftX] = ToStickByte(std::sin(stick_angle) * g_pad.move_amount);   // right
        pad[kPadLeftY] = ToStickByte(-std::cos(stick_angle) * g_pad.move_amount);  // forward; the pad's y points down
    }

    // Points the player straight along the movement (see the top comment),
    // only in the plain walking/running contexts.
    void FaceMovement() {
        __try {
            auto *object = *GameAddress<unsigned char *const>(kPlayers);
            if (object == nullptr) {
                return;
            }
            const signed char context = *reinterpret_cast<const signed char *>(object + kObjContext);
            if (context != -1 && context != 1) {
                return;
            }
            const auto angle = static_cast<unsigned short>(
                static_cast<int>(std::lround(g_pad.move_yaw * 65536.0f / (2.0f * kPi))) & 0xffff);
            *reinterpret_cast<unsigned short *>(object + kObjFacing) = angle;
            *reinterpret_cast<unsigned short *>(object + kObjMoveFacing) = angle;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // Right stick turns the view: smoothly (default) or in snaps.
    void MapTurn(const VrHand &right, float dt) {
        if (CameraSmoothTurn()) {
            const float x = right.stick_x;
            if (std::fabs(x) > kTurnDeadzone) {
                const float t = (std::fabs(x) - kTurnDeadzone) / (1.0f - kTurnDeadzone);
                CameraTurn(std::copysign(t * t * kSmoothTurnRate * dt, x));
            }
            return;
        }
        if (std::fabs(right.stick_x) < kSnapTurnRelease) {
            g_snap_armed = true;
        } else if (g_snap_armed && std::fabs(right.stick_x) > kSnapTurnPress) {
            g_snap_armed = false;
            CameraTurn(right.stick_x > 0.0f ? kSnapTurnRadians : -kSnapTurnRadians);
        }
    }

    unsigned MapButtons(const VrHand &left, const VrHand &right) {
        unsigned held = 0;
        if (right.buttons & kVrButtonA) held |= Mask(kMaskJump);
        // B attacks for characters without a saber (blasters etc.); sabers
        // hit by swinging. The triggers are the holster gesture.
        if (right.buttons & kVrButtonB) held |= Mask(kMaskAction);
        if (left.buttons & kVrButtonX) held |= Mask(kMaskSpecial);
        if (left.buttons & kVrButtonY) held |= Mask(kMaskTag);
        if (left.buttons & kVrButtonMenu) held |= Mask(kMaskStart);
        return held;
    }

    int __cdecl HookNuPadRead(unsigned char *pad, int flag) {
        NoteGameThread();
        const int result = g_real_pad_read(pad, flag);
        if (!g_pad.active || pad == nullptr || pad != *GameAddress<unsigned char *>(kGamePads)) {
            return result;
        }
        // Pressed edges only on the first read after each Present, so a game
        // that reads the pad twice per frame doesn't see two presses.
        const unsigned pressed = g_pressed_pending ? g_pad.pressed : 0;
        g_pressed_pending = false;
        if (result == 0) {
            // No physical controller: the Touch controllers are the pad.
            // Centre both sticks so stale device bytes don't move anything.
            if (!g_logged_no_device) {
                Log("controls: no physical pad on port 0; Touch controllers drive it alone");
                g_logged_no_device = true;
            }
            *reinterpret_cast<unsigned *>(pad + kPadHeld) = g_pad.held;
            *reinterpret_cast<unsigned *>(pad + kPadPressed) = pressed;
            pad[kPadLeftX] = g_pad.has_stick ? g_pad.stick_x : 128;
            pad[kPadLeftY] = g_pad.has_stick ? g_pad.stick_y : 128;
            pad[kPadRightX] = 128;
            pad[kPadRightY] = 128;
        } else {
            *reinterpret_cast<unsigned *>(pad + kPadHeld) |= g_pad.held;
            *reinterpret_cast<unsigned *>(pad + kPadPressed) |= pressed;
            if (g_pad.has_stick) {
                pad[kPadLeftX] = g_pad.stick_x;
                pad[kPadLeftY] = g_pad.stick_y;
            }
        }
        if (g_pad.has_move) {
            WriteMoveStick(pad);
            if (g_pad.face_move) {
                FaceMovement();
            }
        }
        return result == 0 ? 1 : result;
    }
} // namespace

void InstallVrControlsHook() {
    if (HookGameFunction("NuPadRead", kNuPadRead, kNuPadReadPrologue, sizeof(kNuPadReadPrologue),
                         reinterpret_cast<void *>(&HookNuPadRead), reinterpret_cast<void **>(&g_real_pad_read))) {
        Log("controls: left stick moves where you look, right stick turns, A jump, B attack, X special, Y switch, "
            "Menu start");
    }
}

void VrControlsOnPresent(unsigned frame) {
    if (g_real_pad_read == nullptr) {
        return;
    }
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float dt = g_last_present.QuadPart == 0
                         ? 0.0f
                         : std::min(0.1f, static_cast<float>(now.QuadPart - g_last_present.QuadPart) / frequency.QuadPart);
    g_last_present = now;
    const VrTrackingState *tracking = VrTracking();
    VirtualPad pad;
    if (tracking != nullptr) {
        const VrHand &left = tracking->hands[0];
        const VrHand &right = tracking->hands[1];
        pad.active = true;
        pad.held = MapButtons(left, right) | g_gesture_held;
        if (SimActive()) {
            // Simulation commands press buttons and push the stick directly.
            const unsigned sim = SimButtons();
            if (sim & kSimA) pad.held |= Mask(kMaskJump);
            if (sim & kSimB) pad.held |= Mask(kMaskAction);
            if (sim & kSimX) pad.held |= Mask(kMaskSpecial);
            if (sim & kSimY) pad.held |= Mask(kMaskTag);
            if (sim & kSimStart) pad.held |= Mask(kMaskStart);
            if (sim & kSimUp) pad.held |= Mask(kMaskStart + 0x1c);     // DUP @00802290
            if (sim & kSimDown) pad.held |= Mask(kMaskStart + 0x20);   // DDOWN @00802294
            if (sim & kSimLeft) pad.held |= Mask(kMaskStart + 0x24);   // DLEFT @00802298
            if (sim & kSimRight) pad.held |= Mask(kMaskStart + 0x28);  // DRIGHT @0080229c
            float sx = 0, sy = 0;
            if (SimStick(&sx, &sy)) {
                pad.has_stick = true;
                pad.stick_x = ToStickByte(sx);
                pad.stick_y = ToStickByte(-sy);
            }
        }
        if (!CameraCalibrating() && !CameraMenuOpen()) {  // else the sticks calibrate / menus use the pad
            MapLocomotion(left, &pad);
            MapTurn(right, dt);
        } else if (CameraMenuOpen() && !pad.has_stick && std::hypot(left.stick_x, left.stick_y) > kStickDeadzone) {
            // Menus: the left stick as a plain stick, to move the cursor.
            pad.has_stick = true;
            pad.stick_x = ToStickByte(left.stick_x);
            pad.stick_y = ToStickByte(-left.stick_y);
        }
    }
    g_gesture_held = 0;
    pad.pressed = pad.held & ~g_prev_held;
    g_prev_held = pad.held;
    g_moving = pad.has_move;  // the stick, not the room-scale catch-up below
    // Room scale: the player walked or leaned in the playspace, so the body
    // follows the head (gently, and only while the stick is idle).
    float catch_up_yaw = 0.0f;
    if (!pad.has_move && tracking != nullptr && !CameraMenuOpen() && CameraRoomCatchUp(&catch_up_yaw)) {
        pad.has_move = true;
        pad.move_yaw = catch_up_yaw;
        pad.move_amount = kCatchUpStick;
    }
    // Latched here: the first-person flag is this frame's (the pad is read
    // before the next camera set, when it is not yet re-established).
    pad.face_move = pad.has_move && CameraFirstPersonActive();
    g_pad = pad;
    g_pressed_pending = true;

    if (tracking != nullptr && frame % kLogEvery == 0) {
        float view_yaw = 0.0f;
        bool cam_ok = false;
        const bool view_ok = CameraViewYaw(&view_yaw);
        const float cam_yaw = GameCamInputYaw(&cam_ok);
        Log("controls: hands tracked L=%d R=%d, view yaw %.1f%s, GameCam input yaw %.1f%s, stick %s",
            tracking->hands[0].tracked, tracking->hands[1].tracked, view_yaw * 180.0f / kPi, view_ok ? "" : " (none)",
            cam_yaw * 180.0f / kPi, cam_ok ? "" : " (none)", pad.has_move ? "moving" : (pad.has_stick ? "sim" : "idle"));
        __try {
            const auto *object = *GameAddress<const unsigned char *const>(kPlayers);
            if (object != nullptr) {
                Log("controls: player context %d, facing %.1f",
                    *reinterpret_cast<const signed char *>(object + kObjContext),
                    *reinterpret_cast<const unsigned short *>(object + kObjFacing) * 360.0f / 65536.0f);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

void VrControlsHold(unsigned mask) {
    g_gesture_held |= mask;
}

unsigned VrControlsSpecialMask() {
    return g_real_pad_read != nullptr ? Mask(kMaskSpecial) : 0;
}

unsigned VrControlsActionMask() {
    return g_real_pad_read != nullptr ? Mask(kMaskAction) : 0;
}

bool VrControlsMoving() {
    return g_moving;
}
