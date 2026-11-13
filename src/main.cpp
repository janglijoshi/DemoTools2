// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
// DemoTools2 - CS2 demo tools for HLAE: skins for chosen players, and calcs + a camera that follows them. Load it in HLAE:
//   mirv_loadlibrary "C:\path\to\DemoTools2.dll"
#include "calc.h"
#include "game.h"
#include "render.h"
#include "skins.h"
#include "util.h"

static HMODULE g_self = nullptr;

// FrameStageNotify(this, stage). Declared with the four register arguments and a return value so the hook passes
// everything through untouched, whatever the real signature is.
using FrameStageFn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t);
static FrameStageFn g_oFrameStage = nullptr;
static bool g_skinsOff = false;

// Is the function we hooked really FrameStageNotify? It is called several times a frame with small stage numbers
// (-1..15), going through several different ones. Nothing runs until the first 300 calls look like that; if they
// don't (a CS2 update moved the function and the guess was wrong), DemoTools2 only passes the calls through.
static int g_hookCalls = 0, g_hookState = 0;   // 0 checking, 1 confirmed, -1 not FrameStageNotify
static uint32_t g_stagesSeen = 0;
static bool HookConfirmed(uintptr_t stage) {
    if (g_hookState) return g_hookState > 0;
    const int st = (int)(uint32_t)stage;
    if (st < -1 || st > 15) {
        g_hookState = -1;
        Log("ERROR: the hooked function isn't FrameStageNotify in this CS2 build (got stage %d) - skins and camera are off. "
            "Send DemoTools2.log to the developer.", st);
        SetFeature("hook", "Game hook", Feat_Off, "CS2 update: the frame function moved and wasn't recognised. Skins and camera are off.");
        return false;
    }
    if (st >= 0) g_stagesSeen |= 1u << st;
    if (++g_hookCalls < 300) return false;
    int distinct = 0;
    for (uint32_t m = g_stagesSeen; m; m &= m - 1) ++distinct;
    if (distinct >= 3) {
        g_hookState = 1;
        Log("FrameStageNotify confirmed (%d different stages seen)", distinct);
        SetFeature("hook", "Game hook", Feat_Ok);
    } else {
        g_hookState = -1;
        Log("ERROR: the hooked function doesn't behave like FrameStageNotify (%d stage(s) seen) - skins and camera are off", distinct);
        SetFeature("hook", "Game hook", Feat_Off, "CS2 update: the frame function moved and wasn't recognised. Skins and camera are off.");
    }
    return g_hookState > 0;
}

static uintptr_t hkFrameStage(void* self, uintptr_t stage, uintptr_t a3, uintptr_t a4) {
    if (!HookConfirmed(stage)) return g_oFrameStage(self, stage, a3, a4);
    {
        Step step("camera set-up");
        Calc::OnGameThread();
    }
    if (!g_skinsOff) {
        Step step("applying skins (FrameStageNotify)");
        FirstRunBegin("frame-stage");
        Skins::OnFrameStage((int)stage);
        FirstRunEnd("frame-stage");
    }
    return g_oFrameStage(self, stage, a3, a4);
}

