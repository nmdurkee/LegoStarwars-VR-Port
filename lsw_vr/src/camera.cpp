// Engine camera hook. NuCameraSetEx (opensaga name) is the single function every
// camera goes through: it copies the NUCAMERA into global_camera, inverts its
// matrix into the view, and queues the render-side camera command. Changing
// the camera here (rather than the GPU matrices) keeps the engine's culling in
// sync with what is drawn.
//
// PC addresses (Ghidra VA, base 0x400000), found by tracing the D3D9 view
// upload back through the display-list CAMERA command (0x9a):
//   005ed540 NuCameraSetEx(NUCAMERA *cam, i32 fast)   cdecl
//   00603cc0 NuCameraSet(NUCAMERA *cam)               28 call sites
//   009736e8 global_camera (NUCAMERA, 0x78 bytes on PC)
//   0080b708 vmtx (view = inverse(camera matrix), scaled)
//   00925164 pNuCam (NUCAMERA *), the game camera MoveGameCamera sets
//   008021b4 GameCam (GAMECAMERA_s *), layout as opensaga (render_mtx +0xb8,
//            pos +0x11c, target +0x128)
//   0093d7f0 Player[8] (GameObject_s *); position +0x5c, facing_angle +0x58
//            (u16, 65536 = 360 deg, forward = (sin, 0, cos)), world matrix +0xb8

#include "camera.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>

#include "game.h"
#include "head_tracking.h"
#include "skeleton.h"
#include "log.h"
#include "mat4.h"
#include "vr/vr_controls.h"
#include "vr/vr_frame.h"

namespace {
    constexpr uintptr_t kNuCameraSetEx = 0x5ed540;
    constexpr uintptr_t kGlobalCamera = 0x9736e8;
    constexpr uintptr_t kNuCamPtr = 0x925164;
    constexpr uintptr_t kGameCamPtr = 0x8021b4;
    constexpr uintptr_t kPlayers = 0x93d7f0;
    constexpr uintptr_t kObjFacing = 0x58;
    constexpr uintptr_t kObjPosition = 0x5c;
    constexpr uintptr_t kObjMoveFacing = 0x5a;  // movement_facing_angle, u16
    constexpr uintptr_t kObjRadius = 0x1dc;     // collision radius (game units)
    constexpr uintptr_t kObjContext = 0x7b5;    // character_context (s8), -1/1 = plain movement
    // Standing still in VR, the (hidden) character turns to face where the
    // view faces, at up to kBodyTurnRate, so the game's "in front of the
    // player" (force targets, interactions) is where you look. While moving,
    // the controls point it along the movement instead (vr_controls).
    constexpr float kBodyTurnRate = 720.0f * 3.14159265f / 180.0f;
    constexpr uintptr_t kGameCamTarget = 0x128;
    // Menu stack (found via GetMenuID 0050b3f0 and the menu table users):
    // GameMenuLevel @00800944 (-1 = no menu), GameMenu[] entries 0xe0 bytes
    // with the menu index (u16) at 00921454 + level * 0xe0, MenuInfo[] @00800aa8
    // (0x1c each, id first; 0 titles, 2 pause, 1000+ memory card).
    constexpr uintptr_t kGameMenuLevel = 0x800944;
    constexpr uintptr_t kGameMenuIndex = 0x921454;
    constexpr uintptr_t kGameMenuStride = 0xe0;
    constexpr uintptr_t kMenuInfo = 0x800aa8;
    constexpr uintptr_t kMenuInfoStride = 0x1c;
    // sub esp,0xc8; push ebp; push esi
    constexpr unsigned char kNuCameraSetExPrologue[] = {0x81, 0xec, 0xc8, 0x00, 0x00, 0x00, 0x55, 0x56};

    constexpr float kTestYawDegrees = 25.0f;
    constexpr float kOffsetStep = 0.05f;
    constexpr unsigned kLogEvery = 300;  // frames

    // Row-vector matrices: rows 0-2 are the camera's right/up/forward axes,
    // row 3 its position (opensaga: camera->dir = row 2, position = m30..m32).
    struct NuCamera {
        float mtx[16];
        float fov;
        float aspect;
        float near_clip;
        float far_clip;
        unsigned char rest[0x78 - 0x50];
    };
    static_assert(sizeof(NuCamera) == 0x78, "PC NUCAMERA size");

    using NuCameraSetExFn = void(__cdecl *)(NuCamera *, int);
    NuCameraSetExFn g_real_set_ex = nullptr;

