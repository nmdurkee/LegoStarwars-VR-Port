// VR interaction for player 1: physical saber hits, hip holster, grip-to-build.
//
// Controller poses map into the game world through the camera's tracking
// transform (CameraTrackingToWorld), so hands, head and saber agree with what
// is drawn. Game functions are called from Present, after the frame's update.
//
// PC addresses / offsets (Ghidra VA; found statically, see memory notes):
//   00438230 ObjHitObj(attacker, target, damage, flags, probe, 1)
//   0058b720 ObjHitObj_Flags(obj); a sabre hit ORs 0x100 (as LightSabreStreakCode)
//   00569c70 GizmoBlowUp_Hit(obj, points, count, radius, min, max, bolt, type, flags):
//            breaks breakable props; LightSabreStreakCode passes the blade's
//            base, tip and middle, radius = length/4, box = middle +- 1.5 length
//   0058d480 FastWeaponOut(obj, sound), 0058d930 FastWeaponIn(obj, sound),
//   0058d8b0 KeepWeaponOut(obj)
//   Obj array @0093d504, HIGHGAMEOBJECT @0093d500, GameObject size 0x10d8
//   object: +0x1fc flags (&1, &0x1000 live; &0x80 player), +0x28b must be 0,
//     +0x80 collision_position, +0x1dc/+0x1e0 radius/height, +0x107f hp,
//     +0x7b5 character_context, +0x798 current buildit, +0xd3c weapon_scale,
//     +0xd44 sabre_collision_radius (blade length / 4), +0xe33 weapon state
//   GAMECHARACTERDATA (object +0x54 -> +0x24) +0x100 streak_joints (sabre user
//     if >= 0)
//   00580950 Bolt_GetShootDirection_Default(obj, dir) -> u16 heading: the
//     default BoltSys->shoot_direction (BoltSys_Default @00804198 +0x14), which
//     every shot asks for its direction; hooked so the player aims with the
//     gun-hand controller
//   00582e70 Bolt_Add(owner, position, matrix, type, flags) -> BOLT*: creates
//     every bolt from its shot matrix (rows right/up/forward), which the fire
//     code (00583af0) aims at its auto-target; hooked so the player's bolts fly
//     exactly along the aim ray instead (no aim assist)
//   Bolt[32] @00951ce8, 0x138 each (BOLT_s as opensaga; found from the bolt
//     allocator 0057fba0 and Bolt_Add's init 00582470): +0x04 owner, +0x08
//     shot matrix (rows right/up/forward), +0x48 effect matrix (a fixed turn
//     times the shot matrix), +0x88 position, +0x94 previous position, +0xa0
//     velocity, +0xac unit direction, +0xc8 time, +0xd8 collision radius,
//     +0xf0 flags (0x10000000 = reflected), +0xf8 hit flags (u16, the owner's
//     ObjHitObj_Flags | 0x80), +0x100 active
//   WORLD @00802c54 -> +0x46a8 buildit system (+0 array, +4 u16 count, 0x7c
//     each: +0x28 start_position, +0x40 radius, +0x6d build_state, +0x7a
//     availability &3); LEGOCONTEXT_BUILDIT value @00802860

#include "vr_interaction.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../camera.h"
#include "../game.h"
#include "../log.h"
#include "../skeleton.h"
#include "vr_controls.h"
#include "vr_frame.h"

namespace {
    // Game addresses.
    constexpr uintptr_t kPlayers = 0x93d7f0;
    constexpr uintptr_t kObjArray = 0x93d504;
    constexpr uintptr_t kObjHigh = 0x93d500;
    constexpr uintptr_t kObjStride = 0x10d8;
    constexpr uintptr_t kWorldPtr = 0x802c54;
    constexpr uintptr_t kContextBuildIt = 0x802860;

    // Object fields.
    constexpr uintptr_t kObjFlags = 0x1fc;
    constexpr uintptr_t kObjDead = 0x28b;
    constexpr uintptr_t kObjCollisionPos = 0x80;
    constexpr uintptr_t kObjRadius = 0x1dc;
    constexpr uintptr_t kObjHeight = 0x1e0;
    constexpr uintptr_t kObjHp = 0x107f;
    constexpr uintptr_t kObjCharacterData = 0x54;
    constexpr uintptr_t kObjContext = 0x7b5;
    constexpr uintptr_t kObjBuildIt = 0x798;
    constexpr uintptr_t kObjWeaponScale = 0xd3c;
    constexpr uintptr_t kObjSabreRadius = 0xd44;
    constexpr uintptr_t kObjWeaponState = 0xe33;
    constexpr uintptr_t kCharDataGameCharacter = 0x24;
    constexpr uintptr_t kGameCharStreakJoints = 0x100;

    // Bolts (blaster shots).
    constexpr uintptr_t kBolts = 0x951ce8;
    constexpr int kBoltCount = 32;
    constexpr uintptr_t kBoltStride = 0x138;
    constexpr uintptr_t kBoltOwner = 0x04;
    constexpr uintptr_t kBoltMatrix = 0x08;
    constexpr uintptr_t kBoltEffectMatrix = 0x48;
    constexpr uintptr_t kBoltPosition = 0x88;
    constexpr uintptr_t kBoltPrevious = 0x94;
    constexpr uintptr_t kBoltVelocity = 0xa0;
    constexpr uintptr_t kBoltDirection = 0xac;
    constexpr uintptr_t kBoltTime = 0xc8;
    constexpr uintptr_t kBoltRadius = 0xd8;
    constexpr uintptr_t kBoltFlags = 0xf0;
    constexpr uintptr_t kBoltHitFlags = 0xf8;
    constexpr uintptr_t kBoltActive = 0x100;
    constexpr unsigned kBoltReflected = 0x10000000;

