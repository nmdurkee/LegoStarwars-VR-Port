// Player skeleton: VR first-person hands, head hiding, diagnostics.
//
// Hook point: StoreLocatorCoordinates(model, world, joints, positions,
// matrices), which APIDrawCharacterModel calls after evaluating the
// animation into `joints` and before drawing with them (opensaga). Joint
// changes made here are what gets drawn, and the game's own locators (hand
// positions that weapons and sabers attach to) follow them.
//
// VR first person is HANDS ONLY: each hand joint (with whatever hangs off it:
// gun, saber hilt, claw) sits exactly on its controller, and every other joint
// (arms, head, torso, legs) is shrunk to nothing, so the third-person
// animation never shows as stretched arms or a neck in view. The arms shrink
// into their own wrist, the rest into the character's feet. A character's gun
// always goes in the right controller and a saber in the controller that drew
// it: if the animation holds it in the other hand, the two hands swap
// controllers (the body is hidden, so nothing shows the swap).
//
// Joints are found by NAME: humanoid rigs share the names (leftShoulder,
// leftElbow, leftHand, rightShoulder, ..., head) but not the indices (shoulder
// is joint 7 on Obi-Wan, 13 on a Stormtrooper, 5 on a Clone; see
// lsw_analysis/scripts/ghg_skeleton.py --survey). Droids differ: battle
// droids end each arm at <side>Elbow (forearm and hand are one piece) and have
// "face"/"neck" rather than "head"; pit and PK droids use <side>Wrist. Each
// joint record holds an int32 at +0x4c locating its name: relative to that
// field in the model file, possibly absolute once loaded, so both are
// accepted. The config's head_joint is NOT a skeleton index (Obi-Wan: 0 = the
// root), so it is unused.
//
// PC addresses / layouts (Ghidra VA), checked against the exe:
//   0062e1e0 StoreLocatorCoordinates (cdecl); model+0x04 hierarchy,
//            model+0x14 points_of_interest[16]
//   005daae0 NuHGobjPOIMtx(hierarchy, locator, world, joints, out): locator ->
//            hierarchy+0x18c map -> hierarchy+0x184 locators (0x50 each),
//            joint index at +0x44
//   005ca550 joint walk: hierarchy+0x168 joint_count, +0x16c joints (0x60
//            each), parent index at +0x50
//   object+0x50 character_model, +0x804 locator matrices (written by
//   StoreLocatorCoordinates), +0x54 character_data -> +0x24
//   game_character; GAMECHARACTERDATA+0x100 streak (blade) locators [2],
//   +0xf8 weapon_joints[4] (gun grip), +0xfc weapon_shoot_joints[4] (muzzle)
//
// Joint matrices are joint -> model space, row-vector (rows: axes, position);
// `world` is model -> world. They are already flattened, so moving a joint
// means carrying its descendants along (SetJoint), while hiding one is a
// plain overwrite of its own matrix.

#include "skeleton.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "arm_ik.h"
#include "camera.h"
#include "game.h"
#include "log.h"
#include "mat4.h"
#include "vr/vr_frame.h"
#include "vr/vr_interaction.h"

namespace {
    constexpr uintptr_t kStoreLocatorCoordinates = 0x62e1e0;
    // sub esp,0x48; push esi; mov esi,[esp+0x5c]
    constexpr unsigned char kStoreLocatorPrologue[] = {0x83, 0xec, 0x48, 0x56, 0x8b, 0x74, 0x24, 0x5c};
    constexpr uintptr_t kPoiMtx = 0x5daae0;
    // mov ecx,[esp+4]; mov edx,[ecx+0x18c]
    constexpr unsigned char kPoiMtxPrologue[] = {0x8b, 0x4c, 0x24, 0x04, 0x8b, 0x91, 0x8c, 0x01};
    constexpr uintptr_t kPlayers = 0x93d7f0;

    constexpr uintptr_t kObjPosition = 0x5c;
    constexpr uintptr_t kObjWorldMatrix = 0xb8;
    constexpr uintptr_t kObjCharacterModel = 0x50;
    // Object flags (+0x1f8; opensaga field_0x1f4): 0x200 = draw without the
    // camera clip test (APIDrawCharacterModel 0062fbf0, and per-part clipping).
    // Set on the player in VR first person: the game culls a character by the
    // body's bounds, so a hand held out of view of the (hidden) body vanished.
    constexpr uintptr_t kObjRenderFlags = 0x1f8;
    constexpr unsigned kRenderNoClip = 0x200;
    constexpr uintptr_t kObjLocatorMatrices = 0x804;  // joint_matrices[16]: the object's locators
    constexpr uintptr_t kObjCharacterData = 0x54;
    constexpr uintptr_t kCharDataGameCharacter = 0x24;
    constexpr uintptr_t kGameCharStreakLocators = 0x100;
    constexpr uintptr_t kGameCharWeaponLocators = 0xf8;
    constexpr uintptr_t kGameCharShootLocators = 0xfc;
    constexpr uintptr_t kModelHierarchy = 0x04;
    constexpr uintptr_t kModelPointsOfInterest = 0x14;  // [16], NULL = locator unused
    constexpr uintptr_t kHierJointCount = 0x168;
    constexpr uintptr_t kHierJoints = 0x16c;
    constexpr uintptr_t kHierLocators = 0x184;
    constexpr uintptr_t kHierLocatorMap = 0x18c;
    constexpr uintptr_t kJointStride = 0x60;
    constexpr uintptr_t kJointName = 0x4c;
    constexpr uintptr_t kJointParent = 0x50;
    constexpr uintptr_t kLocatorStride = 0x50;
    constexpr uintptr_t kLocatorJoint = 0x44;

