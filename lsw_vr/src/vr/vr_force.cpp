// Point-to-target Force: whatever the LEFT hand points at becomes the Force
// target, if the game lets the Force act on it. X (SPECIAL), or the left
// trigger away from the hip, then uses the Force as usual.
//
// The game picks Force targets itself every frame, for its glow highlight and
// for when SPECIAL is pressed. Both pickers are hooked; each runs the game's
// own selection (so every rule about what can be used stays the game's), but
// steered toward the pointing ray:
//
// Force objects (GizForce, the glowing LEGO props), 00469820
//   GizForce_FindBestForceTarget(force_sys = WORLD+0x46ac, object), cdecl. It
//   lists every usable force within its interaction radius and in front of
//   the character's movement_facing_angle (+0x5a) into a static array @00877ae0
//   (0x10 each: force, animation object or 0, distance^2, LOS index), then
//   takes the nearest one whose line-of-sight bit (object+0x800 -> bit index)
//   is set, writing object+0x10cc gizforce_target, +0x10d0 its animation
//   object. Hooked: the facing is turned to the pointing direction for the
//   call (so "in front" means "where the hand points"), the array is cleared
//   beforehand so its filled length can be read back, and from the listed
//   candidates the one nearest the ray (within kConeDegrees, line of sight
//   as the game requires) replaces the game's pick. Pointing at nothing listed
//   leaves the game's own choice.
//   GIZFORCE: +0x1c position. Animation object: +0x04 special (draw position
//   via NuSpecialGetDrawPos 005cf9e0).
//
// Characters (Force push / choke / mind trick), 004a3270
//   FindForcePushTarget(object, activate, filter), cdecl: the nearest live
//   character within 1.25 units (distance^2 limit 1.5625 read from @0075be28)
//   in front of facing_direction (object+0xf30 x, +0xf38 z). Hooked: if the ray
//   points at a live character within kPushRange, for the call the facing
//   looks at it, the limit admits it, and nearer characters are skipped
//   (model_draw_result +0x288 = 0, which the picker requires), so the game's
//   own checks decide whether the Force works on it. If they reject it, the
//   picker runs again unchanged. Object fields as elsewhere: flags +0x1fc
//   (&0x1001 live), dead +0x28b, collision position +0x80.

#include "vr_force.h"

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
#include "vr_interaction.h"

namespace {
    constexpr uintptr_t kPlayers = 0x93d7f0;
    constexpr uintptr_t kObjArray = 0x93d504;
    constexpr uintptr_t kObjHigh = 0x93d500;
    constexpr uintptr_t kObjStride = 0x10d8;

    constexpr uintptr_t kObjFacing = 0x58;
    constexpr uintptr_t kObjMoveFacing = 0x5a;
    constexpr uintptr_t kObjPosition = 0x5c;
    constexpr uintptr_t kObjContext = 0x7b5;  // character_context (s8), 8 = using the Force on a prop
    constexpr uintptr_t kObjCollisionPos = 0x80;
    constexpr uintptr_t kObjFlags = 0x1fc;
    constexpr uintptr_t kObjDrawn = 0x288;
    constexpr uintptr_t kObjDead = 0x28b;
    constexpr uintptr_t kObjLos = 0x800;
    constexpr uintptr_t kObjFacingX = 0xf30;
    constexpr uintptr_t kObjFacingZ = 0xf38;
    constexpr uintptr_t kObjGizTarget = 0x10cc;
    constexpr uintptr_t kObjGizTargetObject = 0x10d0;

    constexpr uintptr_t kForceCandidates = 0x877ae0;
    constexpr int kMaxForceCandidates = 384;
    constexpr uintptr_t kForcePosition = 0x1c;
    constexpr uintptr_t kAnimObjectSpecial = 0x04;
    constexpr uintptr_t kPushRangeSquared = 0x75be28;

    // sub esp,0x40; fld [00751644]; push ebx
    constexpr unsigned char kFindBestPrologue[] = {0x83, 0xec, 0x40, 0xd9, 0x05, 0x44, 0x16, 0x75, 0x00, 0x53};
    // sub esp,0x34; push ebp; mov ebp,[esp+0x3c]
    constexpr unsigned char kPushTargetPrologue[] = {0x83, 0xec, 0x34, 0x55, 0x8b, 0x6c, 0x24, 0x3c};
    // mov ecx,[esp+4]; mov eax,[ecx+8]
    constexpr unsigned char kDrawPosPrologue[] = {0x8b, 0x4c, 0x24, 0x04, 0x8b, 0x41, 0x08};

    constexpr float kPi = 3.14159265f;
    // How far off the pointing ray a target may be (degrees, measured from the
    // hand), so pointing needn't be exact.
    constexpr float kConeDegrees = 20.0f;
    constexpr float kPushRange = 3.0f;  // game units, for characters
    constexpr float kTriggerPress = 0.7f;
    constexpr float kTriggerRelease = 0.4f;

