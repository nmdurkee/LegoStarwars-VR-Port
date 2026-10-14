#include "sim.h"

#include "../shared/png_write.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "../camera.h"
#include "../log.h"

namespace {
    // Rift CV1 as reported by the runtime (lsw_vr_host.log): per-eye field of
    // view in degrees (outer, inner, up, down) and eye separation.
    constexpr float kOuterDeg = 44.0f, kInnerDeg = 35.6f, kUpDeg = 41.7f, kDownDeg = 48.0f;
    constexpr float kIpd = 0.0639f;
    constexpr float kHeadHeight = 1.6f;
    constexpr float kPi = 3.14159265f;
    // Each eye's crop in sim_headset.png, roughly a CV1 panel's aspect.
    constexpr int kEyeOutWidth = 540, kEyeOutHeight = 600;

    bool g_checked = false;
    bool g_active = false;
    char g_folder[MAX_PATH] = {};

    float g_yaw = 0.0f, g_pitch = 0.0f;  // degrees
    float g_offset[3] = {};              // meters
    // Fake controllers (tracking space, meters), held in front at waist
    // height pointing forward, like a player standing ready.
    bool g_hands_on = true;
    float g_hand_pos[2][3] = {{-0.20f, 1.05f, -0.35f}, {0.20f, 1.05f, -0.35f}};
    float g_hand_pitch[2] = {0.0f, 0.0f};  // degrees, + = pointing up
    float g_trigger[2] = {};

    struct Command {
        std::string verb;
        float a = 0, b = 0, c = 0, d = 0;
        int frames = 0;
    };
    std::deque<Command> g_queue;
    unsigned g_buttons = 0;
    bool g_has_stick = false;
    float g_stick[2] = {};
    int g_frames_left = 0;  // of the current timed command
    bool g_dump_pending = false;
    int g_dump_frames = 0;  // frames captured since the dump request

    // Latest captured image per eye (index 0 left, 1 right; mono/screen -> both).
    struct EyeImage {
        std::vector<uint8_t> pixels;
        uint32_t width = 0, height = 0;
        VrFrameMeta meta = {};
        bool valid = false;
    };
    EyeImage g_eyes[2];
    // Labels of recent captured frames (diagnostic: should alternate 2/3).
    int g_kinds[24] = {};
    int g_kind_count = 0;

    std::string PathOf(const char *name) {
        return std::string(g_folder) + name;
    }

    using png_write::WritePng;

    // Samples a BGRA image region (in pixels, floats) into an RGB canvas area.
    void Blit(const EyeImage &eye, float x0, float y0, float x1, float y1, std::vector<uint8_t> &canvas,
              int canvas_width, int out_x, int out_y, int out_width, int out_height) {
        for (int y = 0; y < out_height; ++y) {
            for (int x = 0; x < out_width; ++x) {
                const float sx = x0 + (x + 0.5f) * (x1 - x0) / out_width;
                const float sy = y0 + (y + 0.5f) * (y1 - y0) / out_height;
                uint8_t *dst = &canvas[(static_cast<size_t>(out_y + y) * canvas_width + out_x + x) * 3];
                if (sx < 0 || sy < 0 || sx >= eye.width || sy >= eye.height) {
                    dst[0] = 255, dst[1] = 0, dst[2] = 255;  // magenta: outside the rendered image
                    continue;
                }
                const uint8_t *src = &eye.pixels[(static_cast<size_t>(sy) * eye.width + static_cast<size_t>(sx)) * 4];
                dst[0] = src[2], dst[1] = src[1], dst[2] = src[0];
            }
        }
    }

