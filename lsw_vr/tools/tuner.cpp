// lsw_vr_tuner.exe: a small window for dialling in the VR view live.
//
// Writes lsw_vr_tune.txt next to itself (the game folder); the game and
// lsw_vr_host.exe re-read it within a fraction of a second. The keys are
// global hotkeys (Ctrl + key), so they work with the game focused and the
// headset on; with the tuner window focused the plain keys work too.
//
//   Ctrl+Left / Ctrl+Right   image separation (degrees per eye; + = apart)
//   Ctrl+Down / Ctrl+Up      3D depth (stereo strength, 1 = real eye spacing)
//   Ctrl+PgDn / Ctrl+PgUp    world scale (game units per meter)
//   Ctrl+End / Ctrl+Home     eye height (game units above the character's neck)
//   Ctrl+M                   stereo / mono
//   Ctrl+R                   reset all
//   Ctrl+D                   save what the headset is shown (diagnostics)
//   Ctrl+F                   PC window: full screen (for recording) / normal
//   Ctrl+Space               reset the view (recenter)
//
// Started by the game with its process id; closes when the game exits.

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
    struct Tune {
        float separation = 0.0f;
        float strength = 1.0f;
        float scale = 0.3f;
        int stereo = 1;
        float height = 0.03f;
        int dump = 0;
        int fullscreen = 1;
        int recenter = 0;
    };
    Tune g_tune;
    std::string g_folder;
    HWND g_window = nullptr;
    HFONT g_big = nullptr, g_small = nullptr;
    HANDLE g_game = nullptr;
    std::string g_taken;  // shortcuts another program already owns

    enum Action { kSepDown = 1, kSepUp, kDepthDown, kDepthUp, kScaleDown, kScaleUp, kMono, kReset, kDump, kHeightDown, kHeightUp, kFullscreen, kRecenter };

    void ReadFile(const std::string &path, Tune *tune) {
        FILE *file = nullptr;
        if (fopen_s(&file, path.c_str(), "r") != 0 || file == nullptr) {
            return;
        }
        char line[128];
        while (fgets(line, sizeof(line), file) != nullptr) {
            float v = 0;
            int flag = 0;
            if (sscanf_s(line, "separation=%f", &v) == 1) tune->separation = v;
            else if (sscanf_s(line, "stereo_strength=%f", &v) == 1) tune->strength = v;
            else if (sscanf_s(line, "world_scale=%f", &v) == 1) tune->scale = v;
            else if (sscanf_s(line, "stereo=%d", &flag) == 1) tune->stereo = flag != 0;
            else if (sscanf_s(line, "dump=%d", &flag) == 1) tune->dump = flag;
            else if (sscanf_s(line, "eye_lift=%f", &v) == 1) tune->height = v;
            else if (sscanf_s(line, "pc_fullscreen=%d", &flag) == 1) tune->fullscreen = flag != 0;
            else if (sscanf_s(line, "recenter=%d", &flag) == 1) tune->recenter = flag;
        }
        fclose(file);
    }

    void Save() {
        FILE *file = nullptr;
        const std::string path = g_folder + "lsw_vr_tune.txt";
        if (fopen_s(&file, path.c_str(), "w") != 0 || file == nullptr) {
            return;
        }
        fprintf(file, "separation=%.2f\nstereo_strength=%.2f\nworld_scale=%.3f\nstereo=%d\neye_lift=%.3f\ndump=%d\npc_fullscreen=%d\nrecenter=%d\n",
                g_tune.separation, g_tune.strength, g_tune.scale, g_tune.stereo, g_tune.height, g_tune.dump,
                g_tune.fullscreen, g_tune.recenter);
        fclose(file);
    }

    float Clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    void Apply(int action) {
        switch (action) {
        case kSepDown: g_tune.separation = Clamp(g_tune.separation - 0.25f, -10.0f, 10.0f); break;
        case kSepUp: g_tune.separation = Clamp(g_tune.separation + 0.25f, -10.0f, 10.0f); break;
        case kDepthDown: g_tune.strength = Clamp(g_tune.strength - 0.1f, 0.0f, 3.0f); break;
        case kDepthUp: g_tune.strength = Clamp(g_tune.strength + 0.1f, 0.0f, 3.0f); break;
        case kScaleDown: g_tune.scale = Clamp(g_tune.scale - 0.02f, 0.04f, 3.0f); break;
        case kScaleUp: g_tune.scale = Clamp(g_tune.scale + 0.02f, 0.04f, 3.0f); break;
        case kMono: g_tune.stereo = !g_tune.stereo; break;
        case kReset: {
            const Tune old = g_tune;
            g_tune = Tune();
            g_tune.dump = old.dump;
            g_tune.fullscreen = old.fullscreen;
            g_tune.recenter = old.recenter;
            break;
        }
        case kFullscreen: g_tune.fullscreen = !g_tune.fullscreen; break;
        case kRecenter: ++g_tune.recenter; break;
        case kDump: ++g_tune.dump; break;
        case kHeightDown: g_tune.height = Clamp(g_tune.height - 0.02f, -0.4f, 0.9f); break;
        case kHeightUp: g_tune.height = Clamp(g_tune.height + 0.02f, -0.4f, 0.9f); break;
        default: return;
        }
        // Avoid "-0.00".
        if (std::fabs(g_tune.separation) < 0.001f) g_tune.separation = 0.0f;
        Save();
        MessageBeep(MB_OK);  // feedback with the headset on
        InvalidateRect(g_window, nullptr, TRUE);
    }

    int ActionForKey(WPARAM key) {
        switch (key) {
        case VK_LEFT: return kSepDown;
        case VK_RIGHT: return kSepUp;
        case VK_DOWN: return kDepthDown;
        case VK_UP: return kDepthUp;
        case VK_NEXT: return kScaleDown;
        case VK_PRIOR: return kScaleUp;
        case 'M': return kMono;
        case 'R': return kReset;
        case 'D': return kDump;
        case VK_END: return kHeightDown;
        case VK_HOME: return kHeightUp;
        case 'F': return kFullscreen;
        case VK_SPACE: return kRecenter;
        default: return 0;
        }
    }

    void Paint(HWND window) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT client;
        GetClientRect(window, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(dc, TRANSPARENT);
        char text[256];
        const struct {
            const char *name;
            const char *keys;
        } rows[] = {
            {"Separation", "Ctrl+Left/Right"},
            {"Depth", "Ctrl+Down/Up"},
            {"World scale", "Ctrl+PgDn/PgUp"},
            {"Mode", "Ctrl+M"},
            {"Eye height", "Ctrl+End/Home"},
        };
        char values[5][32];
        sprintf_s(values[0], "%+.2f deg", g_tune.separation);
        sprintf_s(values[1], "%.1f", g_tune.strength);
        sprintf_s(values[2], "%.2f", g_tune.scale);
        sprintf_s(values[3], "%s", g_tune.stereo ? "STEREO" : "MONO");
        sprintf_s(values[4], "%+.2f", g_tune.height);
        int y = 16;
        for (int i = 0; i < 5; ++i) {
            SelectObject(dc, g_small);
            SetTextColor(dc, RGB(150, 150, 150));
            sprintf_s(text, "%s   (%s)", rows[i].name, rows[i].keys);
            TextOutA(dc, 20, y, text, static_cast<int>(strlen(text)));
            SelectObject(dc, g_big);
            SetTextColor(dc, RGB(255, 220, 80));
            TextOutA(dc, 20, y + 22, values[i], static_cast<int>(strlen(values[i])));
            y += 84;
        }
        SelectObject(dc, g_small);
        SetTextColor(dc, RGB(150, 150, 150));
        const char *footer = "Ctrl+Space recenter   Ctrl+F PC full screen";
        TextOutA(dc, 20, y, footer, static_cast<int>(strlen(footer)));
        y += 24;
        footer = "Ctrl+R reset, Ctrl+D snapshot. Beep = applied.";
        if (!g_taken.empty()) {
            const std::string taken = "In use by another program: " + g_taken;
            SetTextColor(dc, RGB(255, 90, 90));
            TextOutA(dc, 20, y + 24, taken.c_str(), static_cast<int>(taken.size()));
            SetTextColor(dc, RGB(150, 150, 150));
        }
        TextOutA(dc, 20, y, footer, static_cast<int>(strlen(footer)));
        EndPaint(window, &ps);
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_HOTKEY: Apply(static_cast<int>(wparam)); return 0;
        case WM_TIMER:
            // The game this tuner belongs to has exited.
            if (g_game != nullptr && WaitForSingleObject(g_game, 0) == WAIT_OBJECT_0) {
                DestroyWindow(window);
            }
            return 0;
        case WM_KEYDOWN: Apply(ActionForKey(wparam)); return 0;
        case WM_PAINT: Paint(window); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: return DefWindowProcA(window, message, wparam, lparam);
        }
    }
} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR command_line, int) {
    // One tuner at a time (the game starts it too).
    CreateMutexA(nullptr, FALSE, "Local\\lsw_vr_tuner");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }
    SetProcessDPIAware();
    const unsigned long game_pid = strtoul(command_line, nullptr, 10);
    if (game_pid != 0) {
        g_game = OpenProcess(SYNCHRONIZE, FALSE, game_pid);
    }
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    g_folder = path;
    g_folder.resize(g_folder.find_last_of('\\') + 1);
    // Start from the game's saved settings, then any earlier tuning.
    ReadFile(g_folder + "lsw_vr_settings.txt", &g_tune);
    ReadFile(g_folder + "lsw_vr_tune.txt", &g_tune);

    g_big = CreateFontA(-44, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    g_small = CreateFontA(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "LswVrTuner";
    RegisterClassA(&wc);
    g_window = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, "LSW VR Tuner",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 40, 40, 440, 600, nullptr,
                               nullptr, instance, nullptr);
    const struct {
        int id;
        UINT key;
    } hotkeys[] = {{kSepDown, VK_LEFT},   {kSepUp, VK_RIGHT},  {kDepthDown, VK_DOWN}, {kDepthUp, VK_UP},
                   {kScaleDown, VK_NEXT}, {kScaleUp, VK_PRIOR}, {kMono, 'M'},          {kReset, 'R'},
                   {kDump, 'D'},         {kHeightDown, VK_END}, {kHeightUp, VK_HOME},
                   {kFullscreen, 'F'},  {kRecenter, VK_SPACE}};
    for (const auto &hotkey : hotkeys) {
        if (!RegisterHotKey(g_window, hotkey.id, MOD_CONTROL | MOD_NOREPEAT, hotkey.key)) {
            char name[32] = {};
            GetKeyNameTextA(static_cast<LONG>(MapVirtualKeyA(hotkey.key, MAPVK_VK_TO_VSC)) << 16, name, sizeof(name));
            g_taken += g_taken.empty() ? "" : ", ";
            g_taken += std::string("Ctrl+") + name;
        }
    }
    Save();
    SetTimer(g_window, 1, 1000, nullptr);
    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