    // Build-it system.
    constexpr uintptr_t kWorldBuildIts = 0x46a8;
    constexpr uintptr_t kBuildItStride = 0x7c;
    constexpr uintptr_t kBuildItStart = 0x28;
    constexpr uintptr_t kBuildItRadius = 0x40;
    constexpr uintptr_t kBuildItState = 0x6d;
    constexpr uintptr_t kBuildItAvailability = 0x7a;

    // Game functions (first bytes checked before use).
    constexpr unsigned char kObjHitObjPrologue[] = {0x83, 0xec, 0x4c, 0xa1, 0x70, 0x95, 0x82, 0x00};
    constexpr unsigned char kObjHitObjFlagsPrologue[] = {0x8b, 0x54, 0x24, 0x04, 0x33, 0xc0, 0x85, 0xd2};
    constexpr unsigned char kFastWeaponPrologue[] = {0x56, 0x8b, 0x74, 0x24, 0x08, 0x8a, 0x86, 0xb5};
    constexpr unsigned char kKeepWeaponOutPrologue[] = {0x8b, 0x44, 0x24, 0x04, 0xd9, 0xe8, 0x81, 0x88};
    using ObjHitObjFn = int(__cdecl *)(void *attacker, void *target, int damage, int flags, int probe, int one);
    using ObjHitObjFlagsFn = int(__cdecl *)(void *object);
    using BlowUpHitFn = void *(__cdecl *)(void *object, float *points, int count, float radius, float *minimum,
                                          float *maximum, void *bolt, unsigned hit_type, unsigned char *flags);
    constexpr unsigned char kBlowUpHitPrologue[] = {0x83, 0xec, 0x28, 0xa1, 0xac, 0x07, 0x95, 0x00};
    using WeaponFn = void(__cdecl *)(void *object, int sound);
    using KeepWeaponOutFn = void(__cdecl *)(void *object);
    using ShootDirectionFn = int(__cdecl *)(void *object, float *direction);
    // mov eax,[esp+8]; test eax,eax; jz +0x39
    constexpr unsigned char kShootDirectionPrologue[] = {0x8b, 0x44, 0x24, 0x08, 0x85, 0xc0, 0x74, 0x39};
    ShootDirectionFn g_real_shoot_direction = nullptr;
    using BoltAddFn = void *(__cdecl *)(void *owner, float *position, float *matrix, int type, int flags);
    // mov eax,[esp+0x10]; mov ecx,[BoltSys]
    constexpr unsigned char kBoltAddPrologue[] = {0x8b, 0x44, 0x24, 0x10, 0x8b, 0x0d, 0xbc, 0x40};
    BoltAddFn g_real_bolt_add = nullptr;
    // This frame's gun aim (world direction), set at Present, used by the hook.
    bool g_aim_valid = false;
    float g_aim[3] = {};
    bool g_firing = false;
    ObjHitObjFn g_obj_hit_obj = nullptr;
    ObjHitObjFlagsFn g_obj_hit_obj_flags = nullptr;
    BlowUpHitFn g_blowup_hit = nullptr;  // optional: breaking props
    WeaponFn g_fast_weapon_out = nullptr;
    WeaponFn g_fast_weapon_in = nullptr;
    KeepWeaponOutFn g_keep_weapon_out = nullptr;
    bool g_installed = false;

    // Tuning (real-world units unless noted).
    constexpr float kSwingSpeed = 1.5f;        // m/s at the blade tip to deal damage
    constexpr float kHardSwingSpeed = 4.0f;    // m/s for double damage
    constexpr float kRealBladeLength = 1.0f;   // m, for measuring tip speed
    constexpr float kHitCooldown = 0.35f;      // s per target
    constexpr float kHipDrop = 0.55f;          // m below the head
    constexpr float kHipSide = 0.20f;          // m to the side
    constexpr float kHipRadius = 0.22f;        // m
    constexpr float kTriggerPress = 0.7f;
    constexpr float kTriggerRelease = 0.4f;
    constexpr float kSqueezePress = 0.6f;
    constexpr float kPileMargin = 0.25f;       // game units around a pile's radius
    constexpr float kDefaultBladeLength = 0.5f;  // game units, if the game's is unset
    constexpr unsigned kLogEvery = 300;
    // The game's own extend takes 0.2 s; if the blade still isn't out after
    // this (the game keeps it in, e.g. after a respawn), force it out.
    constexpr float kSaberStuckSeconds = 0.75f;
    constexpr float kDeflectMargin = 0.04f;  // game units added to bolt + blade radius

    struct Vec3 {
        float x, y, z;
    };
    Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    float Length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
    Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
    float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    Vec3 Unit(Vec3 v) {
        const float length = Length(v);
        return length > 1e-6f ? v * (1.0f / length) : Vec3{0.0f, 0.0f, 0.0f};
    }

    // Rotates v by quaternion q (x, y, z, w), in tracking space.
    Vec3 Rotate(const float *q, Vec3 v) {
        const float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
        const float tx = 2.0f * (qy * v.z - qz * v.y);
        const float ty = 2.0f * (qz * v.x - qx * v.z);
        const float tz = 2.0f * (qx * v.y - qy * v.x);
        return {v.x + qw * tx + (qy * tz - qz * ty), v.y + qw * ty + (qz * tx - qx * tz),
                v.z + qw * tz + (qx * ty - qy * tx)};
    }
    Vec3 PosOf(const VrPose &pose) { return {pose.position[0], pose.position[1], pose.position[2]}; }