    void WriteDump() {
        for (int e = 0; e < 2; ++e) {
            const EyeImage &eye = g_eyes[e];
            if (!eye.valid) {
                continue;
            }
            const int w = static_cast<int>(eye.width / 2), h = static_cast<int>(eye.height / 2);
            std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
            Blit(eye, 0, 0, static_cast<float>(eye.width), static_cast<float>(eye.height), rgb, w, 0, 0, w, h);
            WritePng(PathOf(e == 0 ? "sim_left.png" : "sim_right.png"), rgb, w, h);
        }
        // What each eye sees: its real (asymmetric) field of view cut out of
        // the image, which was rendered for the symmetric tangents in its meta.
        std::vector<uint8_t> canvas(static_cast<size_t>(kEyeOutWidth) * 2 * kEyeOutHeight * 3, 0);
        const float out_t = std::tan(kOuterDeg * kPi / 180), in_t = std::tan(kInnerDeg * kPi / 180);
        const float up_t = std::tan(kUpDeg * kPi / 180), down_t = std::tan(kDownDeg * kPi / 180);
        for (int e = 0; e < 2; ++e) {
            const EyeImage &eye = g_eyes[e].valid ? g_eyes[e] : g_eyes[1 - e];
            if (!eye.valid) {
                continue;
            }
            const VrFovTangents &f = eye.meta.has_pose ? eye.meta.fov : VrFovTangents{-1, 1, 1, -1};
            const float left = e == 0 ? -out_t : -in_t, right = e == 0 ? in_t : out_t;
            auto px = [&](float t) { return (t - f.left) / (f.right - f.left) * eye.width; };
            auto py = [&](float t) { return (f.up - t) / (f.up - f.down) * eye.height; };
            Blit(eye, px(left), py(up_t), px(right), py(-down_t), canvas, kEyeOutWidth * 2, e * kEyeOutWidth, 0,
                 kEyeOutWidth, kEyeOutHeight);
        }
        WritePng(PathOf("sim_headset.png"), canvas, kEyeOutWidth * 2, kEyeOutHeight);

        FILE *info = nullptr;
        if (fopen_s(&info, PathOf("sim_info.txt").c_str(), "w") == 0 && info != nullptr) {
            fprintf(info, "look yaw %.1f pitch %.1f, head offset %.2f %.2f %.2f\n", g_yaw, g_pitch, g_offset[0],
                    g_offset[1], g_offset[2]);
            for (int e = 0; e < 2; ++e) {
                const EyeImage &eye = g_eyes[e];
                fprintf(info, "eye %d: valid %d, %ux%u, kind %d, pos %.4f %.4f %.4f, fov tan l %.3f r %.3f u %.3f d %.3f\n",
                        e, eye.valid, eye.width, eye.height, eye.meta.has_pose, eye.meta.pose.position[0],
                        eye.meta.pose.position[1], eye.meta.pose.position[2], eye.meta.fov.left, eye.meta.fov.right,
                        eye.meta.fov.up, eye.meta.fov.down);
            }
            fprintf(info, "recent frame kinds (0 screen, 1 mono, 2 left, 3 right):");
            for (int i = 0; i < 24; ++i) {
                fprintf(info, " %d", g_kinds[(g_kind_count + i) % 24]);
            }
            fputc('\n', info);
            char camera[256];
            CameraDescribe(camera, sizeof(camera));
            fprintf(info, "%s\n", camera);
            fclose(info);
        }
        Log("sim: dump written (sim_left/right/headset.png, sim_info.txt)");
    }

    // ---- Commands ----------------------------------------------------------

    void ReadCommands() {
        const std::string path = PathOf("lsw_vr_cmd.txt");
        FILE *file = nullptr;
        if (fopen_s(&file, path.c_str(), "r") != 0 || file == nullptr) {
            return;
        }
        char line[256];
        while (fgets(line, sizeof(line), file) != nullptr) {
            char verb[32] = {};
            Command c;
            char word[32] = {};
            const int n = sscanf_s(line, "%31s %f %f %f %f", verb, static_cast<unsigned>(sizeof(verb)), &c.a, &c.b, &c.c, &c.d);
            if (n < 1) {
                continue;
            }
            c.verb = verb;
            if (c.verb == "press" && sscanf_s(line, "press %31s %d", word, static_cast<unsigned>(sizeof(word)),
                                              &c.frames) >= 1) {
                c.verb = std::string("press:") + word;
                if (c.frames <= 0) c.frames = 6;
            } else if (c.verb == "stick") {
                c.frames = n >= 4 ? static_cast<int>(c.c) : 30;
            } else if (c.verb == "wait") {
                c.frames = static_cast<int>(c.a);
            }
            g_queue.push_back(c);
        }
        fclose(file);
        DeleteFileA(path.c_str());
        Log("sim: %zu commands queued", g_queue.size());
    }

