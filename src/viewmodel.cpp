// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
// DemoTools2's own viewmodel position and FOV - see viewmodel.h.
//
// How the function is found (no fixed code pattern, so a CS2 update that only renumbers things doesn't break it):
//  1. CS2's schema gives the four fields by name: C_CSPlayerPawn::m_flViewmodelOffsetX/Y/Z and m_flViewmodelFOV.
//  2. The function is the one place in client.dll that loads all four, in that order, within a few instructions.
//  3. Its start is the first 16-byte boundary after the padding (int3) before it; its calls are the "call rel32"
//     instructions that land there.
// Each call is pointed at a small jump (allocated next to client.dll) to Hook(), which calls the real function first.
#include "viewmodel.h"
#include "console.h"
#include "game.h"
#include "pov.h"
#include "util.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <vector>

namespace Viewmodel {

const Preset kPresets[] = {
    {"CS2 default", 1.f, 1.f, -1.f, 60.f},
    {"Classic", 2.5f, 0.f, -1.5f, 60.f},
    {"Wide", 2.5f, 2.f, -2.f, 68.f},
    {"Centered", 0.f, 0.f, -1.5f, 60.f},
};
const int kNumPresets = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

void Limits(bool extended, float lo[4], float hi[4]) {
    if (extended) {
        const float l[4] = {-10, -10, -10, 40}, h[4] = {10, 10, 10, 120};
        std::copy(l, l + 4, lo);
        std::copy(h, h + 4, hi);
    } else {   // CS2's own limits (from the function: x -2..2.5, y -2..2, z -2..2, fov 60..68)
        const float l[4] = {-2, -2, -2, 60}, h[4] = {2.5f, 2, 2, 68};
        std::copy(l, l + 4, lo);
        std::copy(h, h + 4, hi);
    }
}

// Read by the game thread in Hook(), written by the window: plain atomics, no lock in the game's frame.
static std::atomic<bool> g_custom{false}, g_extended{false};
static std::atomic<float> g_x{1.f}, g_y{1.f}, g_z{-1.f}, g_fov{60.f};
static std::mutex g_statusLock;
static std::string g_status = "not set up yet";
static bool g_available = false;

using Fn = void (*)(void* pawn, float* xyz, float* fov);
static Fn g_real = nullptr;
static ptrdiff_t g_fx = 0, g_fy = 0, g_fz = 0, g_ffov = 0;
static std::atomic<int> g_calls{0};

static void SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> g(g_statusLock);
    g_status = s;
}

static bool Finite4(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::isfinite(v[3]); }

static void Hook(void* pawn, float* xyz, float* fov) {
    g_real(pawn, xyz, fov);
    if (!xyz || !fov) return;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_custom.load(std::memory_order_relaxed)) {
        float lo[4], hi[4];
        Limits(g_extended.load(std::memory_order_relaxed), lo, hi);
        const float v[4] = {g_x.load(), g_y.load(), g_z.load(), g_fov.load()};
        if (!Finite4(v)) return;
        for (int i = 0; i < 3; ++i) xyz[i] = std::clamp(v[i], lo[i], hi[i]);
        *fov = std::clamp(v[3], lo[3], hi[3]);
        return;
    }
    // The demo's own position. Outside the POV HUD's swap CS2 already answered with the player's own values; during
    // it, CS2 thinks the watched player is you and took your settings - answer with the player's own instead, the
    // way CS2 does for them (same limits), so the gun doesn't jump.
    if (!pawn || !Pov::SwapActive()) return;
    uint8_t* p = (uint8_t*)pawn;
    if (!Readable(p + g_fx, 4) || !Readable(p + g_ffov, 4)) return;
    const float v[4] = {::Get<float>(p, g_fx), ::Get<float>(p, g_fy), ::Get<float>(p, g_fz), ::Get<float>(p, g_ffov)};
    if (!Finite4(v)) return;
    float lo[4], hi[4];
    Limits(false, lo, hi);
    for (int i = 0; i < 3; ++i) xyz[i] = std::clamp(v[i], lo[i], hi[i]);
    *fov = std::clamp(v[3], lo[3], hi[3]);
}

// ------------------------------------------------------------------ finding the function
static bool IsLoad(const uint8_t* disp) {   // disp32 of "mov r32,[r64+disp32]" or "movss xmm,[r64+disp32]"
    const uint8_t modrm = disp[-1], op = disp[-2];
    if ((modrm >> 6) != 2 || (modrm & 7) == 4) return false;   // [reg+disp32], no SIB
    return op == 0x8B || (op == 0x10 && disp[-3] == 0x0F);
}