// The FrameStageNotify body starts with a switch over the frame stages: the stage minus 3 (add -3 / sub 3 / lea -3),
// then a compare with the number of cases. Find the slot whose code looks like that, so a CS2 update that moves the
// function doesn't send us into the wrong one. Whatever is picked is still checked at run time (HookConfirmed).
static bool LooksLikeStageSwitch(const uint8_t* fn) {
    for (int k = 0; k < 0xF0 - 20; ++k) {
        const uint8_t* c = fn + k;
        if (c[0] >= 0x40 && c[0] <= 0x4F) ++c;   // REX prefix
        int reg = -1;
        if (c[0] == 0x83 && (c[1] & 0xF8) == 0xC0 && c[2] == 0xFD) reg = c[1] & 7;                 // add r32, -3
        else if (c[0] == 0x83 && (c[1] & 0xF8) == 0xE8 && c[2] == 0x03) reg = c[1] & 7;            // sub r32, 3
        else if (c[0] == 0x8D && (c[1] & 0xC0) == 0x40 && (c[1] & 7) != 4 && c[2] == 0xFD) reg = (c[1] >> 3) & 7;   // lea r32, [r-3]
        if (reg < 0) continue;
        for (const uint8_t* j = c + 3; j < c + 20; ++j)
            if (j[0] == 0x83 && j[1] == (0xF8 | reg) && j[2] >= 4 && j[2] <= 14) return true;   // cmp r32, cases
    }
    return false;
}
static int FindFrameStageIndex(void** vt, int preferred) {
    int found = -1, hits = 0;
    bool preferredHit = false;
    for (int i = 0; i < 90; ++i) {
        uint8_t* fn = (uint8_t*)vt[i];
        if (!Readable(fn, 0x100) || !LooksLikeStageSwitch(fn)) continue;
        if (found < 0) found = i;
        if (i == preferred) preferredHit = true;
        ++hits;
    }
    if (preferredHit) found = preferred;
    if (hits > 1) Log("FrameStageNotify: %d candidates, using %d", hits, found);
    return found;
}

// Swap our function into Source2Client's table. Works whether or not HLAE already hooked the same function.
static bool HookFrameStage() {
    void** vt = *(void***)Game::client;
    if (!Readable(vt, 90 * sizeof(void*))) { Log("ERROR: client function table not readable"); return false; }
    int idx = FindFrameStageIndex(vt, Game::I.clientFrameStageNotify);
    if (idx >= 0 && idx != Game::I.clientFrameStageNotify) {
        Log("FrameStageNotify is at %d in this CS2 build (config says %d), using %d", idx, Game::I.clientFrameStageNotify, idx);
        Game::I.clientFrameStageNotify = idx;
    } else if (idx < 0) {
        Log("WARNING: couldn't recognise FrameStageNotify, trying index %d from the config (checked at run time)", Game::I.clientFrameStageNotify);
    }
    void** slot = &vt[Game::I.clientFrameStageNotify];
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) { Log("ERROR: VirtualProtect failed"); return false; }
    g_oFrameStage = (FrameStageFn)*slot;
    *slot = (void*)&hkFrameStage;
    VirtualProtect(slot, sizeof(void*), old, &old);
    Log("FrameStageNotify hooked (index %d)", Game::I.clientFrameStageNotify);
    return true;
}

// What the game discovery found, for the Status tab. Skins parts depend on CS2's skin code and may need an update of
// DemoTools2 after a CS2 update; everything else should keep working.
static void ReportFeatures() {
    g_gameBuild = Game::ClientVersion();
    Log("CS2 build %d", g_gameBuild);
    const int misses = Game::SchemaMisses();
    SetFeature("core", "Game connection", Game::InterfaceFallbacks() ? Feat_Limited : Feat_Ok,
               Game::InterfaceFallbacks() ? "Some game interfaces have a new version number (CS2 update) - most things should still work." : "");
    SetFeature("skins", "Skins", misses > 3 ? Feat_Limited : Feat_Ok,
               misses ? std::to_string(misses) + " game field(s) not found (CS2 update) - some skin parts may not work." : "");
    SetFeature("refresh", "Skin rebuild", Game::CanRegenerate() ? Feat_Ok : Feat_Limited,
               Game::CanRegenerate() ? "" : "Not found - skins on guns already on screen update after Refresh now.");
    SetFeature("knives", "Knife models", Game::CanSwapModels() ? Feat_Ok : Feat_Off,
               Game::CanSwapModels() ? "" : "Not found - knives keep their model (finishes still work).");
    SetFeature("gloves", "Gloves", Game::CanSetBodyGroups() ? Feat_Ok : Feat_Limited,
               Game::CanSetBodyGroups() ? "" : "Body groups not found - gloves may not show on players with default gloves.");
    SetFeature("agents", "Agents", Game::CanSetModel() ? Feat_Ok : Feat_Off, Game::CanSetModel() ? "" : "SetModel not found - agents are off.");
    SetFeature("firstperson", "First-person skins", Game::CanFixHudModels() ? Feat_Ok : Feat_Limited,
               Game::CanFixHudModels() ? "" : "Not found - first person can show the old skin until the gun is drawn again.");
    SetFeature("demoversion", "Old-demo protection", Game::CanCheckDemoVersion() && Game::DemoFileVerified() ? Feat_Ok : Feat_Limited,
               Game::CanCheckDemoVersion() ? (Game::DemoFileVerified() ? "" : "Using the built-in demo function number.")
                                           : "Not found - knife/glove/agent models nobody in a demo has are only used in current demos.");
}