    constexpr int kMaxJoints = 128;
    constexpr int kMaxName = 32;
    constexpr float kTestPoseDegrees = 60.0f;
    constexpr float kHiddenScale = 0.001f;
    // Controller jitter filter.
    constexpr float kFilterSeconds = 0.025f;
    // Drawn frames averaged to learn how each hand is held in the animation
    // (which way the claw, gun or forearm points in the hand joint's frame).
    constexpr int kReferenceSamples = 45;

    using StoreLocatorFn = void(__cdecl *)(void *model, float *world, float *joints, void *positions, void *matrices);
    using PoiMtxFn = void(__cdecl *)(const void *hierarchy, int locator, const float *world, const float *joints,
                                     float *out);
    StoreLocatorFn g_real_store = nullptr;
    PoiMtxFn g_poi_mtx = nullptr;

    enum Side { kLeft = 0, kRight = 1 };
    // What a side's "hand" joint is: a real hand (or wrist), or a forearm that
    // ends the arm (battle droids), whose hand end is away from its origin.
    enum HandKind { kHandJoint = 0, kForearmJoint = 1 };

    // Skeleton of the player's current model, read once per model.
    struct Skeleton {
        const void *model = nullptr;
        const unsigned char *hierarchy = nullptr;
        int joint_count = 0;
        unsigned char parents[kMaxJoints] = {};
        char names[kMaxJoints][kMaxName] = {};
        int head = -1;
        int hand[2] = {-1, -1};
        int hand_kind[2] = {kHandJoint, kHandJoint};
        int blade_locator[2] = {-1, -1};  // streak locators: blade base, tip
        int blade_side = -1;              // hand the game's blade hangs off
        int gun_locator[2] = {-1, -1};    // gun grip, muzzle
        int gun_side = -1;                // hand holding the gun
    };
    Skeleton g_skeleton;
    // A model whose skeleton could not be read: don't retry it every draw.
    const void *g_failed_model = nullptr;
    const void *g_failed_hierarchy = nullptr;
    // Frame stamps: bumped at each Present, set when the player is drawn.
    unsigned g_frame_counter = 1;
    unsigned g_player_drawn_frame = 0;
    unsigned g_blade_frame = 0;
    bool g_dump_requested = false;
    bool g_test_pose = false;
    unsigned char *g_no_clip_object = nullptr;  // object we set kRenderNoClip on
    SHORT g_prev_keys[256] = {};

    // Per-controller target (world space, smoothed).
    struct HandState {
        bool has_filter = false;
        Vec3f position = {};  // grip (palm)
        Vec3f forward = {};   // grip forward: where a held blade points
        Vec3f up = {};
        Vec3f aim = {};       // aim ray: where a pointing finger / gun barrel points
        Vec3f aim_up = {};
    };
    HandState g_controllers[2];
    LARGE_INTEGER g_last_eval = {};

    // How each model hand is held, in its own joint frame, learned from the
    // animation: the direction that should follow the controller's forward,
    // one that should follow its up, and the point that sits in the palm.
    struct HandReference {
        Vec3f forward_sum = {};
        Vec3f up_sum = {};
        Vec3f anchor_sum = {};
        int samples = 0;
        int anchor_samples = 0;
        int up_axis = 0;  // reference up, chosen on the first sample: 1 = model Y, 2 = model Z
    };
    HandReference g_references[2];
    bool g_logged_references = false;

    // The drawn blade after placement, for saber hits.
    float g_blade_hilt[3] = {};
    float g_blade_tip[3] = {};
    // Height of the neck (head joint) above the character's origin, from the
    // animated pose, smoothed over kNeckSeconds so walking doesn't bob the view.
    constexpr float kNeckSeconds = 0.8f;
    float g_neck_height = 0.0f;
    bool g_neck_valid = false;
    unsigned g_neck_frame = 0;
    const void *g_neck_model = nullptr;

    template <typename T> T Read(const void *base, uintptr_t offset) {
        T value;
        memcpy(&value, static_cast<const unsigned char *>(base) + offset, sizeof(T));
        return value;
    }