static uint8_t* FindFunction(uint8_t* code, size_t size) {
    int32_t want[4] = {(int32_t)g_fx, (int32_t)g_fy, (int32_t)g_fz, (int32_t)g_ffov};
    std::vector<uint8_t*> found;
    for (size_t i = 3; i + 64 + 4 <= size; ++i) {
        if (std::memcmp(code + i, &want[0], 4) != 0 || !IsLoad(code + i)) continue;
        size_t at = i;
        bool ok = true;
        for (int k = 1; k < 4 && ok; ++k) {
            ok = false;
            for (size_t j = at + 4; j < i + 64; ++j)
                if (!std::memcmp(code + j, &want[k], 4) && IsLoad(code + j)) { at = j; ok = true; break; }
        }
        if (ok) found.push_back(code + i);
    }
    if (found.size() != 1) {
        Log("viewmodel: %zu place(s) load the four viewmodel fields (1 expected) - viewmodel is off", found.size());
        return nullptr;
    }
    // walk back to the start: a 16-byte boundary right after int3 padding
    uint8_t* hit = found[0];
    for (uint8_t* s = hit; s > hit - 0x400 && s > code; --s)
        if (((uintptr_t)s & 15) == 0 && s[-1] == 0xCC) return s;
    Log("viewmodel: start of the function not found");
    return nullptr;
}

// Memory for the jump, within 2 GB of client.dll (a call rel32 has to reach it).
static uint8_t* AllocNear(uint8_t* target) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t lo = (uintptr_t)target > 0x70000000 ? (uintptr_t)target - 0x70000000 : gran;
    const uintptr_t hi = (uintptr_t)target + 0x70000000;
    for (uintptr_t a = ((uintptr_t)target & ~(gran - 1)) - gran; a > lo; a -= gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) continue;
        if (mbi.State != MEM_FREE) { a = ((uintptr_t)mbi.AllocationBase & ~(gran - 1)); if (!a) break; continue; }
        if (void* p = VirtualAlloc((void*)a, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return (uint8_t*)p;
    }
    for (uintptr_t a = ((uintptr_t)target & ~(gran - 1)) + gran; a < hi; a += gran) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) continue;
        if (mbi.State != MEM_FREE) { a = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize - 1) & ~(gran - 1); continue; }
        if (void* p = VirtualAlloc((void*)a, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return (uint8_t*)p;
    }
    return nullptr;
}

static bool Install() {
    auto field = [](const char* name, ptrdiff_t& out) { return Game::SchemaField("C_CSPlayerPawn", name, out) && out > 0; };
    if (!field("m_flViewmodelOffsetX", g_fx) || !field("m_flViewmodelOffsetY", g_fy) || !field("m_flViewmodelOffsetZ", g_fz) ||
        !field("m_flViewmodelFOV", g_ffov)) {
        Log("viewmodel: the viewmodel fields aren't in this CS2 build's schema");
        SetStatus("not available: CS2's viewmodel fields weren't found");
        return false;
    }
    uint8_t* code = nullptr;
    size_t size = 0;
    if (!Game::ClientCode(code, size)) return false;
    uint8_t* fn = FindFunction(code, size);
    if (!fn) { SetStatus("not available in this CS2 build (its viewmodel code changed) - see DemoTools2.log"); return false; }
    std::vector<uint8_t*> calls;
    for (size_t i = 0; i + 5 <= size; ++i)
        if (code[i] == 0xE8 && code + i + 5 + *(int32_t*)(code + i + 1) == fn) calls.push_back(code + i);
    if (calls.empty() || calls.size() > 8) {
        Log("viewmodel: function at client.dll+0x%llx has %zu call(s) - viewmodel is off", (unsigned long long)(fn - code), calls.size());
        SetStatus("not available in this CS2 build - see DemoTools2.log");
        return false;
    }
    uint8_t* jump = AllocNear(fn);
    if (!jump) { SetStatus("not available: no memory next to client.dll"); return false; }
    {   // jmp [rip+0]; dq Hook
        const uint8_t j[6] = {0xFF, 0x25, 0, 0, 0, 0};
        void* target = (void*)&Hook;
        std::memcpy(jump, j, 6);
        std::memcpy(jump + 6, &target, 8);
        FlushInstructionCache(GetCurrentProcess(), jump, 14);
    }
    g_real = (Fn)fn;
    int patched = 0;
    for (uint8_t* c : calls) {
        const int64_t rel = (int64_t)(jump - (c + 5));
        if (rel < INT32_MIN || rel > INT32_MAX) continue;
        const int32_t r = (int32_t)rel;
        DWORD old;
        if (!VirtualProtect(c + 1, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        std::memcpy(c + 1, &r, 4);   // only the call's target changes
        VirtualProtect(c + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), c, 5);
        ++patched;
    }
    Log("viewmodel: function at client.dll+0x%llx (fields 0x%x 0x%x 0x%x 0x%x), %d of %zu call(s) redirected",
        (unsigned long long)(fn - (uint8_t*)GetModuleHandleA("client.dll")), (unsigned)g_fx, (unsigned)g_fy, (unsigned)g_fz,
        (unsigned)g_ffov, patched, calls.size());
    return patched > 0;
}

// ------------------------------------------------------------------ settings
static std::string CfgPath() { return DllFolder() + "DemoTools2-viewmodel.cfg"; }

Settings Get() {
    Settings s;
    s.custom = g_custom.load();
    s.extended = g_extended.load();
    s.x = g_x.load(); s.y = g_y.load(); s.z = g_z.load(); s.fov = g_fov.load();
    return s;
}

static void Save() {
    const Settings s = Get();
    std::ofstream f(CfgPath());
    if (!f) return;
    f << "# DemoTools2 viewmodel (first-person gun). Written by DemoTools2.\n";
    f << "custom=" << (s.custom ? 1 : 0) << "\nextended=" << (s.extended ? 1 : 0) << "\n";
    f << "x=" << s.x << "\ny=" << s.y << "\nz=" << s.z << "\nfov=" << s.fov << "\n";
}

void Set(const Settings& in) {
    Settings s = in;
    float lo[4], hi[4];
    Limits(s.extended, lo, hi);
    float* v[4] = {&s.x, &s.y, &s.z, &s.fov};
    for (int i = 0; i < 4; ++i) *v[i] = std::isfinite(*v[i]) ? std::clamp(*v[i], lo[i], hi[i]) : lo[i];
    g_x = s.x; g_y = s.y; g_z = s.z; g_fov = s.fov;
    g_extended = s.extended;
    g_custom = s.custom;
    Save();
}

static void Load() {
    std::ifstream f(CfgPath());
    if (!f) return;
    Settings s;
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const float v = (float)std::atof(line.c_str() + eq + 1);
        if (k == "custom") s.custom = v != 0;
        else if (k == "extended") s.extended = v != 0;
        else if (k == "x") s.x = v;
        else if (k == "y") s.y = v;
        else if (k == "z") s.z = v;
        else if (k == "fov") s.fov = v;
    }
    float lo[4], hi[4];
    Limits(s.extended, lo, hi);
    g_x = std::clamp(s.x, lo[0], hi[0]); g_y = std::clamp(s.y, lo[1], hi[1]);
    g_z = std::clamp(s.z, lo[2], hi[2]); g_fov = std::clamp(s.fov, lo[3], hi[3]);
    g_extended = s.extended;
    g_custom = s.custom;
}