    bool g_test_yaw = false;
    // First person (F10 toggles): on by default with a headset, off on the desktop.
    bool g_first_person_vr = true;
    bool g_first_person_desktop = false;
    // VR first-person view yaw (radians, game convention). It changes only by
    // snap turning, never because the character turned, so the view never
    // rotates on its own. Starts at the character's facing.
    float g_body_yaw = 0.0f;
    bool g_body_yaw_valid = false;
    const void *g_body_yaw_player = nullptr;  // player the body yaw belongs to
    // Whether the game camera is at the player's eyes this frame, and last
    // frame (code that runs before this frame's camera set, e.g. the game's
    // update, goes by the latter).
    bool g_first_person_active = false;
    bool g_first_person_last = false;
    // Final yaw of the game camera as rendered (for head-relative locomotion).
    float g_view_yaw = 0.0f;
    bool g_view_yaw_valid = false;
    // First-person eye placement relative to the player's position, in game
    // units; tuned live with PgUp/PgDn (height) and Home/End (forward).
    // Eye position: g_eye_height above the character's neck (from its
    // skeleton; kDefaultNeck until known) and g_eye_forward in front of it.
    constexpr float kDefaultNeck = 0.26f;
    float g_eye_height = 0.03f;
    float g_eye_forward = 0.0f;
    // The eye follows the character's height through a short filter, so
    // steps and stairs glide instead of jolting the view (jumps barely lag).
    constexpr float kEyeHeightSeconds = 0.06f;
    constexpr float kEyeHeightSnap = 1.0f;  // game units: teleports, respawns
    float g_eye_base_y = 0.0f;
    bool g_eye_base_valid = false;
    LARGE_INTEGER g_eye_base_time = {};
    // Room scale in VR first person. The head may move away from the
    // character by up to kRoomRadiusFraction of its collision radius (the game
    // keeps that much clearance from walls, so the view can't pass through
    // one). Beyond that the character walks after the head (catch-up, through
    // the game's own movement and collision), and where it is blocked the
    // view stops at the limit. g_room_anchor is the world offset (x, z) of the
    // playspace from where the head maps: camera = player + head - anchor.
    constexpr float kRoomRadiusFraction = 0.8f;
    constexpr float kRoomMinRadius = 0.04f;  // game units
    constexpr float kRoomMaxRadius = 0.30f;
    constexpr float kDefaultRoomRadius = 0.12f;
    constexpr float kCatchUpStart = 0.5f;   // of the room radius
    constexpr float kCatchUpStop = 0.15f;
    // Near clip in VR first person, so a wall at arm's length isn't cut away.
    constexpr float kVrNearClip = 0.03f;
    float g_room_anchor[2] = {};
    float g_room_prev_player[2] = {};
    bool g_room_prev_valid = false;
    bool g_catch_up = false;
    float g_catch_up_yaw = 0.0f;
    float g_room_radius = kDefaultRoomRadius;
    // Comfort and aiming options (lsw_vr_settings.txt).
    bool g_smooth_turn = true;    // smooth_turn=1: right stick turns smoothly; 0: 30 degree snaps
    bool g_gun_aim_head = false;  // gun_aim=head: shots go where the head looks, else along the gun
    // Desktop window as a full-screen 16:9 view of the left eye (recording).
    bool g_pc_fullscreen = true;
    int g_recenter_request = -1;
    SHORT g_prev_keys[256] = {};
    volatile LONG g_calls_this_frame = 0;

    // Head tracking (F7 toggles, F8 recenters). The pose is latched once per
    // frame in CameraOnPresent; g_frame_meta records what the game camera was
    // rendered with, for the frame captured at the next Present.
    HeadTracker g_head;
    bool g_head_tracking = true;
    // Stereo by alternating eyes (F4 toggles mono): each frame is rendered from
    // one eye's pose, and the host shows each eye its latest image.
    bool g_stereo = true;
    int g_eye = 0;
    VrPose g_eye_pose = {};
    bool g_eye_pose_valid = false;

    // Render lag: if the game draws a frame with the camera set during the
    // previous update, each captured image must carry the previous frame's
    // pose (otherwise stereo shows each eye the other's image). Measured by
    // matching the view matrices the renderer uploads against the cameras we
    // set this frame and last frame; F5 overrides (auto -> 0 -> 1).
    float g_cam_pos[2][3] = {};  // [0] this frame, [1] previous frame
    bool g_cam_pos_valid[2] = {};
    unsigned g_lag_votes[2] = {};
    int g_lag_mode = -1;  // -1 auto, else forced 0/1
    int g_lag_auto = 0;   // auto-detected delay
    VrFrameMeta g_prev_meta = {};

    // Live calibration, saved to lsw_vr_settings.txt next to the game:
    //   world scale (game units per real meter: how big you are vs the world)
    //   stereo strength (multiplies the eye separation; 0 = flat, 1 = real)
    // Hold the left thumbstick click: left stick y = scale, right stick y =
    // stereo strength (the sticks then don't move the character); a quick
    // tap toggles stereo/mono. Keys: [ ] scale, - = stereo strength.
    float g_stereo_strength = 1.0f;
    bool g_calibrating = false;
    bool g_calibration_changed = false;
    float g_calibration_held = 0.0f;
    bool g_settings_loaded = false;
    LARGE_INTEGER g_last_present = {};

    void SettingsPath(char *path, DWORD size) {
        GetModuleFileNameA(nullptr, path, size);
        char *slash = strrchr(path, '\\');
        if (slash != nullptr) {
            strcpy_s(slash + 1, size - (slash + 1 - path), "lsw_vr_settings.txt");
        }
    }