    unsigned ButtonBit(const std::string &name) {
        if (name == "a") return kSimA;
        if (name == "b") return kSimB;
        if (name == "x") return kSimX;
        if (name == "y") return kSimY;
        if (name == "start") return kSimStart;
        if (name == "up") return kSimUp;
        if (name == "down") return kSimDown;
        if (name == "left") return kSimLeft;
        if (name == "right") return kSimRight;
        return 0;
    }

    // Starts the next command; returns true while a timed command runs.
    void Step() {
        if (g_frames_left > 0) {
            --g_frames_left;
            if (g_frames_left > 0) {
                return;
            }
            g_buttons = 0;
            g_has_stick = false;
            // A gap after a press, so the next one is a new press.
            g_frames_left = 0;
        }
        while (!g_queue.empty() && g_frames_left == 0 && !g_dump_pending) {
            const Command c = g_queue.front();
            g_queue.pop_front();
            if (c.verb.rfind("press:", 0) == 0) {
                g_buttons = ButtonBit(c.verb.substr(6));
                g_frames_left = c.frames;
                g_queue.push_front({"wait", 0, 0, 0, 6});  // release before the next command
            } else if (c.verb == "stick") {
                g_has_stick = true;
                g_stick[0] = c.a, g_stick[1] = c.b;
                g_frames_left = c.frames;
            } else if (c.verb == "wait") {
                g_frames_left = c.frames;
            } else if (c.verb == "look") {
                g_yaw = c.a, g_pitch = c.b;
            } else if (c.verb == "eye") {
                CameraSetEye(c.a, c.b);  // eye <lift> <forward>
            } else if (c.verb == "hand") {
                // hand <0 left|1 right> <x> <y> <z>
                const int side = c.a != 0.0f ? 1 : 0;
                g_hand_pos[side][0] = c.b, g_hand_pos[side][1] = c.c, g_hand_pos[side][2] = c.d;
            } else if (c.verb == "trigger") {
                g_trigger[c.a != 0.0f ? 1 : 0] = c.b;  // trigger <side> <0..1>
            } else if (c.verb == "hand_pitch") {
                g_hand_pitch[c.a != 0.0f ? 1 : 0] = c.b;
            } else if (c.verb == "hands") {
                g_hands_on = c.a != 0.0f;
            } else if (c.verb == "move") {
                g_offset[0] = c.a, g_offset[1] = c.b, g_offset[2] = c.c;
            } else if (c.verb == "dump") {
                g_dump_pending = true;
                g_dump_frames = 0;
                g_eyes[0].valid = g_eyes[1].valid = false;
            } else if (c.verb == "scale") {
                CameraSetWorldScale(c.a);
            } else if (c.verb == "stereo") {
                CameraSetStereoStrength(c.a);
            } else if (c.verb == "mono") {
                CameraSetStereo(false);
            } else if (c.verb == "stereo_on") {
                CameraSetStereo(true);
            } else if (c.verb == "fp") {
                CameraSetFirstPerson(c.a != 0.0f);
            }
            Log("sim: %s %.2f %.2f %.2f", c.verb.c_str(), c.a, c.b, c.c);
        }
    }

