// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
// Cinematics hotkeys - see hotkeys.h.
#include "hotkeys.h"
#include "campath.h"
#include "pov.h"
#include "record.h"
#include "skins.h"
#include "util.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace Hotkeys {

static std::mutex g_lock;
static Binding g_keys[Count];
static bool g_down[Count];
static int g_capture = -1;
static bool g_captureArmed = false;   // all keys were up once since capture started

static const struct { const char* id; const char* name; const char* help; Binding def; } kInfo[Count] = {
    {"seq_add", "Add point", "Sequence: the current view becomes the next point (the demo stays paused).", {VK_F6}},
    {"seq_remove", "Remove point", "Sequence: removes the selected point (the last one added).", {VK_F7}},
    {"seq_clear", "Clear points", "Sequence: removes every point.", {VK_F7, false, true}},
    {"seq_generate", "Generate", "Sequence: makes the real path from the points and switches it on.", {VK_F8}},
    {"seq_generate_preview", "Generate + preview", "Sequence: generates and flies the path once in real time.", {VK_F8, false, true}},
    {"seq_start_here", "Start at this tick", "Sequence: the path starts at the tick the demo is on now.", {}},
    {"preview", "Preview / stop", "Flies the path once in real time (again to stop).", {VK_F9}},
    {"path_toggle", "Play path on/off", "Switches \"Play the path in the demo\".", {VK_F10}},
    {"go_to_start", "Go to start", "Jumps the demo to one second before the path starts.", {VK_F11}},
    {"path_add_key", "Add key (path keys)", "Path keys: the current view at the current demo time.", {}},
    {"undo", "Undo", "Undoes the last change to the path.", {}},
    {"pov_toggle", "POV HUD on/off", "DemoTools2's POV HUD.", {}},
    {"record_path", "Record path / stop", "Records the path to MP4 (same video settings as the Record tab); again to stop.", {}},
};

const char* Name(Action a) { return a >= 0 && a < Count ? kInfo[a].name : "?"; }
const char* Help(Action a) { return a >= 0 && a < Count ? kInfo[a].help : ""; }

static std::string File() { return DllFolder() + "DemoTools2-hotkeys.cfg"; }

static void Save() {
    std::ofstream f(File());
    if (!f) return;
    f << "# DemoTools2 cinematics hotkeys: action=key ctrl shift alt (key = Windows virtual-key code, 0 = none)\n";
    for (int i = 0; i < Count; ++i)
        f << kInfo[i].id << "=" << g_keys[i].vk << " " << (g_keys[i].ctrl ? 1 : 0) << " " << (g_keys[i].shift ? 1 : 0) << " " << (g_keys[i].alt ? 1 : 0) << "\n";
}

void Init() {
    std::lock_guard<std::mutex> g(g_lock);
    for (int i = 0; i < Count; ++i) g_keys[i] = kInfo[i].def;
    std::ifstream f(File());
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string id = line.substr(0, eq);
        std::istringstream v(line.substr(eq + 1));
        int vk = 0, c = 0, s = 0, al = 0;
        v >> vk >> c >> s >> al;
        for (int i = 0; i < Count; ++i)
            if (id == kInfo[i].id) g_keys[i] = {vk, c != 0, s != 0, al != 0};
    }
}

Binding Get(Action a) {
    std::lock_guard<std::mutex> g(g_lock);
    return a >= 0 && a < Count ? g_keys[a] : Binding{};
}
void Set(Action a, const Binding& b) {
    std::lock_guard<std::mutex> g(g_lock);
    if (a < 0 || a >= Count) return;
    // one key, one action: take it away from any other action
    if (b.vk)
        for (int i = 0; i < Count; ++i)
            if (i != a && g_keys[i].vk == b.vk && g_keys[i].ctrl == b.ctrl && g_keys[i].shift == b.shift && g_keys[i].alt == b.alt) g_keys[i] = {};
    g_keys[a] = b;
    Save();
}

void ResetDefaults() {
    std::lock_guard<std::mutex> g(g_lock);
    for (int i = 0; i < Count; ++i) g_keys[i] = kInfo[i].def;
    Save();
}

std::string KeyText(const Binding& b) {
    if (!b.vk) return "-";
    std::string s;
    if (b.ctrl) s += "Ctrl+";
    if (b.shift) s += "Shift+";
    if (b.alt) s += "Alt+";
    switch (b.vk) {
    case VK_LBUTTON: return s + "Mouse 1";
    case VK_RBUTTON: return s + "Mouse 2";
    case VK_MBUTTON: return s + "Mouse 3";
    case VK_XBUTTON1: return s + "Mouse 4";
    case VK_XBUTTON2: return s + "Mouse 5";
    }
    UINT sc = MapVirtualKeyA((UINT)b.vk, MAPVK_VK_TO_VSC);
    switch (b.vk) {   // keys Windows names wrongly without the "extended" bit
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        sc |= 0x100;
    }
    char name[64] = {};
    if (sc && GetKeyNameTextA((LONG)(sc << 16), name, sizeof name) > 0) return s + name;
    char b2[16];
    std::snprintf(b2, sizeof b2, "key %d", b.vk);
    return s + b2;
}

static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static bool Mods(const Binding& b) {
    return Down(VK_CONTROL) == b.ctrl && Down(VK_SHIFT) == b.shift && Down(VK_MENU) == b.alt;
}

// a short note in CS2's console (on the game thread)
static void Note(const std::string& s) {
    std::string t = s;
    for (char& c : t) if (c == '"' || c == ';') c = ' ';
    Campath::QueueCommand("echo \"[DemoTools2] " + t + "\"");
}