    void LoadSettings() {
        char path[MAX_PATH];
        SettingsPath(path, MAX_PATH);
        FILE *file = nullptr;
        if (fopen_s(&file, path, "r") != 0 || file == nullptr) {
            return;
        }
        char line[128];
        while (fgets(line, sizeof(line), file) != nullptr) {
            float value = 0.0f;
            int flag = 0;
            if (sscanf_s(line, "world_scale=%f", &value) == 1 && value > 0.01f && value < 10.0f) {
                g_head.SetScale(value);
            } else if (sscanf_s(line, "stereo_strength=%f", &value) == 1 && value >= 0.0f && value <= 5.0f) {
                g_stereo_strength = value;
            } else if (sscanf_s(line, "stereo=%d", &flag) == 1) {
                g_stereo = flag != 0;
            } else if (sscanf_s(line, "eye_lift=%f", &value) == 1 && value > -0.5f && value < 1.0f) {
                g_eye_height = value;
            } else if (sscanf_s(line, "eye_forward=%f", &value) == 1 && value > -1.0f && value < 1.0f) {
                g_eye_forward = value;
            } else if (sscanf_s(line, "pc_fullscreen=%d", &flag) == 1) {
                g_pc_fullscreen = flag != 0;
            } else if (sscanf_s(line, "smooth_turn=%d", &flag) == 1) {
                g_smooth_turn = flag != 0;
            } else if (strncmp(line, "gun_aim=", 8) == 0) {
                g_gun_aim_head = strncmp(line + 8, "head", 4) == 0;
            }
        }
        fclose(file);
        Log("camera: %s turning, guns aim %s", g_smooth_turn ? "smooth" : "snap",
            g_gun_aim_head ? "where the head looks" : "along the gun");
        Log("camera: settings loaded: world scale %.2f, stereo strength %.2f, %s", g_head.Scale(), g_stereo_strength,
            g_stereo ? "stereo" : "mono");
    }

    void SaveSettings();
    void ResetRoomScale();