    using FindBestFn = int(__cdecl *)(void *force_sys, unsigned char *object);
    using PushTargetFn = void(__cdecl *)(unsigned char *object, int activate, int filter);
    using DrawPosFn = const float *(__cdecl *)(const void *special);
    FindBestFn g_real_find_best = nullptr;
    PushTargetFn g_real_push_target = nullptr;
    DrawPosFn g_draw_pos = nullptr;

    struct Vec3 {
        float x, y, z;
    };
    Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    float Length(Vec3 v) { return std::sqrt(Dot(v, v)); }

    template <typename T> T Field(const void *base, uintptr_t offset) {
        T value;
        memcpy(&value, static_cast<const unsigned char *>(base) + offset, sizeof(T));
        return value;
    }
    template <typename T> void SetField(void *base, uintptr_t offset, T value) {
        memcpy(static_cast<unsigned char *>(base) + offset, &value, sizeof(T));
    }
    Vec3 FieldVec(const void *base, uintptr_t offset) {
        return {Field<float>(base, offset), Field<float>(base, offset + 4), Field<float>(base, offset + 8)};
    }

    // This frame's pointing ray (world), from the left hand's aim pose.
    bool g_pointing = false;
    Vec3 g_ray_origin = {};
    Vec3 g_ray_dir = {};
    float g_cos_cone = 0.0f;
    bool g_trigger_down = false;
    // Diagnostics: what each picker chose, to log changes.
    const void *g_last_object_pick = nullptr;
    const void *g_last_character_pick = nullptr;
    signed char g_last_context = -1;