    template <typename T> T Field(const void *object, uintptr_t offset) {
        T value;
        memcpy(&value, static_cast<const unsigned char *>(object) + offset, sizeof(T));
        return value;
    }
    Vec3 FieldVec(const void *object, uintptr_t offset) {
        return {Field<float>(object, offset), Field<float>(object, offset + 4), Field<float>(object, offset + 8)};
    }

    // ---- State ---------------------------------------------------------------

    struct HitCooldown {
        const void *target;
        float remaining;
    };

    const void *g_player = nullptr;  // player the state below belongs to
    int g_saber_hand = -1;           // hand that drew the saber, -1 = holstered
    // Hand the player last drew the saber into and hasn't holstered from: after
    // a death or respawn (the game puts it away) it comes back out there.
    int g_saber_wanted = -1;
    // Controller the saber was last drawn into (the VR hands put the blade in
    // it, even holstered, so the hands don't swap back and forth).
    int g_saber_last = 1;
    // Holstered on purpose: don't take the saber as drawn again until the
    // game has actually put it away (the blade retracts over a few frames).
    bool g_holstering = false;
    float g_in_play_seconds = 0.0f;
    int g_frames_not_drawn = 0;
    float g_weapon_out_cooldown = 0.0f;
    float g_saber_stuck = 0.0f;  // seconds the drawn saber has been less than fully out
    unsigned g_deflects = 0;

    // The previous frame's blade (game world) for the swept hit test, and a
    // short history of tip positions for measuring swing speed.
    bool g_have_prev_blade = false;
    bool g_prev_blade_drawn = false;  // which source it came from
    Vec3 g_prev_hilt = {}, g_prev_tip = {};
    struct TipSample {
        float time;
        Vec3 tip;
    };
    TipSample g_tip_history[16] = {};
    int g_tip_count = 0;
    float g_clock = 0.0f;
    HitCooldown g_cooldowns[32] = {};
    bool g_trigger_down[2] = {};
    bool g_building = false;
    LARGE_INTEGER g_last_time = {};
    unsigned g_hits = 0;

    float FrameSeconds() {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const float dt = g_last_time.QuadPart == 0
                             ? 0.0f
                             : static_cast<float>(now.QuadPart - g_last_time.QuadPart) / frequency.QuadPart;
        g_last_time = now;
        return std::min(dt, 0.1f);
    }