    void Quaternion(float yaw_deg, float pitch_deg, float q[4]) {
        // Yaw about +Y, then pitch about +X (OpenXR, right-handed).
        const float hy = yaw_deg * kPi / 360.0f, hp = pitch_deg * kPi / 360.0f;
        const float cy = std::cos(hy), sy = std::sin(hy), cp = std::cos(hp), sp = std::sin(hp);
        q[0] = cy * sp;   // x
        q[1] = sy * cp;   // y
        q[2] = -sy * sp;  // z
        q[3] = cy * cp;   // w
    }

    // v rotated by q.
    void Rotate(const float q[4], const float v[3], float out[3]) {
        const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]),
                    tz = 2 * (q[0] * v[1] - q[1] * v[0]);
        out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
        out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
        out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
    }
} // namespace

bool SimActive() {
    if (!g_checked) {
        g_checked = true;
        GetModuleFileNameA(nullptr, g_folder, MAX_PATH);
        char *slash = strrchr(g_folder, '\\');
        if (slash != nullptr) {
            slash[1] = '\0';
        }
        g_active = GetFileAttributesA(PathOf("lsw_vr_sim.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (g_active) {
            Log("sim: SIMULATION MODE (lsw_vr_sim.txt present): fake Rift tracking, dumps and commands");
        }
    }
    return g_active;
}

void SimTracking(VrTrackingState *t) {
    memset(t, 0, sizeof(*t));
    float q[4];
    Quaternion(g_yaw, g_pitch, q);
    const float head[3] = {g_offset[0], kHeadHeight + g_offset[1], g_offset[2]};
    memcpy(t->head.pose.orientation, q, sizeof(q));
    memcpy(t->head.pose.position, head, sizeof(head));
    const float out_t = std::tan(kOuterDeg * kPi / 180), up_t = std::tan(kUpDeg * kPi / 180),
                down_t = std::tan(kDownDeg * kPi / 180);
    t->head.fov = {-out_t, out_t, up_t, -down_t};  // union of both eyes
    for (int e = 0; e < 2; ++e) {
        const float local[3] = {e == 0 ? -kIpd / 2 : kIpd / 2, 0, 0};
        float offset[3];
        Rotate(q, local, offset);
        memcpy(t->eyes[e].orientation, q, sizeof(q));
        for (int i = 0; i < 3; ++i) {
            t->eyes[e].position[i] = head[i] + offset[i];
        }
    }
    for (int side = 0; side < 2 && g_hands_on; ++side) {
        VrHand &hand = t->hands[side];
        hand.tracked = 1;
        float hq[4];
        Quaternion(0.0f, g_hand_pitch[side], hq);
        memcpy(hand.grip.orientation, hq, sizeof(hq));
        memcpy(hand.grip.position, g_hand_pos[side], sizeof(g_hand_pos[side]));
        hand.aim = hand.grip;
        hand.trigger = g_trigger[side];
    }
    t->host_tick = GetTickCount();
}

void SimOnPresent(unsigned) {
    if (!SimActive()) {
        return;
    }
    if (g_queue.empty() && g_frames_left == 0) {
        ReadCommands();
    }
    Step();
}

unsigned SimButtons() {
    return g_buttons;
}

bool SimStick(float *x, float *y) {
    *x = g_stick[0];
    *y = g_stick[1];
    return g_has_stick;
}

void SimOnFrame(const uint8_t *pixels, uint32_t width, uint32_t height, const VrFrameMeta &meta) {
    g_kinds[g_kind_count++ % 24] = meta.has_pose;
    const bool left = meta.has_pose != kVrFrameRightEye;
    const bool right = meta.has_pose != kVrFrameLeftEye;
    for (int e = 0; e < 2; ++e) {
        if (!(e == 0 ? left : right)) {
            continue;
        }
        EyeImage &eye = g_eyes[e];
        eye.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        eye.width = width;
        eye.height = height;
        eye.meta = meta;
        eye.valid = true;
    }
    if (g_dump_pending && ++g_dump_frames >= 4 && g_eyes[0].valid && g_eyes[1].valid) {
        WriteDump();
        g_dump_pending = false;
    }
}