static DWORD WINAPI Start(LPVOID) {
    LogInit(g_self);
    Log("DemoTools2 " DEMOTOOLS2_VERSION " starting");
    CrashGuardInstall();

    // -insecure keeps the game off VAC-secured servers. Without it, do nothing at all.
    if (!std::strstr(GetCommandLineA(), "-insecure")) {
        Log("REFUSED: CS2 was not started with -insecure. Launch CS2 through HLAE (it adds -insecure) and load DemoTools2 there.");
        MessageBoxA(nullptr, "DemoTools2 only runs in CS2 started by HLAE with -insecure.\nNothing was changed.", "DemoTools2", MB_ICONWARNING);
        return 0;
    }
    for (int i = 0; i < 600 && !(GetModuleHandleA("client.dll") && GetModuleHandleA("engine2.dll")); ++i) Sleep(100);

    {
        std::lock_guard<std::mutex> guard(Skins::lock);
        Skins::Load();
    }
    // The window comes first and needs nothing from CS2's code: whatever a game update breaks, the menu still opens
    // and its Status tab says what is off.
    SetFeature("menu", "Menu window", Feat_Ok);
    if (CrashedLastTime("menu-setup") || CrashedLastTime("menu-frame")) {
        Log("menu is off this session (it crashed last time); skins from DemoTools2.cfg still apply");
        SetFeature("menu", "Menu window", Feat_Off, "Crashed last time - off for this session.");
    } else if (!Render::Install()) {
        Log("the menu is unavailable; skins from DemoTools2.cfg still apply");
        SetFeature("menu", "Menu window", Feat_Off, "Could not create the window.");
    }

    bool gameOk;
    {
        Step step("start-up: reading the game");
        gameOk = Game::Init();
    }
    if (!gameOk) {
        Log("ERROR: game interfaces not found, DemoTools2 is inactive");
        SetFeature("core", "Game connection", Feat_Off, "This CS2 version's interfaces weren't found. Skins and camera are off.");
        std::lock_guard<std::mutex> guard(Skins::lock);
        Skins::lastStatus = "Not available in this CS2 version - see the Status tab";
        return 0;
    }
    ReportFeatures();
    {
        Step step("start-up: camera");
        Calc::Init();
    }
    if (CrashedLastTime("frame-stage")) {
        g_skinsOff = true;
        Log("skin changes are off this session (they crashed last time). Send DemoTools2.log to the developer.");
        SetFeature("skins", "Skins", Feat_Off, "Crashed last time - off for this session. Restart CS2 to try again.");
    }
    SetFeature("hook", "Game hook", Feat_Limited, "Checking...");
    if (!HookFrameStage()) {
        Log("ERROR: DemoTools2 is inactive");
        SetFeature("hook", "Game hook", Feat_Off, "Could not hook the game's frame function.");
        return 0;
    }
    Log("ready - play a demo and press Insert");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        if (HANDLE t = CreateThread(nullptr, 0, Start, nullptr, 0, nullptr)) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        LogClosing();   // the process is exiting (not an unload)
    }
    return TRUE;
}