    void *Player() {
        __try {
            return *GameAddress<void *>(kPlayers);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    bool IsSabreUser(const void *player) {
        __try {
            const auto *character_data = Field<const unsigned char *>(player, kObjCharacterData);
            const auto *game_character =
                character_data != nullptr ? Field<const unsigned char *>(character_data, kCharDataGameCharacter) : nullptr;
            return game_character != nullptr && Field<signed char>(game_character, kGameCharStreakJoints) >= 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // ---- Holster -------------------------------------------------------------

    // Hip position (tracking space) on one side, estimated from the head: below
    // it, to the side of where the head faces (yaw only).
    Vec3 HipPosition(const VrHeadState &head, int side) {
        Vec3 forward = Rotate(head.pose.orientation, {0.0f, 0.0f, -1.0f});
        forward.y = 0.0f;
        const float length = std::max(Length(forward), 1e-4f);
        forward = forward * (1.0f / length);
        const Vec3 right = {-forward.z, 0.0f, forward.x};
        return PosOf(head.pose) + Vec3{0.0f, -kHipDrop, 0.0f} + right * (side == 0 ? -kHipSide : kHipSide);
    }

    void UpdateHolster(void *player, const VrTrackingState &tracking, float dt) {
        for (int side = 0; side < 2; ++side) {
            const VrHand &hand = tracking.hands[side];
            const bool was_down = g_trigger_down[side];
            g_trigger_down[side] = was_down ? hand.trigger > kTriggerRelease : hand.trigger > kTriggerPress;
            if (!hand.tracked || was_down || !g_trigger_down[side]) {
                continue;
            }
            if (Length(PosOf(hand.grip) - HipPosition(tracking.head, side)) > kHipRadius) {
                continue;
            }
            if (g_saber_hand < 0) {
                g_saber_hand = side;
                g_saber_wanted = side;
                g_saber_last = side;
                g_have_prev_blade = false;
                g_fast_weapon_out(player, 1);
                Log("interaction: saber drawn into the %s hand", side == 0 ? "left" : "right");
            } else if (g_saber_hand == side) {
                g_saber_hand = -1;
                g_saber_wanted = -1;
                g_holstering = true;
                g_fast_weapon_in(player, 1);
                Log("interaction: saber holstered");
            }
        }
        // Back in play after a death or respawn with the saber put away by the
        // game: draw it again into the hand it was in.
        if (g_saber_hand < 0 && g_saber_wanted >= 0 && g_in_play_seconds > 1.0f && IsSabreUser(player)) {
            g_saber_hand = g_saber_wanted;
            g_have_prev_blade = false;
            g_fast_weapon_out(player, 1);
            Log("interaction: saber drawn again into the %s hand (respawn)", g_saber_hand == 0 ? "left" : "right");
        }
        // Keep it out: the game otherwise retracts it when idle, and after a
        // death or respawn it may leave it half out (drawn but harmless). A
        // retract or a stop part way is re-extended (at most twice a second,
        // so an extend in progress is never restarted); if it is still not
        // fully out after kSaberStuckSeconds it is forced out.
        // Weapon states: 0 idle, 1 extending, 2 retracting.
        if (g_saber_hand >= 0) {
            __try {
                const unsigned char state = Field<unsigned char>(player, kObjWeaponState);
                const float scale = Field<float>(player, kObjWeaponScale);
                if (scale >= 1.0f) {
                    g_saber_stuck = 0.0f;
                    if (state == 0) {
                        g_keep_weapon_out(player);
                    }
                } else {
                    // Only in plain walking/standing (contexts -1, 1): climbing,
                    // building and the like put it away on purpose.
                    const signed char context = Field<signed char>(player, kObjContext);
                    g_saber_stuck = context == -1 || context == 1 ? g_saber_stuck + dt : 0.0f;
                    if (g_saber_stuck > kSaberStuckSeconds) {
                        Log("interaction: saber stuck part way out (scale %.2f, state %d, context %d); forced out",
                            scale, state, Field<signed char>(player, kObjContext));
                        g_keep_weapon_out(player);
                        g_saber_stuck = 0.0f;
                    } else if (state != 1 && g_weapon_out_cooldown <= 0.0f) {
                        g_fast_weapon_out(player, 0);
                        g_weapon_out_cooldown = 0.5f;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        } else {
            g_saber_stuck = 0.0f;
        }
    }

    // ---- Saber hits ----------------------------------------------------------

    bool OnCooldown(const void *target) {
        for (const HitCooldown &c : g_cooldowns) {
            if (c.target == target && c.remaining > 0.0f) {
                return true;
            }
        }
        return false;
    }

    void StartCooldown(const void *target) {
        HitCooldown *slot = &g_cooldowns[0];
        for (HitCooldown &c : g_cooldowns) {
            if (c.remaining <= 0.0f) {
                slot = &c;
                break;
            }
            if (c.remaining < slot->remaining) {
                slot = &c;
            }
        }
        *slot = {target, kHitCooldown};
    }

    // The target's collision volume as an ellipsoid around collision_position,
    // grown by the blade radius.
    bool Touches(const void *target, const Vec3 *points, int count, float blade_radius) {
        const Vec3 centre = FieldVec(target, kObjCollisionPos);
        const float radius = Field<float>(target, kObjRadius) + blade_radius;
        const float height = std::max(Field<float>(target, kObjHeight), Field<float>(target, kObjRadius)) + blade_radius;
        if (radius <= 0.0f || height <= 0.0f) {
            return false;
        }
        for (int i = 0; i < count; ++i) {
            const Vec3 d = points[i] - centre;
            if ((d.x * d.x + d.z * d.z) / (radius * radius) + (d.y * d.y) / (height * height) <= 1.0f) {
                return true;
            }
        }
        return false;
    }

    void HitTargets(void *player, const Vec3 *points, int count, float blade_radius, int damage) {
        __try {
            auto *objects = *GameAddress<unsigned char *>(kObjArray);
            const int high = *GameAddress<int>(kObjHigh);
            if (objects == nullptr || high <= 0 || high > 4096) {
                return;
            }
            const int flags = g_obj_hit_obj_flags(player) | 0x100;
            for (int i = 0; i < high; ++i) {
                unsigned char *target = objects + i * kObjStride;
                const unsigned object_flags = Field<unsigned>(target, kObjFlags);
                if (target == player || (object_flags & 0x1001) != 0x1001 || (object_flags & 0x80) != 0 ||
                    Field<unsigned char>(target, kObjDead) != 0 || OnCooldown(target)) {
                    continue;
                }
                if (!Touches(target, points, count, blade_radius)) {
                    continue;
                }
                const int result = g_obj_hit_obj(player, target, damage, flags, 0, 1);
                StartCooldown(target);
                ++g_hits;
                Log("interaction: saber hit object %d (damage %d) -> %d, hp now %d", i, damage, result,
                    Field<signed char>(target, kObjHp));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("interaction: saber hit test faulted");
        }
    }

    // Breakable props (crates, pipes, furniture): the game's own sabre test.
    void HitBreakables(void *player, Vec3 hilt, Vec3 tip) {
        if (g_blowup_hit == nullptr) {
            return;
        }
        const float length = Length(tip - hilt);
        if (length <= 1e-4f) {
            return;
        }
        const Vec3 middle = Lerp(hilt, tip, 0.5f);
        float points[9] = {hilt.x, hilt.y, hilt.z, tip.x, tip.y, tip.z, middle.x, middle.y, middle.z};
        const float extent = length * 1.5f;
        float minimum[3] = {middle.x - extent, middle.y - extent, middle.z - extent};
        float maximum[3] = {middle.x + extent, middle.y + extent, middle.z + extent};
        __try {
            if (g_blowup_hit(player, points, 3, length * 0.25f, minimum, maximum, nullptr, 0, nullptr) != nullptr) {
                ++g_hits;
                Log("interaction: saber hit a breakable");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("interaction: breakable hit test faulted; disabled");
            g_blowup_hit = nullptr;
        }
    }

    void ResetBlade() {
        g_have_prev_blade = false;
        g_tip_count = 0;
    }

    // Swing speed (real m/s) of the blade tip over at least 20 ms, so pose
    // updates arriving at a different rate than game frames don't inflate it.
    float TipSpeed(Vec3 tip_world) {
        if (g_tip_count == 16) {
            memmove(g_tip_history, g_tip_history + 1, 15 * sizeof(TipSample));
            --g_tip_count;
        }
        g_tip_history[g_tip_count++] = {g_clock, tip_world};
        for (int i = g_tip_count - 2; i >= 0; --i) {
            const float span = g_clock - g_tip_history[i].time;
            if (span >= 0.02f) {
                return Length(tip_world - g_tip_history[i].tip) / span / std::max(CameraWorldScale(), 0.01f);
            }
        }
        return 0.0f;
    }

    // Closest points of segments p0-p1 and q0-q1: returns the squared
    // distance, *on_q the point on the second segment.
    float SegmentDistance2(Vec3 p0, Vec3 p1, Vec3 q0, Vec3 q1, Vec3 *on_q) {
        const Vec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
        const float a = Dot(d1, d1), e = Dot(d2, d2), f = Dot(d2, r);
        float sp = 0.0f, tq = 0.0f;
        if (a <= 1e-8f && e <= 1e-8f) {
            sp = tq = 0.0f;
        } else if (a <= 1e-8f) {
            tq = std::clamp(f / e, 0.0f, 1.0f);
        } else {
            const float c = Dot(d1, r);
            if (e <= 1e-8f) {
                sp = std::clamp(-c / a, 0.0f, 1.0f);
            } else {
                const float b = Dot(d1, d2), denom = a * e - b * b;
                sp = denom > 1e-8f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
                tq = (b * sp + f) / e;
                if (tq < 0.0f) {
                    tq = 0.0f;
                    sp = std::clamp(-c / a, 0.0f, 1.0f);
                } else if (tq > 1.0f) {
                    tq = 1.0f;
                    sp = std::clamp((b - c) / a, 0.0f, 1.0f);
                }
            }
        }
        const Vec3 cp = p0 + d1 * sp;
        *on_q = q0 + d2 * tq;
        const Vec3 gap = cp - *on_q;
        return Dot(gap, gap);
    }

    bool IsAlive(const void *object) {
        return object != nullptr && (Field<unsigned>(object, kObjFlags) & 0x1001) == 0x1001 &&
               Field<unsigned char>(object, kObjDead) == 0;
    }

    // Sends a bolt back: from the blade, at whoever fired it (or glancing off
    // the blade if they are gone), now the player's, so it hurts enemies.
    void ReflectBolt(void *player, unsigned char *bolt, Vec3 contact, Vec3 blade_axis, float clearance) {
        const Vec3 velocity = FieldVec(bolt, kBoltVelocity);
        const float speed = Length(velocity);
        const Vec3 incoming = Unit(velocity);
        Vec3 direction = {};
        const void *shooter = Field<const void *>(bolt, kBoltOwner);
        if (IsAlive(shooter)) {
            direction = Unit(FieldVec(shooter, kObjCollisionPos) - contact);
        }
        if (Length(direction) < 0.5f) {
            // Mirror the incoming direction about the blade, then away from it.
            direction = Unit(blade_axis * (2.0f * Dot(incoming, blade_axis)) - incoming);
            if (Length(direction) < 0.5f) {
                direction = incoming * -1.0f;
            }
        }
        // The shot matrix (rows right/up/forward), and the effect matrix kept
        // in the same relation to it as the game set up: effect = R * shot.
        const float *old_shot = reinterpret_cast<const float *>(bolt + kBoltMatrix);
        const float *old_effect = reinterpret_cast<const float *>(bolt + kBoltEffectMatrix);
        float relation[9] = {};  // R = effect * shot^T (shot is orthonormal)
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                for (int k = 0; k < 3; ++k) {
                    relation[i * 3 + j] += old_effect[i * 4 + k] * old_shot[j * 4 + k];
                }
            }
        }
        Vec3 right = Unit({direction.z, 0.0f, -direction.x});
        if (Length(right) < 0.5f) {
            right = {1.0f, 0.0f, 0.0f};
        }
        const Vec3 up = Cross(direction, right);
        const Vec3 rows[3] = {right, up, direction};
        float shot[16], effect[16];
        memcpy(shot, old_shot, sizeof(shot));
        memcpy(effect, old_effect, sizeof(effect));
        for (int i = 0; i < 3; ++i) {
            shot[i * 4 + 0] = rows[i].x, shot[i * 4 + 1] = rows[i].y, shot[i * 4 + 2] = rows[i].z;
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                effect[i * 4 + j] = relation[i * 3 + 0] * shot[0 * 4 + j] + relation[i * 3 + 1] * shot[1 * 4 + j] +
                                    relation[i * 3 + 2] * shot[2 * 4 + j];
            }
        }
        memcpy(bolt + kBoltMatrix, shot, sizeof(shot));
        memcpy(bolt + kBoltEffectMatrix, effect, sizeof(effect));
        const Vec3 start = contact + direction * clearance;
        const Vec3 new_velocity = direction * speed;
        const float position[3] = {start.x, start.y, start.z};
        const float vel[3] = {new_velocity.x, new_velocity.y, new_velocity.z};
        const float dir[3] = {direction.x, direction.y, direction.z};
        memcpy(bolt + kBoltPosition, position, sizeof(position));
        memcpy(bolt + kBoltPrevious, position, sizeof(position));
        memcpy(bolt + kBoltVelocity, vel, sizeof(vel));
        memcpy(bolt + kBoltDirection, dir, sizeof(dir));
        *reinterpret_cast<void **>(bolt + kBoltOwner) = player;
        *reinterpret_cast<float *>(bolt + kBoltTime) = 0.0f;
        *reinterpret_cast<unsigned *>(bolt + kBoltFlags) |= kBoltReflected;
        *reinterpret_cast<unsigned short *>(bolt + kBoltHitFlags) =
            static_cast<unsigned short>(g_obj_hit_obj_flags(player) | 0x80);
    }

    // Blocks blaster bolts with the blade: any enemy bolt whose path this
    // frame (and a little ahead, since this runs after the game's update)
    // passes the blade, or the area it swept since last frame, is sent back.
    // Holding the saber in the way is enough; no swing needed.
    void DeflectBolts(void *player, Vec3 hilt, Vec3 tip, float blade_radius, float dt) {
        __try {
            unsigned char *bolts = GameAddress<unsigned char>(kBolts);
            for (int i = 0; i < kBoltCount; ++i) {
                unsigned char *bolt = bolts + i * kBoltStride;
                if (Field<unsigned char>(bolt, kBoltActive) == 0 || Field<const void *>(bolt, 0) == nullptr) {
                    continue;
                }
                const void *owner = Field<const void *>(bolt, kBoltOwner);
                if (owner == player) {
                    continue;
                }
                const Vec3 position = FieldVec(bolt, kBoltPosition);
                const Vec3 velocity = FieldVec(bolt, kBoltVelocity);
                Vec3 from = FieldVec(bolt, kBoltPrevious);
                if (Length(position - from) > Length(velocity) * 0.2f + 1.0f) {
                    from = position;  // stale previous position
                }
                const Vec3 to = position + velocity * (std::max(dt, 1.0f / 60.0f) * 1.5f);
                const float reach = std::clamp(Field<float>(bolt, kBoltRadius), 0.0f, 0.3f) + blade_radius +
                                    kDeflectMargin;
                // The blade now, and where it was swept through since last frame.
                Vec3 best_contact = {};
                float best = reach * reach;
                bool hit = false;
                const int samples = g_have_prev_blade ? 3 : 1;
                for (int k = 0; k < samples; ++k) {
                    const float t = samples == 1 ? 1.0f : k / 2.0f;
                    const Vec3 a = Lerp(g_prev_hilt, hilt, t), b = Lerp(g_prev_tip, tip, t);
                    Vec3 contact;
                    const float d2 = samples == 1 ? SegmentDistance2(from, to, hilt, tip, &contact)
                                                  : SegmentDistance2(from, to, a, b, &contact);
                    if (d2 <= best) {
                        best = d2;
                        best_contact = contact;
                        hit = true;
                    }
                }
                if (!hit) {
                    continue;
                }
                ReflectBolt(player, bolt, best_contact, Unit(tip - hilt), reach);
                ++g_deflects;
                Log("interaction: deflected bolt %d (%u so far)", i, g_deflects);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("interaction: bolt deflection faulted");
        }
    }

    void UpdateSaber(void *player, const VrTrackingState &tracking, float dt) {
        for (HitCooldown &c : g_cooldowns) {
            c.remaining -= dt;
        }
        // The game can bring the saber out by itself (respawn, character
        // switch, cutscene end). If it is out, it is in play: swings hit
        // without the hip gesture having been used.
        if (g_saber_hand < 0 && IsSabreUser(player)) {
            float scale = 0.0f;
            __try {
                scale = Field<float>(player, kObjWeaponScale);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            if (scale < 1.0f) {
                g_holstering = false;
            } else if (!g_holstering) {
                g_saber_hand = g_saber_wanted >= 0 ? g_saber_wanted : g_saber_last;
                g_saber_last = g_saber_hand;
                g_have_prev_blade = false;
                Log("interaction: saber is out (game drew it); swings hit with the %s hand",
                    g_saber_hand == 0 ? "left" : "right");
            }
        }
        if (g_saber_hand < 0 || !IsSabreUser(player)) {
            ResetBlade();
            return;
        }
        float weapon_scale = 0.0f, blade_radius = 0.0f;
        __try {
            weapon_scale = Field<float>(player, kObjWeaponScale);
            blade_radius = Field<float>(player, kObjSabreRadius);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        if (weapon_scale < 0.9f || dt <= 0.0f || dt >= 0.1f) {
            ResetBlade();  // not out, or a hitch: don't sweep across the gap
            return;
        }
        // Hit with the blade as drawn this frame (arm IK puts it in the
        // controller's hand). Without it, extend a blade from the drawing
        // hand's fist along the grip's forward axis.
        Vec3 hilt, tip;
        float drawn_hilt[3], drawn_tip[3];
        const bool drawn = SkeletonBladeWorld(drawn_hilt, drawn_tip);
        if (drawn) {
            hilt = {drawn_hilt[0], drawn_hilt[1], drawn_hilt[2]};
            tip = {drawn_tip[0], drawn_tip[1], drawn_tip[2]};
        } else {
            const VrHand &hand = tracking.hands[g_saber_hand];
            float hilt_world[3], axes[9];
            if (!hand.tracked || !CameraTrackingToWorld(hand.grip, hilt_world, axes)) {
                ResetBlade();
                return;
            }
            const float length = blade_radius > 0.0f ? blade_radius * 4.0f : kDefaultBladeLength;
            hilt = {hilt_world[0], hilt_world[1], hilt_world[2]};
            tip = hilt + Vec3{axes[6], axes[7], axes[8]} * length;
        }
        // A change of source or a jump (teleport, respawn) is not a swing.
        if (g_have_prev_blade && (drawn != g_prev_blade_drawn || Length(hilt - g_prev_hilt) > 1.0f)) {
            ResetBlade();
        }
        const float speed = TipSpeed(tip);
        // Diagnostics: fastest swing in each second, with the blade's length.
        static float max_speed = 0.0f, max_clock = 0.0f;
        max_speed = std::max(max_speed, speed);
        if (g_clock - max_clock >= 1.0f) {
            if (max_speed >= kSwingSpeed) {
                Log("interaction: swing %.1f m/s (blade %.2f units, %s)", max_speed, Length(tip - hilt),
                    drawn ? "as drawn" : "from the controller");
            }
            max_speed = 0.0f;
            max_clock = g_clock;
        }

        if (g_have_prev_blade) {
            if (speed >= kSwingSpeed) {
                // Sample the swept area between last frame's blade and this one.
                Vec3 points[12];
                int count = 0;
                for (float s : {0.0f, 0.5f, 1.0f}) {
                    const Vec3 a = Lerp(g_prev_hilt, hilt, s);
                    const Vec3 b = Lerp(g_prev_tip, tip, s);
                    for (float t : {0.3f, 0.55f, 0.8f, 1.0f}) {
                        points[count++] = Lerp(a, b, t);
                    }
                }
                HitTargets(player, points, count, std::max(blade_radius, 0.05f), speed >= kHardSwingSpeed ? 2 : 1);
                // Props along the swing: the blade now and halfway through it.
                HitBreakables(player, hilt, tip);
                HitBreakables(player, Lerp(g_prev_hilt, hilt, 0.5f), Lerp(g_prev_tip, tip, 0.5f));
            }
        }
        DeflectBolts(player, hilt, tip, std::max(blade_radius, 0.05f), dt);
        g_prev_hilt = hilt;
        g_prev_tip = tip;
        g_prev_blade_drawn = drawn;
        g_have_prev_blade = true;
    }

    // ---- Building ------------------------------------------------------------

    // ---- Guns ----------------------------------------------------------------

    // The player's shots go where the gun-hand controller aims. Everything else
    // (enemies, other players, turrets) keeps the default.
    int __cdecl HookShootDirection(void *object, float *direction) {
        if (!g_aim_valid || object == nullptr || object != Player()) {
            return g_real_shoot_direction(object, direction);
        }
        if (direction != nullptr) {
            direction[0] = g_aim[0];
            direction[1] = g_aim[1];
            direction[2] = g_aim[2];
        }
        const float heading = std::atan2(g_aim[0], g_aim[2]);  // game yaw: forward = (sin, 0, cos)
        return static_cast<unsigned short>(static_cast<int>(heading * (65536.0f / (2.0f * 3.14159265f))));
    }

    // The player's bolts leave the muzzle exactly along the aim ray: the shot
    // matrix's rotation is rebuilt from it (forward = aim, right level), so the
    // game's target-seeking angles are discarded. No aim assist.
    void *__cdecl HookBoltAdd(void *owner, float *position, float *matrix, int type, int flags) {
        if (!g_aim_valid || matrix == nullptr || owner == nullptr || owner != Player()) {
            return g_real_bolt_add(owner, position, matrix, type, flags);
        }
        const Vec3 forward = {g_aim[0], g_aim[1], g_aim[2]};
        Vec3 right = {forward.z, 0.0f, -forward.x};  // level, to the right of the aim
        const float right_length = Length(right);
        right = right_length > 1e-4f ? right * (1.0f / right_length) : Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 up = {forward.y * right.z - forward.z * right.y, forward.z * right.x - forward.x * right.z,
                         forward.x * right.y - forward.y * right.x};
        float aimed[16];
        memcpy(aimed, matrix, sizeof(aimed));
        const Vec3 rows[3] = {right, up, forward};
        for (int row = 0; row < 3; ++row) {
            aimed[row * 4 + 0] = rows[row].x;
            aimed[row * 4 + 1] = rows[row].y;
            aimed[row * 4 + 2] = rows[row].z;
            aimed[row * 4 + 3] = 0.0f;
        }
        return g_real_bolt_add(owner, position, aimed, type, flags);
    }

    // Every shooter's gun is in the right hand (the VR hands put it there).
    // It aims along that controller's aim ray, i.e. the drawn barrel, or, with
    // gun_aim=head in lsw_vr_settings.txt, straight where the head looks. The
    // right trigger (away from the hip) fires.
    void UpdateGun(void *player, const VrTrackingState &tracking) {
        g_aim_valid = false;
        constexpr int side = 1;
        if (IsSabreUser(player)) {
            g_firing = false;
            return;
        }
        const VrHand &hand = tracking.hands[side];
        float position[3], axes[9];
        const bool from_head = CameraGunAimFromHead();
        if (!hand.tracked ||
            !CameraTrackingToWorld(from_head ? tracking.head.pose : hand.aim, position, axes)) {
            g_firing = false;
            return;
        }
        const Vec3 aim = {axes[6], axes[7], axes[8]};
        const float length = Length(aim);
        if (length < 1e-4f) {
            return;
        }
        g_aim[0] = aim.x / length, g_aim[1] = aim.y / length, g_aim[2] = aim.z / length;
        g_aim_valid = true;
        const bool at_hip = Length(PosOf(hand.grip) - HipPosition(tracking.head, side)) <= kHipRadius;
        const bool firing = !at_hip && hand.trigger > (g_firing ? kTriggerRelease : kTriggerPress);
        if (firing != g_firing) {
            g_firing = firing;
        }
        if (g_firing) {
            VrControlsHold(VrControlsActionMask());
        }
    }

    // ---- Building ------------------------------------------------------------

    bool HandAtPile(const Vec3 &hand) {
        __try {
            const auto *world = *GameAddress<const unsigned char *const>(kWorldPtr);
            const auto *system = world != nullptr ? Field<const unsigned char *>(world, kWorldBuildIts) : nullptr;
            if (system == nullptr) {
                return false;
            }
            const auto *piles = Field<const unsigned char *>(system, 0);
            const int count = Field<unsigned short>(system, 4);
            for (int i = 0; piles != nullptr && i < count; ++i) {
                const unsigned char *pile = piles + i * kBuildItStride;
                if ((Field<unsigned char>(pile, kBuildItAvailability) & 3) != 3 ||
                    Field<unsigned char>(pile, kBuildItState) != 0) {
                    continue;
                }
                const Vec3 start = FieldVec(pile, kBuildItStart);
                const float reach = Field<float>(pile, kBuildItRadius) + kPileMargin;
                const float dx = hand.x - start.x, dz = hand.z - start.z;
                if (dx * dx + dz * dz <= reach * reach && hand.y >= start.y - kPileMargin &&
                    hand.y <= start.y + reach + kPileMargin) {
                    return true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return false;
    }

    bool PlayerIsBuilding(const void *player) {
        __try {
            return Field<unsigned char>(player, kObjContext) ==
                       static_cast<unsigned char>(*GameAddress<const int>(kContextBuildIt)) &&
                   Field<const void *>(player, kObjBuildIt) != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void UpdateBuilding(void *player, const VrTrackingState &tracking) {
        bool gripping = false;
        bool at_pile = false;
        for (const VrHand &hand : tracking.hands) {
            if (!hand.tracked || hand.squeeze < kSqueezePress) {
                continue;
            }
            gripping = true;
            float position[3], axes[9];
            if (CameraTrackingToWorld(hand.grip, position, axes) && HandAtPile({position[0], position[1], position[2]})) {
                at_pile = true;
            }
        }
        // Grip at a pile starts building; keep gripping to keep building.
        const bool hold = gripping && (at_pile || PlayerIsBuilding(player));
        if (hold != g_building) {
            Log("interaction: %s building", hold ? "started" : "stopped");
            g_building = hold;
        }
        if (hold) {
            VrControlsHold(VrControlsSpecialMask());
        }
    }
} // namespace

void InstallVrInteraction() {
    g_obj_hit_obj = GameFunction<ObjHitObjFn>("ObjHitObj", 0x438230, kObjHitObjPrologue);
    g_obj_hit_obj_flags = GameFunction<ObjHitObjFlagsFn>("ObjHitObj_Flags", 0x58b720, kObjHitObjFlagsPrologue);
    g_blowup_hit = GameFunction<BlowUpHitFn>("GizmoBlowUp_Hit", 0x569c70, kBlowUpHitPrologue);
    g_fast_weapon_out = GameFunction<WeaponFn>("FastWeaponOut", 0x58d480, kFastWeaponPrologue);
    g_fast_weapon_in = GameFunction<WeaponFn>("FastWeaponIn", 0x58d930, kFastWeaponPrologue);
    g_keep_weapon_out = GameFunction<KeepWeaponOutFn>("KeepWeaponOut", 0x58d8b0, kKeepWeaponOutPrologue);
    g_installed = g_obj_hit_obj && g_obj_hit_obj_flags && g_fast_weapon_out && g_fast_weapon_in && g_keep_weapon_out;
    if (g_installed &&
        HookGameFunction("Bolt_GetShootDirection_Default", 0x580950, kShootDirectionPrologue, sizeof(kShootDirectionPrologue),
                         reinterpret_cast<void *>(&HookShootDirection), reinterpret_cast<void **>(&g_real_shoot_direction))) {
        if (HookGameFunction("Bolt_Add", 0x582e70, kBoltAddPrologue, sizeof(kBoltAddPrologue),
                             reinterpret_cast<void *>(&HookBoltAdd), reinterpret_cast<void **>(&g_real_bolt_add))) {
            Log("interaction: guns are in the right hand and fire exactly along its aim (no aim assist); right trigger "
                "fires");
        }
    }
    Log("interaction: %s (trigger at a hip draws/holsters the saber, swing to hit, the blade blocks and returns "
        "blaster bolts, grip at a pile to build)",
        g_installed ? "ready" : "disabled, game functions not found");
}

void VrInteractionOnPresent(unsigned frame) {
    const float dt = FrameSeconds();
    g_clock += dt;
    g_weapon_out_cooldown -= dt;
    if (!g_installed) {
        return;
    }
    const VrTrackingState *tracking = VrTracking();
    void *player = Player();
    if (player != g_player) {
        // New character (switch, respawn, level load): start clean.
        g_player = player;
        g_saber_hand = -1;
        ResetBlade();
        g_building = false;
    }
    // Game functions only on the game's own thread, and only while the player
    // is actually in play (drawn this frame): not during loads or menus.
    const bool in_play = OnGameThread() && SkeletonPlayerDrawn() && !CameraMenuOpen();
    static bool logged_thread = false;
    if (tracking != nullptr && player != nullptr && !OnGameThread() && !logged_thread) {
        Log("interaction: Present runs on thread %lu, not the game's; saber/build gestures are off",
            GetCurrentThreadId());
        logged_thread = true;
    }
    g_frames_not_drawn = in_play ? 0 : g_frames_not_drawn + 1;
    g_in_play_seconds = in_play ? g_in_play_seconds + dt : 0.0f;
    if (g_frames_not_drawn > 30 && g_saber_hand >= 0) {
        g_saber_wanted = g_saber_hand;  // out again after the respawn
        g_saber_hand = -1;  // the game will have put it away
        ResetBlade();
    }
    if (tracking == nullptr || player == nullptr || !in_play) {
        g_aim_valid = false;
        g_firing = false;
        return;
    }
    UpdateHolster(player, *tracking, dt);
    if (g_real_shoot_direction != nullptr) {
        UpdateGun(player, *tracking);
    }
    UpdateSaber(player, *tracking, dt);
    UpdateBuilding(player, *tracking);

    if (frame % kLogEvery == 0) {
        float right[3] = {}, axes[9];
        const bool mapped = CameraTrackingToWorld(tracking->hands[1].grip, right, axes);
        Log("interaction: scale %.2f, saber %s, %u hits so far, right hand world (%.2f %.2f %.2f)%s",
            CameraWorldScale(), g_saber_hand < 0 ? "holstered" : (g_saber_hand == 0 ? "left" : "right"), g_hits,
            right[0], right[1], right[2], mapped ? "" : " (unmapped)");
    }
}

int VrInteractionSaberHand() {
    return g_saber_hand >= 0 ? g_saber_hand : g_saber_last;
}

bool VrInteractionHandAtHip(const VrTrackingState &tracking, int side) {
    return tracking.hands[side].tracked &&
           Length(PosOf(tracking.hands[side].grip) - HipPosition(tracking.head, side)) <= kHipRadius;
}