bool Available() { return g_available; }
std::string Status() {
    std::lock_guard<std::mutex> g(g_statusLock);
    if (!g_available) return g_status;
    const Settings s = Get();
    char b[160];
    if (s.custom) std::snprintf(b, sizeof b, "on - %.1f / %.1f / %.1f, FOV %.0f", s.x, s.y, s.z, s.fov);
    else std::snprintf(b, sizeof b, "off - each player's own, as in the demo");
    return b;
}

// ------------------------------------------------------------------ console
static void Cmd(const Console::Args& a) {
    Settings s = Get();
    if (a.size() < 2) {
        Console::Print("dt2_viewmodel - %s", Status().c_str());
        Console::Print("  dt2_viewmodel 0|1                   off (demo's own) / on (custom)");
        Console::Print("  dt2_viewmodel <x> <y> <z> [fov]     set and switch on (x right, y forward, z up)");
        Console::Print("  dt2_viewmodel preset <name>         CS2 default | Classic | Wide | Centered");
        Console::Print("  dt2_viewmodel extended 0|1          allow values past CS2's limits");
        return;
    }
    if (!g_available) { Console::Print("dt2_viewmodel: %s", Status().c_str()); return; }
    if (a[1] == "extended" && a.size() > 2) {
        s.extended = std::atoi(a[2].c_str()) != 0;
    } else if (a[1] == "preset" && a.size() > 2) {
        std::string want;
        for (size_t i = 2; i < a.size(); ++i) want += (i > 2 ? " " : "") + a[i];
        bool ok = false;
        for (int i = 0; i < kNumPresets; ++i)
            if (!_stricmp(kPresets[i].name, want.c_str())) {
                s.x = kPresets[i].x; s.y = kPresets[i].y; s.z = kPresets[i].z; s.fov = kPresets[i].fov;
                s.custom = ok = true;
            }
        if (!ok) { Console::Print("dt2_viewmodel: no preset '%s'", want.c_str()); return; }
    } else if (a.size() >= 4) {
        s.x = (float)std::atof(a[1].c_str());
        s.y = (float)std::atof(a[2].c_str());
        s.z = (float)std::atof(a[3].c_str());
        if (a.size() >= 5) s.fov = (float)std::atof(a[4].c_str());
        s.custom = true;
    } else {
        s.custom = std::atoi(a[1].c_str()) != 0;
    }
    Set(s);
    Console::Print("dt2_viewmodel - %s", Status().c_str());
}

void Init() {
    Load();
    Console::Add("dt2_viewmodel", "DemoTools2's viewmodel position and FOV: dt2_viewmodel 0|1 | <x> <y> <z> [fov] | preset <name>", &Cmd);
    if (CrashedLastTime("viewmodel")) {
        Log("viewmodel: off this session (CS2 crashed while it was being set up last time)");
        SetStatus("crashed last time - off for this session");
        SetFeature("viewmodel", "Viewmodel", Feat_Off, "Crashed last time - off for this session.");
        return;
    }
    Step step("viewmodel");
    FirstRunBegin("viewmodel");
    g_available = Install();
    FirstRunEnd("viewmodel");
    if (g_available) {
        SetStatus("ready");
        SetFeature("viewmodel", "Viewmodel", Feat_Ok);
    } else {
        SetFeature("viewmodel", "Viewmodel", Feat_Off, Status());
    }
}

}  // namespace Viewmodel