    // Live tuning from lsw_vr_tuner.exe: lsw_vr_tune.txt is re-read when it
    // changes, and the values become the saved settings.
    void PollTuneFile() {
        static FILETIME last = {};
        char path[MAX_PATH];
        SettingsPath(path, MAX_PATH);
        char *slash = strrchr(path, '\\');
        if (slash == nullptr) {
            return;
        }
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "lsw_vr_tune.txt");
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info) ||
            CompareFileTime(&info.ftLastWriteTime, &last) == 0) {
            return;
        }
        last = info.ftLastWriteTime;
        FILE *file = nullptr;
        if (fopen_s(&file, path, "r") != 0 || file == nullptr) {
            return;
        }
        char line[128];
        while (fgets(line, sizeof(line), file) != nullptr) {
            float value = 0.0f;
            int flag = 0;
            if (sscanf_s(line, "world_scale=%f", &value) == 1 && value > 0.01f && value < 10.0f) {
                g_head.SetScale(value);
            } else if (sscanf_s(line, "stereo_strength=%f", &value) == 1 && value >= 0.0f && value <= 5.0f) {
                g_stereo_strength = value;
            } else if (sscanf_s(line, "stereo=%d", &flag) == 1) {
                g_stereo = flag != 0;
            } else if (sscanf_s(line, "eye_lift=%f", &value) == 1 && value > -0.5f && value < 1.0f) {
                g_eye_height = value;
            } else if (sscanf_s(line, "pc_fullscreen=%d", &flag) == 1) {
                g_pc_fullscreen = flag != 0;
            } else if (sscanf_s(line, "recenter=%d", &flag) == 1) {
                if (g_recenter_request >= 0 && flag != g_recenter_request) {
                    g_head.Recenter();
                    g_body_yaw_valid = false;
                    ResetRoomScale();
                    Log("camera: recentered (tuner)");
                }
                g_recenter_request = flag;
            }
        }
        fclose(file);
        Log("camera: tuner: world scale %.2f, stereo strength %.2f, %s, eye height %.2f", g_head.Scale(),
            g_stereo_strength, g_stereo ? "stereo" : "mono", g_eye_height);
        SaveSettings();
    }

    void SaveSettings() {
        char path[MAX_PATH];
        SettingsPath(path, MAX_PATH);
        FILE *file = nullptr;
        if (fopen_s(&file, path, "w") != 0 || file == nullptr) {
            return;
        }
        fprintf(file,
                "world_scale=%.3f\nstereo_strength=%.3f\nstereo=%d\neye_lift=%.3f\neye_forward=%.3f\npc_fullscreen=%d\n"
                "smooth_turn=%d\ngun_aim=%s\n",
                g_head.Scale(), g_stereo_strength, g_stereo ? 1 : 0, g_eye_height, g_eye_forward,
                g_pc_fullscreen ? 1 : 0, g_smooth_turn ? 1 : 0, g_gun_aim_head ? "head" : "gun");
        fclose(file);
    }

    // The pose a stereo eye renders from: the eye's offset from the head,
    // scaled by the stereo strength.
    VrPose ScaledEye(const VrPose &eye, const VrPose &head) {
        VrPose out = eye;
        for (int i = 0; i < 3; ++i) {
            out.position[i] = head.position[i] + (eye.position[i] - head.position[i]) * g_stereo_strength;
        }
        return out;
    }
    VrFrameMeta g_frame_meta = {};
    bool g_logged_game_fov = false;
    // Menu open this frame (latched at Present): menus go on the floating screen.
    bool g_menu_open = false;
    int g_menu_id = -1;

    bool IsIdentity(const float *m) {
        for (int i = 0; i < 16; ++i) {
            if (std::fabs(m[i] - ((i % 5 == 0) ? 1.0f : 0.0f)) > 1e-4f) {
                return false;
            }
        }
        return true;
    }

    // Rotates the camera's axes about world Y, leaving its position alone.
    void Yaw(float *m, float degrees) {
        const float radians = degrees * 3.14159265f / 180.0f;
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        for (int row = 0; row < 3; ++row) {
            const float x = m[row * 4 + 0];
            const float z = m[row * 4 + 2];
            m[row * 4 + 0] = x * c + z * s;
            m[row * 4 + 2] = -x * s + z * c;
        }
    }

    struct PlayerState {
        float position[3];
        unsigned short facing;
        float radius;
    };

    // Player[0] may be mid-construction during level loads, so read defensively.
    bool ReadPlayer(int index, PlayerState *out) {
        __try {
            const auto *object = *GameAddress<const unsigned char *const>(kPlayers + index * 4);
            if (object == nullptr) {
                return false;
            }
            memcpy(out->position, object + kObjPosition, sizeof(out->position));
            out->facing = *reinterpret_cast<const unsigned short *>(object + kObjFacing);
            out->radius = *reinterpret_cast<const float *>(object + kObjRadius);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    float NeckHeight() {
        float height = 0.0f;
        return SkeletonNeckHeight(&height) && height > 0.02f && height < 3.0f ? height : kDefaultNeck;
    }

    // In VR the character always faces where the player looks (walking
    // included), so the body stays under and behind the view instead of
    // twisting round or being seen through from the side.
    void FollowHeadWithBody(float dt) {
        if (!g_view_yaw_valid || dt <= 0.0f) {
            return;
        }
        __try {
            auto *object = *GameAddress<unsigned char *const>(kPlayers);
            if (object == nullptr) {
                return;
            }
            // Only in plain walking/standing (contexts -1, 1). Using the Force
            // (8), Force push (0x1b), building and the like face their target
            // themselves, and the Force cancels if the character is turned
            // too far from it.
            const signed char context = *reinterpret_cast<const signed char *>(object + kObjContext);
            if (context != -1 && context != 1) {
                return;
            }
            auto *facing_u16 = reinterpret_cast<unsigned short *>(object + kObjFacing);
            const float facing = *facing_u16 * (2.0f * 3.14159265f / 65536.0f);
            const float diff = std::remainder(g_view_yaw - facing, 2.0f * 3.14159265f);
            if (std::fabs(diff) < 0.002f) {
                return;
            }
            const float step = std::copysign(std::min(std::fabs(diff), kBodyTurnRate * dt), diff);
            const float turned = facing + step;
            const auto value = static_cast<unsigned short>(
                static_cast<int>(std::lround(turned * 65536.0f / (2.0f * 3.14159265f))) & 0xffff);
            *facing_u16 = value;
            *reinterpret_cast<unsigned short *>(object + kObjMoveFacing) = value;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // The character's height for the eye, filtered (see kEyeHeightSeconds).
    float SmoothedBaseY(float y) {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const float dt = g_eye_base_time.QuadPart == 0
                             ? 0.0f
                             : std::min(0.1f, static_cast<float>(now.QuadPart - g_eye_base_time.QuadPart) /
                                                  frequency.QuadPart);
        g_eye_base_time = now;
        if (!g_eye_base_valid || std::fabs(y - g_eye_base_y) > kEyeHeightSnap) {
            g_eye_base_y = y;
            g_eye_base_valid = true;
        } else if (dt > 0.0f) {
            g_eye_base_y += (y - g_eye_base_y) * (1.0f - std::exp(-dt / kEyeHeightSeconds));
        }
        return g_eye_base_y;
    }

    // Room scale (see g_room_anchor): returns the world (x, z) to subtract from
    // the player's position so that the head, added by the head tracker,
    // stays within the room radius of the character.
    void UpdateRoomScale(const PlayerState &player, float yaw, float *shift_x, float *shift_z) {
        *shift_x = *shift_z = 0.0f;
        float head[3];
        if (!g_head.HeadOffset(yaw, head)) {
            return;
        }
        const float radius = std::isfinite(player.radius) && player.radius > 0.0f
                                 ? std::clamp(player.radius * kRoomRadiusFraction, kRoomMinRadius, kRoomMaxRadius)
                                 : kDefaultRoomRadius;
        if (std::fabs(radius - g_room_radius) > 1e-4f) {
            g_room_radius = radius;
            Log("camera: room scale: head may lean %.3f game units (%.2f m) from the body", radius,
                radius / std::max(g_head.Scale(), 0.01f));
        }
        // While the body walks after the head, the view stays where the head
        // is: the playspace moves with the body.
        if (g_room_prev_valid && g_catch_up && !VrControlsMoving()) {
            const float dx = player.position[0] - g_room_prev_player[0];
            const float dz = player.position[2] - g_room_prev_player[1];
            if (dx * dx + dz * dz < 0.5f * 0.5f) {  // not a teleport
                g_room_anchor[0] += dx;
                g_room_anchor[1] += dz;
            }
        }
        g_room_prev_player[0] = player.position[0];
        g_room_prev_player[1] = player.position[2];
        g_room_prev_valid = true;
        // The head's offset from the body, held within the radius: beyond it
        // the playspace slides along, so the view stops (at a wall, say).
        float dx = head[0] - g_room_anchor[0];
        float dz = head[2] - g_room_anchor[1];
        const float distance = std::sqrt(dx * dx + dz * dz);
        if (distance > radius) {
            const float excess = 1.0f - radius / distance;
            g_room_anchor[0] += dx * excess;
            g_room_anchor[1] += dz * excess;
            dx -= dx * excess;
            dz -= dz * excess;
        }
        // Catch-up: walk the body toward the head once it is well off centre.
        const float off = std::sqrt(dx * dx + dz * dz);
        if (!g_catch_up && off > kCatchUpStart * radius) {
            g_catch_up = true;
        } else if (g_catch_up && off < kCatchUpStop * radius) {
            g_catch_up = false;
        }
        g_catch_up_yaw = std::atan2(dx, dz);
        *shift_x = g_room_anchor[0];
        *shift_z = g_room_anchor[1];
    }

    void ResetRoomScale() {
        g_room_anchor[0] = g_room_anchor[1] = 0.0f;
        g_room_prev_valid = false;
        g_catch_up = false;
    }

    // Camera at the player's eyes. With a headset it faces the VR body yaw
    // (only turning changes it, never the character); on the desktop, where
    // the player faces. In VR only the hands are drawn, so the eye stands
    // right above the character's position, whichever way the body faces.
    bool BuildFirstPerson(float *m, bool vr) {
        PlayerState player;
        if (!ReadPlayer(0, &player)) {
            return false;
        }
        const float facing = player.facing * (2.0f * 3.14159265f / 65536.0f);
        if (vr && !g_body_yaw_valid) {
            g_body_yaw = facing;
            g_body_yaw_valid = true;
        }
        const float radians = vr ? g_body_yaw : facing;
        const float s = std::sin(radians);
        const float c = std::cos(radians);
        const float base_y = vr ? SmoothedBaseY(player.position[1]) : player.position[1];
        float shift_x = 0.0f, shift_z = 0.0f;
        if (vr) {
            UpdateRoomScale(player, radians, &shift_x, &shift_z);
        }
        const float rows[16] = {
            c,    0.0f, -s,   0.0f,  // right
            0.0f, 1.0f, 0.0f, 0.0f,  // up
            s,    0.0f, c,    0.0f,  // forward
            player.position[0] + s * g_eye_forward - shift_x, base_y + NeckHeight() + g_eye_height,
            player.position[2] + c * g_eye_forward - shift_z, 1.0f};
        memcpy(m, rows, sizeof(rows));
        return true;
    }

    // Game camera field of view to match the headset. NUCAMERA.fov is the
    // vertical angle in radians in opensaga; a value above pi would mean
    // degrees, so convert in that case rather than collapse the view.
    bool ApplyHeadsetFov(NuCamera *cam) {
        if (!g_logged_game_fov) {
            Log("camera: game fov=%.4f aspect=%.4f (before headset fov)", cam->fov, cam->aspect);
            g_logged_game_fov = true;
        }
        const bool degrees = cam->fov > 3.2f;
        float fov = 0.0f, aspect = 0.0f;
        if (!g_head.Fov(&fov, &aspect, &g_frame_meta.fov)) {
            return false;  // keep the game's own field of view
        }
        cam->aspect = aspect;
        cam->fov = degrees ? fov * 180.0f / 3.14159265f : fov;
        return true;
    }

    // Whether a menu (titles, pause, shop, character/level select...) is open,
    // and its id. Read directly with range checks: GetMenuID itself doesn't
    // handle "no menu".
    bool ReadMenu(int *id) {
        *id = -1;
        __try {
            const int level = *GameAddress<const int>(kGameMenuLevel);
            if (level < 0 || level >= 10) {
                return false;
            }
            // During play the stack keeps a level with an empty slot (menu -1,
            // as opensaga's "GameMenu[GameMenuLevel].menu == -1" check): no menu.
            const int index = *GameAddress<const short>(kGameMenuIndex + level * kGameMenuStride);
            if (index < 0 || index >= 100) {
                return false;
            }
            *id = *GameAddress<const int>(kMenuInfo + index * kMenuInfoStride);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // The game camera (pNuCam) in first person and/or head tracked. Returns
    // false if nothing applies. With a menu open the game's own camera is kept
    // (no pose, so the host shows the frame on the floating screen).
    bool AdjustGameCamera(NuCamera *copy) {
        if (g_menu_open) {
            g_first_person_active = false;
            return false;
        }
        bool changed = false;
        const bool vr = g_head_tracking && g_head.HasPose();
        if (vr ? g_first_person_vr : g_first_person_desktop) {
            changed = BuildFirstPerson(copy->mtx, vr);
            g_first_person_active = changed;
        } else {
            g_first_person_active = false;
            ResetRoomScale();
        }
        if (vr) {
            const bool stereo = g_stereo && g_eye_pose_valid;
            const VrPose view = stereo ? ScaledEye(g_eye_pose, g_head.RawPose()) : g_head.RawPose();
            g_head.Apply(copy->mtx, view);
            // Without the headset's field of view the frame can't be shown
            // world-locked (it goes on the floating screen instead).
            if (g_first_person_active) {
                copy->near_clip = std::min(copy->near_clip, kVrNearClip);
            }
            g_frame_meta.has_pose = !ApplyHeadsetFov(copy) ? kVrFrameScreen
                                    : stereo             ? (g_eye == 0 ? kVrFrameLeftEye : kVrFrameRightEye)
                                                         : kVrFrameMono;
            g_frame_meta.pose = view;
            g_cam_pos[0][0] = copy->mtx[12], g_cam_pos[0][1] = copy->mtx[13], g_cam_pos[0][2] = copy->mtx[14];
            g_cam_pos_valid[0] = true;
            changed = true;
        }
        return changed;
    }

    void __cdecl HookNuCameraSetEx(NuCamera *cam, int fast) {
        InterlockedIncrement(&g_calls_this_frame);
        // global_camera is re-set from itself (e.g. DrawGameObjects); it already
        // holds our change, so applying it again would double it.
        if (cam != nullptr && !IsIdentity(cam->mtx) && cam != GameAddress<NuCamera>(kGlobalCamera)) {
            // Change a copy so the game's own camera struct is untouched.
            NuCamera copy = *cam;
            bool changed = false;
            if (cam == *GameAddress<NuCamera *>(kNuCamPtr)) {
                changed = AdjustGameCamera(&copy);
                // Heading of the view: its forward, leaning on its up as the
                // gaze tilts toward straight down or up (where the forward
                // alone has no heading).
                const float *m = copy.mtx;
                g_view_yaw = std::atan2(m[8] - m[4] * m[9], m[10] - m[6] * m[9]);
                g_view_yaw_valid = true;
            }
            if (!changed && g_test_yaw) {
                Yaw(copy.mtx, kTestYawDegrees);
                changed = true;
            }
            if (changed) {
                g_real_set_ex(&copy, fast);
                return;
            }
        }
        g_real_set_ex(cam, fast);
    }

    bool Pressed(int key) {
        const SHORT down = GetAsyncKeyState(key) & 0x8000;
        const bool pressed = down && !g_prev_keys[key];
        g_prev_keys[key] = down;
        return pressed;
    }
} // namespace

void InstallCameraHook() {
    if (HookGameFunction("NuCameraSetEx", kNuCameraSetEx, kNuCameraSetExPrologue, sizeof(kNuCameraSetExPrologue),
                         reinterpret_cast<void *>(&HookNuCameraSetEx), reinterpret_cast<void **>(&g_real_set_ex))) {
        Log("camera: F4 stereo/mono, F5 frame delay auto/0/1, F7 head tracking, F8 recenter, F9 %.0f deg test yaw, F10 first person, "
            "PgUp/PgDn eye height, Home/End eye forward",
            kTestYawDegrees);
    }
}

void CameraOnPresent(unsigned frame) {
    if (g_real_set_ex == nullptr) {
        return;
    }
    // Menus: latch for the coming frame, and log changes.
    int menu_id = -1;
    const bool menu_open = ReadMenu(&menu_id);
    if (menu_open != g_menu_open || (menu_open && menu_id != g_menu_id)) {
        if (menu_open) {
            Log("camera: menu %d open -> floating screen", menu_id);
        } else {
            Log("camera: menu closed -> world-locked view");
        }
    }
    g_menu_open = menu_open;
    g_menu_id = menu_id;

    // Latch this frame's head pose (VrLatchTracking ran first) before the
    // game sets its cameras.
    const VrTrackingState *tracking = VrTracking();
    g_head.Latch(tracking != nullptr ? &tracking->head : nullptr);
    // Next frame renders the other eye.
    // Decide the render lag from the last ~2 s of votes.
    static unsigned lag_report = 0;
    if (++lag_report % 300 == 0) {
        Log("camera: frame-delay votes: this-frame camera %u, previous-frame camera %u (using %d)", g_lag_votes[0],
            g_lag_votes[1], g_lag_mode >= 0 ? g_lag_mode : g_lag_auto);
    }
    if (g_lag_votes[0] + g_lag_votes[1] >= 60) {
        const int detected = g_lag_votes[1] > 2 * g_lag_votes[0] ? 1 : (g_lag_votes[0] > 2 * g_lag_votes[1] ? 0 : -1);
        if (detected >= 0 && detected != g_lag_auto) {
            g_lag_auto = detected;
            Log("camera: game renders %s frame late (votes %u vs %u); frame labels adjusted",
                detected ? "one" : "no", g_lag_votes[1], g_lag_votes[0]);
        }
        g_lag_votes[0] = g_lag_votes[1] = 0;
    }
    // Controller shortcuts (the keyboard is out of reach in the headset, and
    // compact keyboards may send media keys from the F-row): left thumbstick
    // click cycles the frame delay, right thumbstick click recenters.
    static unsigned previous_buttons[2] = {};
    unsigned clicked[2] = {};
    for (int side = 0; side < 2; ++side) {
        const unsigned buttons = tracking != nullptr ? tracking->hands[side].buttons : 0;
        clicked[side] = buttons & ~previous_buttons[side];
        previous_buttons[side] = buttons;
    }
    if (!g_settings_loaded) {
        LoadSettings();
        g_settings_loaded = true;
    }
    if (frame % 15 == 0) {
        PollTuneFile();
    }
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float dt = g_last_present.QuadPart == 0
                         ? 0.0f
                         : std::min(0.1f, static_cast<float>(now.QuadPart - g_last_present.QuadPart) / frequency.QuadPart);
    g_last_present = now;
    if (g_first_person_active && g_head.HasPose() && !g_menu_open && !VrControlsMoving()) {
        FollowHeadWithBody(dt);
    }

    if (clicked[1] & kVrButtonStick) {
        g_head.Recenter();
        g_body_yaw_valid = false;
        ResetRoomScale();
        Log("camera: recentered (right stick click)");
    }
    // Calibration: hold the left stick click.
    const bool holding = tracking != nullptr && (tracking->hands[0].buttons & kVrButtonStick) != 0;
    if (holding) {
        g_calibration_held += dt;
        const float scale_input = tracking->hands[0].stick_y;
        const float stereo_input = tracking->hands[1].stick_y;
        if (std::fabs(scale_input) > 0.3f) {
            // Proportional: about x2 per 1.5 s at full deflection.
            g_head.SetScale(std::clamp(g_head.Scale() * std::exp(scale_input * 0.46f * dt), 0.03f, 5.0f));
            g_calibration_changed = true;
        }
        if (std::fabs(stereo_input) > 0.3f) {
            g_stereo_strength = std::clamp(g_stereo_strength + stereo_input * 0.6f * dt, 0.0f, 3.0f);
            g_calibration_changed = true;
        }
        g_calibrating = g_calibration_held > 0.25f || g_calibration_changed;
    } else if (g_calibration_held > 0.0f) {
        if (!g_calibration_changed && g_calibration_held < 0.4f) {
            g_stereo = !g_stereo;  // quick tap
            Log("camera: %s (left stick tap)", g_stereo ? "stereo (alternating eyes)" : "mono");
            SaveSettings();
        } else if (g_calibration_changed) {
            Log("camera: calibrated: world scale %.3f, stereo strength %.2f", g_head.Scale(), g_stereo_strength);
            SaveSettings();
        }
        g_calibration_held = 0.0f;
        g_calibration_changed = false;
        g_calibrating = false;
    }
    if (Pressed(VK_OEM_MINUS) | Pressed(VK_OEM_PLUS)) {  // - and = (both polled every frame)
        const float step = GetAsyncKeyState(VK_OEM_PLUS) & 0x8000 ? 0.1f : -0.1f;
        g_stereo_strength = std::clamp(g_stereo_strength + step, 0.0f, 3.0f);
        Log("camera: stereo strength %.2f", g_stereo_strength);
        SaveSettings();
    }
    if (Pressed(VK_F5)) {
        g_lag_mode = g_lag_mode == -1 ? 0 : (g_lag_mode == 0 ? 1 : -1);
        Log("camera: frame delay %s", g_lag_mode == -1 ? "auto" : (g_lag_mode == 0 ? "0 (forced)" : "1 (forced)"));
    }
    // This frame's camera becomes last frame's.
    memcpy(g_cam_pos[1], g_cam_pos[0], sizeof(g_cam_pos[0]));
    g_cam_pos_valid[1] = g_cam_pos_valid[0];
    g_cam_pos_valid[0] = false;

    g_eye = 1 - g_eye;
    g_eye_pose_valid = false;
    if (tracking != nullptr) {
        const float *q = tracking->eyes[g_eye].orientation;
        if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] > 0.5f) {
            g_eye_pose = tracking->eyes[g_eye];
            g_eye_pose_valid = true;
        }
    }
    if (Pressed(VK_F4)) {
        g_stereo = !g_stereo;
        Log("camera: %s", g_stereo ? "stereo (alternating eyes)" : "mono");
        SaveSettings();
    }
    if (Pressed(VK_F7)) {
        g_head_tracking = !g_head_tracking;
        Log("camera: head tracking %s", g_head_tracking ? "on" : "off");
    }
    if (Pressed(VK_OEM_4) | Pressed(VK_OEM_6)) {  // [ and ] (both polled every frame)
        const float step = GetAsyncKeyState(VK_OEM_6) & 0x8000 ? 0.05f : -0.05f;
        g_head.SetScale(std::max(0.05f, g_head.Scale() + step));
        Log("camera: world scale %.2f game units per meter", g_head.Scale());
        SaveSettings();
    }
    if (Pressed(VK_F8)) {
        g_head.Recenter();
        g_body_yaw_valid = false;
        ResetRoomScale();  // face where the character faces again
        Log("camera: recentered");
    }
    if (Pressed(VK_F9)) {
        g_test_yaw = !g_test_yaw;
        Log("camera: test yaw %s", g_test_yaw ? "on" : "off");
    }
    if (Pressed(VK_F10)) {
        bool &first_person = g_head.HasPose() ? g_first_person_vr : g_first_person_desktop;
        first_person = !first_person;
        Log("camera: first person %s (%s)", first_person ? "on" : "off", g_head.HasPose() ? "VR" : "desktop");
    }
    const float old_height = g_eye_height;
    const float old_forward = g_eye_forward;
    // Ctrl+Home/End belong to the tuner (eye height).
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (Pressed(VK_HOME) && !ctrl) g_eye_forward += kOffsetStep;
    if (Pressed(VK_END) && !ctrl) g_eye_forward -= kOffsetStep;
    if (g_eye_height != old_height || g_eye_forward != old_forward) {
        Log("camera: eye height %.2f, forward %.2f", g_eye_height, g_eye_forward);
        SaveSettings();
    }

    const LONG calls = InterlockedExchange(&g_calls_this_frame, 0);
    if (frame % kLogEvery == 0) {
        const auto *global = GameAddress<const NuCamera>(kGlobalCamera);
        Log("camera: frame %u, NuCameraSetEx x%ld; global_camera fov=%.3f aspect=%.3f near=%.3f far=%.1f "
            "pos=(%.2f %.2f %.2f) fwd=(%.3f %.3f %.3f)",
            frame, calls, global->fov, global->aspect, global->near_clip, global->far_clip, global->mtx[12],
            global->mtx[13], global->mtx[14], global->mtx[8], global->mtx[9], global->mtx[10]);
        PlayerState player;
        if (ReadPlayer(0, &player)) {
            const auto *game_cam = *GameAddress<const unsigned char *const>(kGameCamPtr);
            const auto *target = game_cam != nullptr
                                     ? reinterpret_cast<const float *>(game_cam + kGameCamTarget)
                                     : nullptr;
            Log("camera: Player[0] pos=(%.2f %.2f %.2f) facing=%u (%.1f deg); GameCam target=(%.2f %.2f %.2f)",
                player.position[0], player.position[1], player.position[2], player.facing,
                player.facing * 360.0f / 65536.0f, target ? target[0] : 0.0f, target ? target[1] : 0.0f,
                target ? target[2] : 0.0f);
        }
    }
}

VrFrameMeta CameraTakeFrameMeta() {
    const VrFrameMeta meta = g_frame_meta;
    g_frame_meta.has_pose = 0;
    const int delay = g_lag_mode >= 0 ? g_lag_mode : g_lag_auto;
    const VrFrameMeta previous = g_prev_meta;
    g_prev_meta = meta;
    return delay == 1 ? previous : meta;
}

void CameraOnShaderView(const float *upload) {
    if (!g_cam_pos_valid[0] || !g_cam_pos_valid[1]) {
        return;
    }
    // Uploaded transposed: the engine's row-vector view is the transpose.
    float view[16], camera[16];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            view[r * 4 + c] = upload[c * 4 + r];
        }
    }
    if (!Mat4InverseAffine(camera, view)) {
        return;
    }
    float distance[2];
    for (int i = 0; i < 2; ++i) {
        const float dx = camera[12] - g_cam_pos[i][0], dy = camera[13] - g_cam_pos[i][1], dz = camera[14] - g_cam_pos[i][2];
        distance[i] = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    // Only the main camera (not shadow or HUD views), and only when the two
    // candidates are distinguishable.
    if (std::min(distance[0], distance[1]) < 0.2f && std::fabs(distance[0] - distance[1]) > 0.002f) {
        ++g_lag_votes[distance[1] < distance[0] ? 1 : 0];
    }
}

bool CameraViewYaw(float *radians) {
    *radians = g_view_yaw;
    return g_view_yaw_valid;
}

void CameraTurn(float radians) {
    g_body_yaw += radians;
}

bool CameraFirstPersonActive() {
    return g_first_person_active;
}

bool CameraFirstPersonRecent() {
    return (g_first_person_active || g_first_person_last) && !g_menu_open;
}

bool CameraTrackingToWorld(const VrPose &pose, float position[3], float axes[9]) {
    return g_head_tracking && g_head.HasPose() && g_head.ToWorld(pose, position, axes);
}

bool CameraRoomCatchUp(float *yaw) {
    *yaw = g_catch_up_yaw;
    if (!g_catch_up || !g_first_person_active) {
        return false;
    }
    // Only in plain walking/standing (contexts -1, 1). Mid-Force (8), building
    // and the like, a stick push moves or turns the body away from the target
    // and the game cancels the action; the view just stops at the room radius.
    __try {
        const auto *object = *GameAddress<const unsigned char *const>(kPlayers);
        if (object == nullptr) {
            return false;
        }
        const signed char context = *reinterpret_cast<const signed char *>(object + kObjContext);
        return context == -1 || context == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CameraSmoothTurn() {
    return g_smooth_turn;
}

bool CameraGunAimFromHead() {
    return g_gun_aim_head;
}

void CameraSetEye(float lift, float forward) {
    g_eye_height = lift;
    g_eye_forward = forward;
}

bool CameraPcFullscreen() {
    return g_pc_fullscreen;
}

float CameraWorldScale() {
    return g_head.Scale();
}

void CameraEndFrame() {
    // Re-established by the next frame's camera set; until then the frame
    // goes by this one (CameraFirstPersonRecent).
    g_first_person_last = g_first_person_active && !g_menu_open;
    g_first_person_active = false;
    // A new player object (level load, respawn, switch) starts facing its own way.
    const void *player = nullptr;
    __try {
        player = *GameAddress<const void *const>(kPlayers);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (player != g_body_yaw_player) {
        g_body_yaw_player = player;
        g_body_yaw_valid = false;
        g_eye_base_valid = false;
        ResetRoomScale();
    }
}

bool CameraMenuOpen() {
    return g_menu_open;
}

bool CameraCalibrating() {
    return g_calibrating;
}

void CameraSetWorldScale(float scale) {
    g_head.SetScale(std::clamp(scale, 0.03f, 5.0f));
}

void CameraSetStereoStrength(float strength) {
    g_stereo_strength = std::clamp(strength, 0.0f, 3.0f);
}

void CameraSetStereo(bool stereo) {
    g_stereo = stereo;
}

void CameraSetFirstPerson(bool on) {
    g_first_person_vr = on;
}

void CameraDescribe(char *out, size_t size) {
    const auto *global = GameAddress<const NuCamera>(kGlobalCamera);
    sprintf_s(out, size,
              "camera: world scale %.3f, stereo strength %.2f, %s, first person %d, menu %d; global_camera pos %.3f %.3f "
              "%.3f fwd %.3f %.3f %.3f fov %.3f aspect %.3f near %.3f",
              g_head.Scale(), g_stereo_strength, g_stereo ? "stereo" : "mono", g_first_person_active ? 1 : 0, g_menu_id,
              global->mtx[12], global->mtx[13], global->mtx[14], global->mtx[8], global->mtx[9], global->mtx[10],
              global->fov, global->aspect, global->near_clip);
}