    unsigned char *Player() {
        __try {
            return *GameAddress<unsigned char *>(kPlayers);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // How well the ray points at p: the cosine of the angle off the ray (1 =
    // dead on), or -2 if outside the cone or behind the hand.
    float Aim(Vec3 p) {
        const Vec3 to = p - g_ray_origin;
        const float distance = Length(to);
        if (distance < 1e-4f) {
            return -2.0f;
        }
        const float c = Dot(to, g_ray_dir) / distance;
        return c >= g_cos_cone ? c : -2.0f;
    }

    // ---- Force objects -------------------------------------------------------

    struct Candidate {
        void *force;
        void *animation;
        float distance2;
        int los_index;
    };
    static_assert(sizeof(Candidate) == 0x10, "GizForce candidate entry");

    bool LineOfSightOk(const unsigned char *object, int index) {
        const auto *los = Field<const unsigned *>(object, kObjLos);
        return los == nullptr || index < 0 || (los[index >> 5] & (1u << (index & 31))) != 0;
    }

    int __cdecl HookFindBest(void *force_sys, unsigned char *object) {
        NoteGameThread();
        if (!g_pointing || object == nullptr || object != Player() || g_draw_pos == nullptr) {
            return g_real_find_best(force_sys, object);
        }
        auto *candidates = GameAddress<Candidate>(kForceCandidates);
        unsigned short facing = 0;
        __try {
            // Cleared whole (AI Jedi fill it too), so the end of this call's
            // list is the first empty entry.
            memset(candidates, 0, kMaxForceCandidates * sizeof(Candidate));
            facing = Field<unsigned short>(object, kObjMoveFacing);
            // "In front" for the call = where the hand points.
            const float yaw = std::atan2(g_ray_dir.x, g_ray_dir.z);
            SetField<unsigned short>(object, kObjMoveFacing,
                                     static_cast<unsigned short>(std::lround(yaw * 65536.0f / (2.0f * kPi)) & 0xffff));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return g_real_find_best(force_sys, object);
        }
        const int result = g_real_find_best(force_sys, object);
        __try {
            SetField<unsigned short>(object, kObjMoveFacing, facing);
            const Candidate *best = nullptr;
            float best_aim = -1.0f;
            int count = 0;
            while (count < kMaxForceCandidates && candidates[count].force != nullptr) {
                const Candidate &c = candidates[count++];
                if (!LineOfSightOk(object, c.los_index)) {
                    continue;
                }
                const float *p = c.animation != nullptr
                                     ? g_draw_pos(static_cast<const unsigned char *>(c.animation) + kAnimObjectSpecial)
                                     : reinterpret_cast<const float *>(static_cast<const unsigned char *>(c.force) +
                                                                       kForcePosition);
                if (p == nullptr) {
                    continue;
                }
                const float aim = Aim({p[0], p[1], p[2]});
                if (aim > best_aim) {
                    best_aim = aim;
                    best = &c;
                }
            }
            if (best != nullptr) {
                SetField<void *>(object, kObjGizTarget, best->force);
                SetField<void *>(object, kObjGizTargetObject, best->animation);
                const void *pick = best->animation != nullptr ? best->animation : best->force;
                if (pick != g_last_object_pick) {
                    g_last_object_pick = pick;
                    Log("force: pointing at force object %p (of %d in reach)", pick, count);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("force: steering the force-object target faulted");
        }
        return result;
    }

    // ---- Characters ----------------------------------------------------------

    bool IsLive(const unsigned char *o) {
        return (Field<unsigned>(o, kObjFlags) & 0x1001) == 0x1001 && Field<unsigned char>(o, kObjDead) == 0 &&
               Field<unsigned char>(o, kObjDrawn) != 0;
    }

    // The live character the ray points at (nearest the ray within the cone
    // and kPushRange of the player), or nullptr.
    unsigned char *PointedCharacter(unsigned char *player, float *distance2) {
        auto *objects = *GameAddress<unsigned char *>(kObjArray);
        const int high = *GameAddress<int>(kObjHigh);
        if (objects == nullptr || high <= 0 || high > 4096) {
            return nullptr;
        }
        const Vec3 self = FieldVec(player, kObjCollisionPos);
        unsigned char *best = nullptr;
        float best_aim = -1.0f;
        for (int i = 0; i < high; ++i) {
            unsigned char *o = objects + i * kObjStride;
            if (o == player || !IsLive(o)) {
                continue;
            }
            const Vec3 p = FieldVec(o, kObjCollisionPos);
            const Vec3 d = p - self;
            const float d2 = Dot(d, d);
            if (d2 > kPushRange * kPushRange) {
                continue;
            }
            const float aim = Aim(p);
            if (aim > best_aim) {
                best_aim = aim;
                best = o;
                *distance2 = d2;
            }
        }
        return best;
    }

    void __cdecl HookPushTarget(unsigned char *object, int activate, int filter) {
        NoteGameThread();
        unsigned char *player = Player();
        if (!g_pointing || object == nullptr || object != player) {
            g_real_push_target(object, activate, filter);
            return;
        }
        // Up to 64 nearer characters skipped for the call.
        unsigned char *skipped[64];
        unsigned char skipped_value[64];
        int skipped_count = 0;
        float saved_facing[2] = {};
        float saved_range = 0.0f;
        float *range = GameAddress<float>(kPushRangeSquared);
        unsigned char *target = nullptr;
        __try {
            float target_d2 = 0.0f;
            target = PointedCharacter(player, &target_d2);
            if (target != nullptr) {
                auto *objects = *GameAddress<unsigned char *>(kObjArray);
                const int high = *GameAddress<int>(kObjHigh);
                const Vec3 self = FieldVec(player, kObjCollisionPos);
                for (int i = 0; i < high && skipped_count < 64; ++i) {
                    unsigned char *o = objects + i * kObjStride;
                    if (o == player || o == target || !IsLive(o)) {
                        continue;
                    }
                    const Vec3 d = FieldVec(o, kObjCollisionPos) - self;
                    if (Dot(d, d) <= target_d2) {
                        skipped[skipped_count] = o;
                        skipped_value[skipped_count++] = Field<unsigned char>(o, kObjDrawn);
                        SetField<unsigned char>(o, kObjDrawn, 0);
                    }
                }
                const Vec3 to = FieldVec(target, kObjCollisionPos) - self;
                const float flat = std::sqrt(to.x * to.x + to.z * to.z);
                saved_facing[0] = Field<float>(player, kObjFacingX);
                saved_facing[1] = Field<float>(player, kObjFacingZ);
                if (flat > 1e-4f) {
                    SetField<float>(player, kObjFacingX, to.x / flat);
                    SetField<float>(player, kObjFacingZ, to.z / flat);
                }
                saved_range = *range;
                *range = std::max(saved_range, target_d2 * 1.05f + 0.01f);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            target = nullptr;
        }
        g_real_push_target(object, activate, filter);
        if (target == nullptr) {
            return;
        }
        __try {
            for (int i = 0; i < skipped_count; ++i) {
                SetField<unsigned char>(skipped[i], kObjDrawn, skipped_value[i]);
            }
            SetField<float>(player, kObjFacingX, saved_facing[0]);
            SetField<float>(player, kObjFacingZ, saved_facing[1]);
            *range = saved_range;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("force: restoring after the character pick faulted");
        }
        if (target != g_last_character_pick) {
            g_last_character_pick = target;
            Log("force: pointing at character %p", target);
        }
    }

    // Logs when the player stops using the Force on a prop (context 8), and
    // whether the trigger was still held: if it was, the game ended it itself
    // (target done, or the character turned too far from it).
    void LogForceEnd() {
        unsigned char *player = Player();
        if (player == nullptr) {
            g_last_context = -1;
            return;
        }
        __try {
            const signed char context = Field<signed char>(player, kObjContext);
            if (g_last_context == 8 && context != 8) {
                Log("force: ended (context now %d, left trigger %s, target %p)", context,
                    g_trigger_down ? "held" : "released", Field<void *>(player, kObjGizTarget));
            }
            g_last_context = context;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_last_context = -1;
        }
    }

    // While using the Force on a prop (context 8), keeps the hidden body facing
    // the target. ForceCode ends the Force once the target is more than about
    // 0.85 off the body's facing (head_target_facing), and up close to a wall
    // a small shift of the body (room scale, the game's own nudges) swings
    // that angle a lot. The body isn't drawn in first person, so turning it
    // costs nothing.
    void FaceForceTarget() {
        unsigned char *player = Player();
        if (player == nullptr) {
            return;
        }
        __try {
            if (Field<signed char>(player, kObjContext) != 8) {
                return;
            }
            const auto *force = Field<const unsigned char *>(player, kObjGizTarget);
            if (force == nullptr) {
                return;
            }
            const Vec3 to = FieldVec(force, kForcePosition) - FieldVec(player, kObjPosition);
            if (to.x * to.x + to.z * to.z < 1e-6f) {
                return;
            }
            const auto angle = static_cast<unsigned short>(
                static_cast<int>(std::lround(std::atan2(to.x, to.z) * 65536.0f / (2.0f * kPi))) & 0xffff);
            SetField<unsigned short>(player, kObjFacing, angle);
            SetField<unsigned short>(player, kObjMoveFacing, angle);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    bool MakeWritable(void *address, size_t size) {
        DWORD old = 0;
        return VirtualProtect(address, size, PAGE_READWRITE, &old) != 0;
    }
} // namespace

void InstallVrForce() {
    g_draw_pos = GameFunction<DrawPosFn>("NuSpecialGetDrawPos", 0x5cf9e0, kDrawPosPrologue);
    const bool objects = g_draw_pos != nullptr &&
                         HookGameFunction("GizForce_FindBestForceTarget", 0x469820, kFindBestPrologue,
                                          sizeof(kFindBestPrologue), reinterpret_cast<void *>(&HookFindBest),
                                          reinterpret_cast<void **>(&g_real_find_best));
    // The push range constant lives in read-only data: make it writable once.
    const bool range_ok = std::fabs(*GameAddress<float>(kPushRangeSquared) - 1.5625f) < 1e-6f &&
                          MakeWritable(GameAddress<float>(kPushRangeSquared), sizeof(float));
    const bool characters = range_ok && HookGameFunction("FindForcePushTarget", 0x4a3270, kPushTargetPrologue,
                                                         sizeof(kPushTargetPrologue),
                                                         reinterpret_cast<void *>(&HookPushTarget),
                                                         reinterpret_cast<void **>(&g_real_push_target));
    g_cos_cone = std::cos(kConeDegrees * kPi / 180.0f);
    Log("force: point with the left hand to pick the Force target (objects %s, characters %s); X or the left "
        "trigger uses it",
        objects ? "on" : "off", characters ? "on" : "off");
}

void VrForceOnPresent(unsigned) {
    g_pointing = false;
    LogForceEnd();
    if (CameraFirstPersonRecent()) {
        FaceForceTarget();
    }
    const VrTrackingState *tracking = VrTracking();
    if (tracking == nullptr || CameraMenuOpen()) {
        g_trigger_down = false;
        return;
    }
    const VrHand &left = tracking->hands[0];
    // Left trigger away from the hip = SPECIAL (at the hip it is the saber).
    // Only the press needs first person in play; once down the button stays
    // held for as long as the trigger is, because one frame without SPECIAL
    // ends the Force (ForceCode), and the in-play checks can blink for a frame
    // (no camera set, player not drawn) mid-Force. Likewise decided before the
    // pose, so a brief tracking loss doesn't let go.
    const bool in_play = CameraFirstPersonActive() && SkeletonPlayerDrawn();
    const bool pressed = left.trigger > (g_trigger_down ? kTriggerRelease : kTriggerPress);
    g_trigger_down = pressed && (g_trigger_down || (in_play && !VrInteractionHandAtHip(*tracking, 0)));
    if (g_trigger_down) {
        VrControlsHold(VrControlsSpecialMask());
    }
    if (!in_play) {
        return;
    }
    float position[3], axes[9];
    if (!left.tracked || !CameraTrackingToWorld(left.aim, position, axes)) {
        return;
    }
    g_ray_origin = {position[0], position[1], position[2]};
    const float length = std::sqrt(axes[6] * axes[6] + axes[7] * axes[7] + axes[8] * axes[8]);
    if (length < 1e-4f) {
        return;
    }
    g_ray_dir = {axes[6] / length, axes[7] / length, axes[8] / length};
    g_pointing = true;
}