static void Run(Action a) {
    std::string why;
    switch (a) {
    case SeqAdd:
        if (Campath::SeqAdd(&why)) {
            size_t n;
            { std::lock_guard<std::mutex> g(Campath::lock); n = Campath::seq.points.size(); }
            Note("point " + std::to_string(n) + " added");
        } else Note(why);
        break;
    case SeqRemoveLast: {
        int i;
        { std::lock_guard<std::mutex> g(Campath::lock); i = Campath::seq.selected >= 0 ? Campath::seq.selected : (int)Campath::seq.points.size() - 1; }
        if (i >= 0) { Campath::SeqRemove(i); Note("point " + std::to_string(i + 1) + " removed"); }
        break;
    }
    case SeqClear: Campath::SeqClear(); Note("points cleared"); break;
    case SeqGenerate:
        if (Campath::SeqGenerate(&why)) Note("path generated and on"); else Note(why);
        break;
    case SeqGeneratePreview:
        if (Campath::SeqGenerate(&why)) { Campath::Preview(); Note("path generated - preview"); } else Note(why);
        break;
    case SeqStartHere:
        if (Campath::SeqUseCurrentStart(&why)) Note("the path starts here"); else Note(why);
        break;
    case Preview:
        if (Campath::Status().previewing) { Campath::StopPreview(); Note("preview stopped"); }
        else { Campath::Preview(); Note("preview"); }
        break;
    case PathToggle: {
        bool on;
        { std::lock_guard<std::mutex> g(Campath::lock); on = Campath::set.enabled = !Campath::set.enabled; }
        Note(on ? "path on" : "path off");
        break;
    }
    case GoToStart: {
        double t0 = -1;
        { std::lock_guard<std::mutex> g(Campath::lock); if (!Campath::keys.empty()) t0 = Campath::keys.front().t; }
        const int tick = t0 >= 0 ? Campath::TickFor(t0) : -1;
        if (tick >= 0) { Campath::QueueCommand("demo_gototick " + std::to_string(std::max(0, tick - 64))); Note("going to the start"); }
        else Note("no path start known yet");
        break;
    }
    case PathAddKey:
        if (Campath::AddKeyHere(&why)) Note("key added"); else Note(why);
        break;
    case Undo: Note(Campath::Undo() ? "undone" : "nothing to undo"); break;
    case PovToggle: {
        const bool on = Pov::Mode() == 0;
        Pov::SetMode(on ? 1 : 0);
        Note(on ? "POV HUD on" : "POV HUD off");
        break;
    }
    case RecordPath: {
        if (Record::Status().running) { Record::Stop(); Note("recording stopped"); break; }
        double t0 = -1, t1 = -1;
        {
            std::lock_guard<std::mutex> g(Campath::lock);
            if (Campath::keys.size() >= 2) { t0 = Campath::keys.front().t; t1 = Campath::keys.back().t; Campath::set.enabled = true; }
        }
        const int a0 = t0 >= 0 ? Campath::TickFor(t0) : -1, a1 = t1 >= 0 ? Campath::TickFor(t1) : -1;
        if (a0 < 0 || a1 <= a0) { Note("no path to record (or its ticks aren't known)"); break; }
        SYSTEMTIME st;
        GetLocalTime(&st);
        char name[64];
        std::snprintf(name, sizeof name, "cinematic_%02d%02d%02d", st.wHour, st.wMinute, st.wSecond);
        Campath::StopPreview();
        Record::StartCinematic(a0, a1, name);
        Note("recording the path");
        break;
    }
    default: break;
    }
}

void Poll(bool gameInFront) {
    Binding keys[Count];
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_capture >= 0) return;   // the window is waiting for a new key
        std::memcpy(keys, g_keys, sizeof keys);
    }
    for (int i = 0; i < Count; ++i) {
        const bool down = gameInFront && keys[i].vk && keys[i].vk != Skins::settings.menuKey && Down(keys[i].vk) && Mods(keys[i]);
        if (down && !g_down[i]) Run((Action)i);
        g_down[i] = down;
    }
}

void BeginCapture(Action a) {
    std::lock_guard<std::mutex> g(g_lock);
    g_capture = a;
    g_captureArmed = false;
}
bool Capturing(Action& a) {
    std::lock_guard<std::mutex> g(g_lock);
    if (g_capture < 0) return false;
    a = (Action)g_capture;
    return true;
}
bool Capture() {
    int a;
    {
        std::lock_guard<std::mutex> g(g_lock);
        a = g_capture;
    }
    if (a < 0) return true;
    // wait until nothing is held (the click that started it), then take the first key that goes down
    bool any = false;
    for (int vk = 1; vk < 0xFF; ++vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN) continue;
        if (!Down(vk)) continue;
        any = true;
        bool armed;
        {
            std::lock_guard<std::mutex> g(g_lock);
            armed = g_captureArmed;
        }
        if (!armed) continue;
        if (vk == VK_LBUTTON || vk == VK_RBUTTON) continue;   // clicking around the window doesn't bind
        Binding b;
        if (vk == VK_ESCAPE) { std::lock_guard<std::mutex> g(g_lock); g_capture = -1; return true; }
        if (vk == VK_BACK) { b = {}; }
        else if (vk == Skins::settings.menuKey) continue;   // the menu key stays the menu key
        else b = {vk, Down(VK_CONTROL), Down(VK_SHIFT), Down(VK_MENU)};
        Set((Action)a, b);
        std::lock_guard<std::mutex> g(g_lock);
        g_capture = -1;
        std::memset(g_down, 1, sizeof g_down);   // the key just pressed doesn't also fire its action
        return true;
    }
    if (!any) { std::lock_guard<std::mutex> g(g_lock); g_captureArmed = true; }
    return false;
}

}  // namespace Hotkeys