    Vec3f Add(Vec3f a, Vec3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec3f Sub(Vec3f a, Vec3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    Vec3f Scale(Vec3f v, float k) { return {v.x * k, v.y * k, v.z * k}; }
    float Dot(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3f Cross(Vec3f a, Vec3f b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
    float Length(Vec3f v) { return std::sqrt(Dot(v, v)); }
    bool IsZero(Vec3f v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

    // ---- Skeleton discovery --------------------------------------------------

    bool CopyIdentifier(const char *text, char *out) {
        for (int i = 0; i < kMaxName; ++i) {
            const char c = text[i];
            if (c == '\0') {
                out[i] = '\0';
                return i > 0;
            }
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) {
                return false;
            }
            out[i] = c;
        }
        return false;
    }

    // A joint's name: the int32 at +0x4c is relative to that field in the
    // file and may have been made absolute when loaded; accept either.
    bool ReadJointName(const unsigned char *joint, char *out) {
        const unsigned char *field = joint + kJointName;
        const auto value = Read<intptr_t>(field, 0);
        __try {
            if (CopyIdentifier(reinterpret_cast<const char *>(field + value), out)) {
                return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        __try {
            if (value > 0x10000 && CopyIdentifier(reinterpret_cast<const char *>(value), out)) {
                return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        out[0] = '\0';
        return false;
    }

    int FindJoint(const Skeleton &s, const char *name) {
        for (int i = 0; i < s.joint_count; ++i) {
            if (_stricmp(s.names[i], name) == 0) {
                return i;
            }
        }
        return -1;
    }

    bool IsDescendant(const Skeleton &s, int joint, int ancestor) {
        for (int guard = 0; guard < kMaxJoints && joint >= 0 && joint < s.joint_count; ++guard) {
            if (joint == ancestor) {
                return true;
            }
            const int parent = s.parents[joint];
            if (parent >= s.joint_count || parent == joint) {
                return false;
            }
            joint = parent;
        }
        return false;
    }

    int Parent(const Skeleton &s, int joint) {
        const int parent = joint >= 0 ? s.parents[joint] : -1;
        return parent < s.joint_count && parent != joint ? parent : -1;
    }

    int LocatorJoint(const Skeleton &s, int locator) {
        const auto *map = Read<const unsigned char *>(s.hierarchy, kHierLocatorMap);
        const auto *locators = Read<const unsigned char *>(s.hierarchy, kHierLocators);
        if (map == nullptr || locators == nullptr || locator < 0 || locator >= 16 ||
            Read<const void *>(s.model, kModelPointsOfInterest + locator * 4) == nullptr) {
            return -1;
        }
        const int joint = locators[map[locator] * kLocatorStride + kLocatorJoint];
        return joint < s.joint_count ? joint : -1;
    }

    // The joint a side's hand is: <side>Hand, else <side>Wrist, else a
    // <side>Elbow that ends the arm (battle droids: forearm and hand in one).
    void FindHand(Skeleton &s, int side) {
        const char *prefix = side == kLeft ? "left" : "right";
        char name[kMaxName];
        for (const char *suffix : {"Hand", "Wrist"}) {
            sprintf_s(name, "%s%s", prefix, suffix);
            const int joint = FindJoint(s, name);
            if (joint >= 0) {
                s.hand[side] = joint;
                s.hand_kind[side] = kHandJoint;
                return;
            }
        }
        // No hand or wrist on this side, so the elbow's piece is the forearm
        // and hand in one (props may still hang off it).
        sprintf_s(name, "%sElbow", prefix);
        const int elbow = FindJoint(s, name);
        if (elbow >= 0) {
            s.hand[side] = elbow;
            s.hand_kind[side] = kForearmJoint;
        }
    }

    bool LoadSkeleton(const void *model, const unsigned char *object, Skeleton *out) {
        __try {
            Skeleton &s = *out;
            s = Skeleton();
            s.model = model;
            s.hierarchy = Read<const unsigned char *>(model, kModelHierarchy);
            if (s.hierarchy == nullptr) {
                return false;
            }
            s.joint_count = Read<int>(s.hierarchy, kHierJointCount);
            if (s.joint_count <= 0 || s.joint_count > kMaxJoints) {
                Log("skeleton: implausible joint count %d", s.joint_count);
                return false;
            }
            const auto *joints = Read<const unsigned char *>(s.hierarchy, kHierJoints);
            int named = 0;
            for (int i = 0; i < s.joint_count; ++i) {
                s.parents[i] = joints[i * kJointStride + kJointParent];
                named += ReadJointName(joints + i * kJointStride, s.names[i]) ? 1 : 0;
            }
            for (const char *name : {"head", "face", "neck"}) {
                if (s.head < 0) {
                    s.head = FindJoint(s, name);
                }
            }
            for (int side = 0; side < 2; ++side) {
                FindHand(s, side);
            }
            // The blade's and the gun's locators, and which hand holds each.
            const auto *character_data = Read<const unsigned char *>(object, kObjCharacterData);
            const auto *game_character =
                character_data != nullptr ? Read<const unsigned char *>(character_data, kCharDataGameCharacter) : nullptr;
            if (game_character != nullptr) {
                s.blade_locator[0] = Read<signed char>(game_character, kGameCharStreakLocators);
                s.blade_locator[1] = Read<signed char>(game_character, kGameCharStreakLocators + 1);
                s.gun_locator[0] = Read<signed char>(game_character, kGameCharWeaponLocators);
                s.gun_locator[1] = Read<signed char>(game_character, kGameCharShootLocators);
                int gun_joint = LocatorJoint(s, s.gun_locator[0]);
                if (gun_joint < 0) {
                    gun_joint = LocatorJoint(s, s.gun_locator[1]);
                }
                const int blade_joint = LocatorJoint(s, s.blade_locator[0]);
                for (int side = 0; side < 2; ++side) {
                    if (s.hand[side] >= 0 && gun_joint >= 0 && IsDescendant(s, gun_joint, s.hand[side])) {
                        s.gun_side = side;
                    }
                    if (s.hand[side] >= 0 && blade_joint >= 0 && IsDescendant(s, blade_joint, s.hand[side])) {
                        s.blade_side = side;
                    }
                }
                if (gun_joint >= 0 && s.gun_side < 0 && s.blade_side < 0) {
                    Log("skeleton: the gun hangs off joint %d (%s), not a hand: it hides with the body", gun_joint,
                        s.names[gun_joint]);
                }
            }
            if (named == 0) {
                Log("skeleton: no joint names readable; VR hands and head hiding off for this character");
            }
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("skeleton: reading the player's skeleton faulted");
            return false;
        }
    }

    const char *SideName(int side) {
        return side < 0 ? "(no)" : (side == kLeft ? "left" : "right");
    }

    void DumpSkeleton(const Skeleton &s, const float *joints) {
        Log("skeleton: model %p, %d joints; head %d (%s); hands L=%d %s R=%d %s; blade locators %d,%d on the %s hand; "
            "gun locators %d,%d on the %s hand",
            s.model, s.joint_count, s.head, s.head >= 0 ? s.names[s.head] : "-", s.hand[0],
            s.hand[0] < 0 ? "-" : s.names[s.hand[0]], s.hand[1], s.hand[1] < 0 ? "-" : s.names[s.hand[1]],
            s.blade_locator[0], s.blade_locator[1], SideName(s.blade_side), s.gun_locator[0], s.gun_locator[1],
            SideName(s.gun_side));
        for (int i = 0; i < s.joint_count; ++i) {
            const float *m = joints + i * 16;
            Log("  joint %2d %-18s parent %3d pos (% .3f % .3f % .3f)", i, s.names[i][0] ? s.names[i] : "?",
                s.parents[i], m[12], m[13], m[14]);
        }
    }

    // ---- Joint editing -------------------------------------------------------

    // Replaces joint `index` with `replacement` (joint -> model) and carries
    // every descendant along, since the matrices are already flattened.
    void SetJoint(const Skeleton &s, float *joints, int index, const float *replacement) {
        float inverse_old[16];
        if (!Mat4InverseAffine(inverse_old, joints + index * 16)) {
            return;
        }
        float delta[16];  // old joint space -> new model space
        Mat4Mul(delta, inverse_old, replacement);
        for (int j = 0; j < s.joint_count; ++j) {
            if (j != index && IsDescendant(s, j, index)) {
                Mat4Mul(joints + j * 16, joints + j * 16, delta);
            }
        }
        memcpy(joints + index * 16, replacement, 16 * sizeof(float));
    }

    Vec3f JointPosition(const float *joints, int index) {
        const float *m = joints + index * 16;
        return {m[12], m[13], m[14]};
    }

    // Shrinks one joint (only its own geometry, not its descendants) to a
    // point.
    void CollapseJoint(float *joints, int index, Vec3f point) {
        float *m = joints + index * 16;
        for (int i = 0; i < 12; ++i) {
            m[i] *= kHiddenScale;
        }
        m[12] = point.x;
        m[13] = point.y;
        m[14] = point.z;
    }

    // Shrinks the head about its own origin; hair, hats and helmets under it
    // follow, so first person never sees the inside of the head.
    void HideHead(const Skeleton &s, float *joints) {
        if (s.head < 0) {
            return;
        }
        const float shrink[16] = {kHiddenScale, 0, 0, 0, 0, kHiddenScale, 0, 0, 0, 0, kHiddenScale, 0, 0, 0, 0, 1};
        float hidden[16];
        Mat4Mul(hidden, shrink, joints + s.head * 16);
        SetJoint(s, joints, s.head, hidden);
    }

    // Test pose: turn each hand about its own X axis.
    void ApplyTestPose(const Skeleton &s, float *joints) {
        const float radians = kTestPoseDegrees * 3.14159265f / 180.0f;
        const float c = std::cos(radians);
        const float sn = std::sin(radians);
        const float rotate_x[16] = {1, 0, 0, 0, 0, c, sn, 0, 0, -sn, c, 0, 0, 0, 0, 1};
        for (int side = 0; side < 2; ++side) {
            const int joint = s.hand[side];
            if (joint < 0) {
                continue;
            }
            float turned[16];
            Mat4Mul(turned, rotate_x, joints + joint * 16);  // local rotation, then the joint's own transform
            SetJoint(s, joints, joint, turned);
        }
    }

    // ---- VR hands ------------------------------------------------------------

    float EvalSeconds() {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const float dt = g_last_eval.QuadPart == 0
                             ? 0.0f
                             : static_cast<float>(now.QuadPart - g_last_eval.QuadPart) / frequency.QuadPart;
        g_last_eval = now;
        return std::clamp(dt, 0.0f, 0.1f);
    }

    Vec3f Lerp(Vec3f a, Vec3f b, float t) {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }

    // Smooths a controller's target (world space) against tracking jitter.
    void FilterTarget(HandState &hand, const HandState &raw, float dt) {
        if (!hand.has_filter) {
            hand = raw;
            hand.has_filter = true;
            return;
        }
        const float a = dt > 0.0f ? 1.0f - std::exp(-dt / kFilterSeconds) : 1.0f;
        hand.position = Lerp(hand.position, raw.position, a);
        hand.forward = Normalize(Lerp(hand.forward, raw.forward, a));
        hand.up = Normalize(Lerp(hand.up, raw.up, a));
        hand.aim = Normalize(Lerp(hand.aim, raw.aim, a));
        hand.aim_up = Normalize(Lerp(hand.aim_up, raw.aim_up, a));
    }

    // A locator's position (model space with world = identity, else world).
    bool LocatorPoint(const Skeleton &s, int locator, const float *world, const float *joints, Vec3f *out) {
        if (g_poi_mtx == nullptr || locator < 0 || LocatorJoint(s, locator) < 0) {
            return false;
        }
        float m[16];
        g_poi_mtx(s.hierarchy, locator, world, joints, m);
        *out = {m[12], m[13], m[14]};
        return true;
    }

    const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    // World-space blade (base, tip) from the game's blade locators.
    bool BladeWorld(const Skeleton &s, const float *world, const float *joints, Vec3f *base, Vec3f *tip) {
        return LocatorPoint(s, s.blade_locator[0], world, joints, base) &&
               LocatorPoint(s, s.blade_locator[1], world, joints, tip);
    }

    // Which controller (0 left, 1 right) drives a model hand. The gun is
    // always in the right controller; the saber in the one that drew it.
    int ControllerFor(const Skeleton &s, int model_side) {
        int held = -1, controller = -1;
        if (s.blade_side >= 0) {
            held = s.blade_side;
            controller = VrInteractionSaberHand();
        } else if (s.gun_side >= 0) {
            held = s.gun_side;
            controller = kRight;
        }
        const bool swap = held >= 0 && controller >= 0 && controller != held;
        return swap ? 1 - model_side : model_side;
    }

    // Learns, from the animated (unedited) pose, how each hand holds things in
    // its joint frame. A blade or gun barrel is rigid in the hand; for an empty
    // hand the forearm direction and the character's forward are averaged over
    // the first frames (minifig wrists only twist, so this is near constant).
    void SampleReferences(const Skeleton &s, const float *joints) {
        for (int side = 0; side < 2; ++side) {
            const int joint = s.hand[side];
            HandReference &ref = g_references[side];
            if (joint < 0 || ref.samples >= kReferenceSamples) {
                continue;
            }
            float inverse_hand[16];
            if (!Mat4InverseAffine(inverse_hand, joints + joint * 16)) {
                continue;
            }
            Vec3f forward = {0.0f, 0.0f, 1.0f};  // the character's forward
            Vec3f up = {0.0f, 1.0f, 0.0f};
            Vec3f a, b;
            const int parent = Parent(s, joint);
            // A droid's empty forearm: it runs the way the gun forearm runs to
            // its grip (same local axis on a symmetric or mirrored rig).
            const HandReference &other = g_references[1 - side];
            const bool droid_arm = s.hand_kind[side] == kForearmJoint && side != s.gun_side &&
                                   s.gun_side == 1 - side && s.hand_kind[1 - side] == kForearmJoint;
            if (droid_arm && other.anchor_samples == 0) {
                continue;  // learn the gun arm first
            }
            if (droid_arm) {
                // forward is set in local space below
            } else if (side == s.blade_side && BladeWorld(s, kIdentity, joints, &a, &b) && Length(Sub(b, a)) > 1e-5f) {
                forward = Sub(b, a);
            } else if (side == s.gun_side && LocatorPoint(s, s.gun_locator[0], kIdentity, joints, &a) &&
                       LocatorPoint(s, s.gun_locator[1], kIdentity, joints, &b) && Length(Sub(b, a)) > 1e-5f) {
                forward = Sub(b, a);  // the barrel
            } else if (s.hand_kind[side] == kHandJoint && parent >= 0) {
                // The fist continues the forearm; the claw opens to the
                // character's front in the animation, which becomes the top of
                // the hand when it points forward.
                forward = Sub(JointPosition(joints, joint), JointPosition(joints, parent));
                up = {0.0f, 0.0f, 1.0f};
            }
            // The up reference must not run along the forward (e.g. a blade
            // held upright). Chosen once, so the average never mixes two.
            if (ref.up_axis == 0) {
                const bool along = std::fabs(Dot(Normalize(forward), up)) > 0.7f;
                ref.up_axis = (up.y != 0.0f) != along ? 1 : 2;
            }
            up = ref.up_axis == 1 ? Vec3f{0.0f, 1.0f, 0.0f} : Vec3f{0.0f, 0.0f, 1.0f};
            const Vec3f local_forward = droid_arm ? Normalize(other.anchor_sum)
                                                  : Normalize(TransformDirection(Normalize(forward), inverse_hand));
            const Vec3f local_up = Normalize(TransformDirection(up, inverse_hand));
            if (IsZero(local_forward) || IsZero(local_up)) {
                continue;
            }
            ref.forward_sum = Add(ref.forward_sum, local_forward);
            ref.up_sum = Add(ref.up_sum, local_up);
            ++ref.samples;
            // The gun's grip goes in the palm.
            if (side == s.gun_side && LocatorPoint(s, s.gun_locator[0], kIdentity, joints, &a)) {
                ref.anchor_sum = Add(ref.anchor_sum, TransformPoint(a, inverse_hand));
                ++ref.anchor_samples;
            }
        }
    }

    Vec3f ReferenceAnchor(const Skeleton &s, int side) {
        const HandReference &ref = g_references[side];
        if (ref.anchor_samples > 0) {
            return Scale(ref.anchor_sum, 1.0f / ref.anchor_samples);
        }
        // A droid's empty forearm: its hand end is as far along it as the gun
        // hand's grip is along the other one.
        const HandReference &other = g_references[1 - side];
        if (s.hand_kind[side] == kForearmJoint && s.hand_kind[1 - side] == kForearmJoint && other.anchor_samples > 0) {
            const float length = Length(Scale(other.anchor_sum, 1.0f / other.anchor_samples));
            return Scale(Normalize(ref.forward_sum), length);
        }
        return {0.0f, 0.0f, 0.0f};
    }

    // Puts a model hand on its controller: the learned forward follows the
    // controller (a blade the grip's forward, anything else the aim ray), the
    // learned up its up, and the learned anchor sits at the palm.
    bool PlaceHand(const Skeleton &s, int side, const HandState &controller, const float *inverse_world,
                   float *joints) {
        const int joint = s.hand[side];
        const HandReference &ref = g_references[side];
        if (joint < 0 || ref.samples == 0) {
            return false;
        }
        const float *hm = joints + joint * 16;
        const Vec3f local_forward = Normalize(ref.forward_sum);
        const Vec3f local_up = Normalize(ref.up_sum);
        const bool blade = side == s.blade_side;
        Vec3f target_forward = Normalize(TransformDirection(blade ? controller.forward : controller.aim, inverse_world));
        Vec3f target_up = Normalize(TransformDirection(blade ? controller.up : controller.aim_up, inverse_world));
        if (IsZero(local_forward) || IsZero(local_up) || IsZero(target_forward) || IsZero(target_up)) {
            return false;
        }
        float q[16];
        AlignFrames(q, local_forward, local_up, target_forward, target_up);
        // Rigs may mirror one side (negative determinant). Keep the mirror,
        // or the hand turns inside out: reflect across the forward/up plane.
        const float det = hm[0] * (hm[5] * hm[10] - hm[6] * hm[9]) - hm[1] * (hm[4] * hm[10] - hm[6] * hm[8]) +
                          hm[2] * (hm[4] * hm[9] - hm[5] * hm[8]);
        if (det < 0.0f) {
            target_up = Normalize(Sub(target_up, Scale(target_forward, Dot(target_up, target_forward))));
            const Vec3f n = Normalize(Cross(target_forward, target_up));
            const float reflect[16] = {1 - 2 * n.x * n.x, -2 * n.x * n.y,    -2 * n.x * n.z,    0,
                                       -2 * n.y * n.x,    1 - 2 * n.y * n.y, -2 * n.y * n.z,    0,
                                       -2 * n.z * n.x,    -2 * n.z * n.y,    1 - 2 * n.z * n.z, 0,
                                       0,                 0,                 0,                 1};
            Mat4Mul(q, q, reflect);
        }
        // Keep the hand's own scale per axis.
        float replacement[16];
        for (int row = 0; row < 3; ++row) {
            const float length = std::sqrt(hm[row * 4] * hm[row * 4] + hm[row * 4 + 1] * hm[row * 4 + 1] +
                                           hm[row * 4 + 2] * hm[row * 4 + 2]);
            for (int col = 0; col < 3; ++col) {
                replacement[row * 4 + col] = q[row * 4 + col] * length;
            }
            replacement[row * 4 + 3] = 0.0f;
        }
        const Vec3f goal = TransformPoint(controller.position, inverse_world);
        const Vec3f offset = TransformDirection(ReferenceAnchor(s, side), replacement);
        replacement[12] = goal.x - offset.x;
        replacement[13] = goal.y - offset.y;
        replacement[14] = goal.z - offset.z;
        replacement[15] = 1.0f;
        SetJoint(s, joints, joint, replacement);
        return true;
    }

    // Every joint but the placed hands (and what hangs off them) shrinks to
    // nothing: arm joints into their own wrist, everything else into the feet.
    void HideAllButHands(const Skeleton &s, float *joints, const bool placed[2]) {
        Vec3f wrist[2] = {};
        for (int side = 0; side < 2; ++side) {
            if (s.hand[side] >= 0) {
                wrist[side] = JointPosition(joints, s.hand[side]);
            }
        }
        for (int j = 0; j < s.joint_count; ++j) {
            bool keep = false;
            for (int side = 0; side < 2; ++side) {
                keep = keep || (placed[side] && IsDescendant(s, j, s.hand[side]));
            }
            if (keep) {
                continue;
            }
            Vec3f point = {0.0f, 0.0f, 0.0f};
            for (int side = 0; side < 2; ++side) {
                const int hand = s.hand[side], other = s.hand[1 - side];
                if (placed[side] && IsDescendant(s, hand, j) && !(other >= 0 && IsDescendant(s, other, j))) {
                    point = wrist[side];
                }
            }
            CollapseJoint(joints, j, point);
        }
    }

    // VR first person: controllers -> hands. Sets placed[side] for the hands
    // now on a controller; the rest of the body is hidden after the game has
    // stored its locators (HideAllButHands).
    void ApplyVrHands(const Skeleton &s, const float *world, float *joints, const VrTrackingState &tracking,
                      bool placed[2]) {
        const float dt = EvalSeconds();
        float inverse_world[16];
        if (!Mat4InverseAffine(inverse_world, world)) {
            return;
        }
        SampleReferences(s, joints);
        const bool learned = (s.hand[0] < 0 || g_references[0].samples >= kReferenceSamples) &&
                             (s.hand[1] < 0 || g_references[1].samples >= kReferenceSamples);
        if (!g_logged_references && learned) {
            g_logged_references = true;
            for (int side = 0; side < 2; ++side) {
                const HandReference &r = g_references[side];
                const Vec3f f = Normalize(r.forward_sum), u = Normalize(r.up_sum), a = ReferenceAnchor(s, side);
                Log("skeleton: %s hand (joint %d, %s) -> %s controller; forward (% .2f % .2f % .2f) up (% .2f % .2f "
                    "% .2f) palm (% .3f % .3f % .3f)",
                    SideName(side), s.hand[side], s.hand_kind[side] == kForearmJoint ? "forearm" : "hand",
                    SideName(ControllerFor(s, side)), f.x, f.y, f.z, u.x, u.y, u.z, a.x, a.y, a.z);
            }
        }
        for (int c = 0; c < 2; ++c) {
            HandState raw;
            float position[3], axes[9];
            float aim_position[3], aim_axes[9];
            if (tracking.hands[c].tracked && CameraTrackingToWorld(tracking.hands[c].grip, position, axes) &&
                CameraTrackingToWorld(tracking.hands[c].aim, aim_position, aim_axes)) {
                raw.position = {position[0], position[1], position[2]};
                raw.forward = {axes[6], axes[7], axes[8]};
                raw.up = {axes[3], axes[4], axes[5]};
                raw.aim = {aim_axes[6], aim_axes[7], aim_axes[8]};
                raw.aim_up = {aim_axes[3], aim_axes[4], aim_axes[5]};
                FilterTarget(g_controllers[c], raw, dt);
            } else {
                // Untracked: hide that hand rather than leave it hanging in
                // the world while the player moves on.
                g_controllers[c].has_filter = false;
            }
        }
        for (int side = 0; side < 2; ++side) {
            const HandState &controller = g_controllers[ControllerFor(s, side)];
            placed[side] = controller.has_filter && PlaceHand(s, side, controller, inverse_world, joints);
        }
    }

    // ---- Hook ----------------------------------------------------------------

    // The player's character model and object, or nullptr (read defensively:
    // objects are rebuilt during level loads).
    const void *PlayerModel(const unsigned char **object_out) {
        __try {
            const auto *object = *GameAddress<const unsigned char *const>(kPlayers);
            if (object == nullptr) {
                return nullptr;
            }
            *object_out = object;
            return Read<const void *>(object, kObjCharacterModel);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // Characters of one type share a model, so the model alone doesn't say
    // whose joints these are: the locator output is the drawn object's own
    // array, or else the drawn world position is the player's.
    bool IsPlayerDraw(const unsigned char *player, const float *world, const void *matrices) {
        if ((matrices != nullptr && matrices == player + kObjLocatorMatrices) ||
            reinterpret_cast<const unsigned char *>(world) == player + kObjWorldMatrix) {
            return true;
        }
        __try {
            const float dx = world[12] - Read<float>(player, kObjPosition);
            const float dy = world[13] - Read<float>(player, kObjPosition + 4);
            const float dz = world[14] - Read<float>(player, kObjPosition + 8);
            return dx * dx + dy * dy + dz * dz < 0.1f * 0.1f;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void MeasureNeck(const void *model, const float *world, const float *joints) {
        if (g_skeleton.head < 0) {
            return;
        }
        const float *head = joints + g_skeleton.head * 16;
        if (head[0] * head[0] + head[1] * head[1] + head[2] * head[2] < 1e-4f) {
            return;  // already hidden (joints passed through twice): no real height
        }
        const Vec3f neck = TransformPoint(JointPosition(joints, g_skeleton.head), world);
        const float height = neck.y - world[13];
        if (!g_neck_valid || g_neck_model != model) {
            g_neck_height = height;
        } else {
            // Own clock: EvalSeconds() is the hand filter's (one call per frame).
            static LARGE_INTEGER last = {};
            LARGE_INTEGER now, frequency;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            const float dt = last.QuadPart == 0
                                 ? 0.0f
                                 : std::min(0.1f, static_cast<float>(now.QuadPart - last.QuadPart) / frequency.QuadPart);
            last = now;
            const float a = dt > 0.0f ? 1.0f - std::exp(-dt / kNeckSeconds) : 0.0f;
            g_neck_height += (height - g_neck_height) * a;
        }
        g_neck_valid = true;
        g_neck_model = model;
        g_neck_frame = g_frame_counter;
    }

    // What to hide once the game has stored the locators from the joints.
    // Locators for game logic (some characters take their collision position
    // from one) keep the animated body; only the drawing loses it.
    enum Hide { kHideNothing, kHideHead, kHideAllButHands };
    struct HidePlan {
        Hide hide = kHideNothing;
        bool placed[2] = {};
    };

    HidePlan EditPlayerJoints(const void *model, float *world, float *joints) {
        HidePlan plan;
        __try {
            if (g_dump_requested) {
                DumpSkeleton(g_skeleton, joints);
                g_dump_requested = false;
            }
            MeasureNeck(model, world, joints);  // from the animated pose, before any edit
            const VrTrackingState *tracking = VrTracking();
            if (g_test_pose) {
                ApplyTestPose(g_skeleton, joints);
            } else if (CameraFirstPersonRecent() && tracking != nullptr) {
                ApplyVrHands(g_skeleton, world, joints, *tracking, plan.placed);
                plan.hide = kHideAllButHands;
            } else {
                // Not first person (menus, cutscenes, F10): the full body,
                // with fresh hand filters when first person resumes.
                g_controllers[0].has_filter = g_controllers[1].has_filter = false;
                if (CameraFirstPersonRecent()) {
                    plan.hide = kHideHead;  // desktop first person
                }
            }
            g_player_drawn_frame = g_frame_counter;
            Vec3f base, tip;
            if (g_skeleton.blade_side >= 0 && BladeWorld(g_skeleton, world, joints, &base, &tip)) {
                g_blade_hilt[0] = base.x, g_blade_hilt[1] = base.y, g_blade_hilt[2] = base.z;
                g_blade_tip[0] = tip.x, g_blade_tip[1] = tip.y, g_blade_tip[2] = tip.z;
                g_blade_frame = g_frame_counter;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("skeleton: editing the player's joints faulted");
            plan.hide = kHideNothing;
        }
        return plan;
    }

    // Helmets, hats, accessories and carried extras are drawn after the
    // character at locators on its head (helmet_locator, head_locator...).
    // Those locators were stored from the animated head, right at the eyes:
    // store them again from the hidden head so these things hide with it.
    // Locators elsewhere on the body (collision origins) are left alone.
    void RestoreHeadLocators(const Skeleton &s, const float *world, const float *joints, float *positions,
                             float *matrices) {
        if (g_poi_mtx == nullptr || s.head < 0 || (positions == nullptr && matrices == nullptr)) {
            return;
        }
        for (int locator = 0; locator < 16; ++locator) {
            const int joint = LocatorJoint(s, locator);
            if (joint < 0 || !IsDescendant(s, joint, s.head)) {
                continue;
            }
            float m[16];
            g_poi_mtx(s.hierarchy, locator, world, joints, m);
            if (matrices != nullptr) {
                memcpy(matrices + locator * 16, m, sizeof(m));
            }
            if (positions != nullptr) {
                memcpy(positions + locator * 3, m + 12, 3 * sizeof(float));
            }
        }
    }

    void HidePlayerBody(const HidePlan &plan, const float *world, float *joints, void *positions, void *matrices) {
        __try {
            if (plan.hide == kHideAllButHands) {
                HideAllButHands(g_skeleton, joints, plan.placed);
            } else if (plan.hide == kHideHead) {
                HideHead(g_skeleton, joints);
            }
            if (plan.hide != kHideNothing) {
                RestoreHeadLocators(g_skeleton, world, joints, static_cast<float *>(positions),
                                    static_cast<float *>(matrices));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("skeleton: hiding the player's body faulted");
        }
    }

    void __cdecl HookStoreLocatorCoordinates(void *model, float *world, float *joints, void *positions,
                                             void *matrices) {
        NoteGameThread();
        const unsigned char *object = nullptr;
        HidePlan plan;
        if (joints != nullptr && world != nullptr && model != nullptr && model == PlayerModel(&object) &&
            IsPlayerDraw(object, world, matrices)) {
            // Re-check identity on every draw: after a level load another model
            // can reuse the address, and stale joint indices/counts would write
            // outside this joint buffer.
            const void *hierarchy = nullptr;
            int joint_count = -1;
            __try {
                hierarchy = Read<const void *>(model, kModelHierarchy);
                joint_count = hierarchy != nullptr ? Read<int>(hierarchy, kHierJointCount) : -1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            const bool same = g_skeleton.model == model && g_skeleton.hierarchy == hierarchy &&
                              g_skeleton.joint_count == joint_count;
            const bool known_bad = model == g_failed_model && hierarchy == g_failed_hierarchy;
            if (!same && !known_bad) {
                g_controllers[0] = g_controllers[1] = HandState();
                g_references[0] = g_references[1] = HandReference();
                g_logged_references = false;
                if (LoadSkeleton(model, object, &g_skeleton)) {
                    g_dump_requested = true;
                } else {
                    g_skeleton = Skeleton();
                    g_failed_model = model;
                    g_failed_hierarchy = hierarchy;
                }
            }
            if (!known_bad && g_skeleton.model == model && g_skeleton.hierarchy == hierarchy &&
                g_skeleton.joint_count == joint_count) {
                plan = EditPlayerJoints(model, world, joints);
            }
        }
        g_real_store(model, world, joints, positions, matrices);
        HidePlayerBody(plan, world, joints, positions, matrices);  // drawn from these joints next
    }

    bool Pressed(int key) {
        const SHORT down = GetAsyncKeyState(key) & 0x8000;
        const bool pressed = down && !g_prev_keys[key];
        g_prev_keys[key] = down;
        return pressed;
    }
} // namespace

void InstallSkeletonHook() {
    g_poi_mtx = GameFunction<PoiMtxFn>("NuHGobjPOIMtx", kPoiMtx, kPoiMtxPrologue);
    if (HookGameFunction("StoreLocatorCoordinates", kStoreLocatorCoordinates, kStoreLocatorPrologue,
                         sizeof(kStoreLocatorPrologue), reinterpret_cast<void *>(&HookStoreLocatorCoordinates),
                         reinterpret_cast<void **>(&g_real_store))) {
        Log("skeleton: VR first person shows hands only (on the controllers); F6 dumps the player skeleton, F11 "
            "toggles the %.0f deg hand test pose",
            kTestPoseDegrees);
    }
}

namespace {
    // Keeps the player drawn whatever the view (kRenderNoClip) in VR first
    // person; restores the flag otherwise and on a new player object.
    void UpdateNoClip() {
        __try {
            auto *player = *GameAddress<unsigned char *const>(kPlayers);
            const bool want = player != nullptr && CameraFirstPersonRecent() && VrTracking() != nullptr;
            if (g_no_clip_object != nullptr && (g_no_clip_object != player || !want)) {
                *reinterpret_cast<unsigned *>(g_no_clip_object + kObjRenderFlags) &= ~kRenderNoClip;
                g_no_clip_object = nullptr;
            }
            if (want && g_no_clip_object == nullptr &&
                (Read<unsigned>(player, kObjRenderFlags) & kRenderNoClip) == 0) {
                *reinterpret_cast<unsigned *>(player + kObjRenderFlags) |= kRenderNoClip;
                g_no_clip_object = player;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_no_clip_object = nullptr;
        }
    }
} // namespace

void SkeletonOnPresent() {
    if (g_real_store == nullptr) {
        return;
    }
    UpdateNoClip();
    ++g_frame_counter;  // stamps from here on belong to the next frame
    if (Pressed(VK_F6)) {
        g_dump_requested = true;
    }
    if (Pressed(VK_F11)) {
        g_test_pose = !g_test_pose;
        Log("skeleton: hand test pose %s (hands L=%d R=%d)", g_test_pose ? "on" : "off", g_skeleton.hand[0],
            g_skeleton.hand[1]);
    }
}

bool SkeletonNeckHeight(float *height) {
    if (!g_neck_valid || g_frame_counter - g_neck_frame > 30) {
        return false;  // player not drawn recently
    }
    *height = g_neck_height;
    return true;
}

bool SkeletonBladeWorld(float hilt[3], float tip[3]) {
    if (g_blade_frame != g_frame_counter) {
        return false;  // not drawn this frame
    }
    memcpy(hilt, g_blade_hilt, sizeof(g_blade_hilt));
    memcpy(tip, g_blade_tip, sizeof(g_blade_tip));
    return true;
}

bool SkeletonPlayerDrawn() {
    return g_player_drawn_frame == g_frame_counter;
}
