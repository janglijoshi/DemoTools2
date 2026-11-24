// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
// DemoTools2's own POV HUD - see pov.h. Written from DemoTools2's own research of client.dll:
//  - GetLocalPlayerController(slot) / GetLocalPlayerPawn(slot) read two arrays indexed by split-screen slot;
//  - "is this controller HLTV" = controller->m_bIsLocalPlayerController ? (global "watching as GOTV" byte)
//                                                                       : controller->m_bIsHLTV,
//    and that test decides between the spectator HUD and a player's own HUD.
// So for a GOTV demo: slot 0 = the watched player's controller, the local flag moved with it, GOTV byte cleared.
// Everything is put back every time it stops (off, demo stopped, demo jumped, target gone).
#include "pov.h"
#include "console.h"
#include "game.h"
#include "skins.h"
#include "util.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <vector>

namespace Pov {

static std::atomic<int> g_mode{0};
static std::mutex g_statusLock;
static std::string g_status = "off";

// What we changed, so it can be put back exactly. Entities are kept as handles: they can be deleted under us.
struct Saved {
    bool active = false;
    uint32_t gotvCtrl = 0;          // the real local controller (the GOTV viewer)
    uint32_t targetCtrl = 0;        // what we put in slot 0
    uint32_t targetPawn = 0;
    uint8_t hltvByte = 1;
};
static Saved g_saved;
static bool g_swapped = false;   // a swap happened since POV was switched on (so switching off tidies up)

static void SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> g(g_statusLock);
    if (s != g_status) Log("POV: %s", s.c_str());
    g_status = s;
}

static ptrdiff_t Field(const char* cls, const char* name) {
    ptrdiff_t o = 0;
    return Game::SchemaField(cls, name, o) ? o : 0;
}

// The two controller flags POV writes. The code patterns give one offset each; CS2's schema gives them by name.
// After a CS2 update the patterns can land on other code, and writing 1 to the wrong byte of a controller breaks
// things far from the HUD (rendering, the hands). So the schema has the last word, and if neither agrees POV stays off.
struct Flags { bool ok = false; ptrdiff_t isLocal = 0, isHltv = 0; std::string why; };
static const Flags& F() {
    static Flags f;
    static bool done = false;
    if (done) return f;
    done = true;
    const Game::LocalSlots& L = Game::Locals();
    const ptrdiff_t sLocal = Field("CBasePlayerController", "m_bIsLocalPlayerController");
    const ptrdiff_t sHltv = Field("CBasePlayerController", "m_bIsHLTV");
    f.isLocal = sLocal ? sLocal : L.isLocal;
    f.isHltv = sHltv ? sHltv : L.isHltv;
    if (sLocal && L.isLocal && sLocal != L.isLocal)
        Log("POV: 'local controller' field: code says 0x%x, schema says 0x%x - using the schema", (unsigned)L.isLocal, (unsigned)sLocal);
    if (sHltv && L.isHltv && sHltv != L.isHltv)
        Log("POV: 'is HLTV' field: code says 0x%x, schema says 0x%x - using the schema", (unsigned)L.isHltv, (unsigned)sHltv);
    if (!L.controllers || !L.hltv) f.why = "the local-player code wasn't found in this CS2 build";
    else if (!f.isLocal || !f.isHltv) f.why = "the controller fields weren't found in this CS2 build";
    f.ok = f.why.empty();
    Log("POV: fields m_bIsLocalPlayerController 0x%x, m_bIsHLTV 0x%x%s%s", (unsigned)f.isLocal, (unsigned)f.isHltv, f.ok ? "" : " - ",
        f.why.c_str());
    return f;
}
struct Offs {
    bool ok = false;
    ptrdiff_t ctrlPawn = 0, pawnObs = 0, obsTarget = 0, pawnCtrl = 0;
};
static const Offs& O() {
    static Offs o;
    static bool done = false;
    if (done) return o;
    done = true;
    o.ctrlPawn = Field("CBasePlayerController", "m_hPawn");
    o.pawnObs = Field("C_BasePlayerPawn", "m_pObserverServices");
    o.obsTarget = Field("CPlayer_ObserverServices", "m_hObserverTarget");
    o.pawnCtrl = Field("C_BasePlayerPawn", "m_hController");
    o.ok = o.ctrlPawn && o.pawnObs && o.obsTarget && o.pawnCtrl;
    Log("POV: fields m_hPawn 0x%x, m_pObserverServices 0x%x, m_hObserverTarget 0x%x, m_hController 0x%x%s", (unsigned)o.ctrlPawn,
        (unsigned)o.pawnObs, (unsigned)o.obsTarget, (unsigned)o.pawnCtrl, o.ok ? "" : " - something MISSING");
    return o;
}

// The entity's handle, only if it really is a live entity (its handle leads back to it); 0 otherwise.
static uint32_t SafeHandle(void* e) {
    if (!Game::LooksLikePtr(e)) return 0;
    void* id = Readable((uint8_t*)e + 0x10, 8) ? *(void**)((uint8_t*)e + 0x10) : nullptr;
    if (!Game::LooksLikePtr(id) || !Readable((uint8_t*)id + 0x10, 4)) return 0;
    const uint32_t h = Game::HandleOf(e);
    return h != 0xffffffffu && Game::FromHandleExact(h) == e ? h : 0;
}

template <class T> static T Rd(void* base, ptrdiff_t off) {
    uint8_t* p = (uint8_t*)base + off;
    return Readable(p, sizeof(T)) ? *(T*)p : T{};
}
template <class T> static void Wr(void* base, ptrdiff_t off, T v) {
    uint8_t* p = (uint8_t*)base + off;
    if (Readable(p, sizeof(T))) *(T*)p = v;
}

// Put slot 0, the flags and the GOTV byte back as they were.
static void Restore(const char* why) {
    if (!g_saved.active) return;
    const Game::LocalSlots& L = Game::Locals();
    void* gotv = Game::FromHandleExact(g_saved.gotvCtrl);
    void* target = Game::FromHandleExact(g_saved.targetCtrl);
    const ptrdiff_t isLocal = F().isLocal;
    if (target && isLocal) Wr<uint8_t>(target, isLocal, 0);
    if (L.controllers) L.controllers[0] = gotv;   // nullptr if the GOTV controller is gone: the game sets it again
    if (gotv && isLocal) Wr<uint8_t>(gotv, isLocal, 1);
    if (L.hltv) *L.hltv = g_saved.hltvByte;
    g_saved = Saved{};
    if (why) SetStatus(why);
}

// The player the GOTV viewer watches now (controller + pawn), from the viewer's observer target.
static bool Watched(void* gotvCtrl, void*& ctrl, void*& pawn) {
    const Offs& o = O();
    ctrl = pawn = nullptr;
    if (!o.ok || !gotvCtrl) return false;
    void* viewer = Game::FromHandle(Rd<uint32_t>(gotvCtrl, o.ctrlPawn));
    if (!viewer) return false;
    void* obs = Rd<void*>(viewer, o.pawnObs);
    if (!Game::LooksLikePtr(obs)) return false;
    pawn = Game::FromHandle(Rd<uint32_t>(obs, o.obsTarget));
    if (!pawn || pawn == viewer) return false;
    ctrl = Game::FromHandle(Rd<uint32_t>(pawn, o.pawnCtrl));
    return ctrl != nullptr && ctrl != gotvCtrl;
}

// The render window: the frame stage during which CS2 sets up the view and draws the world and the first-person
// hands, and the stage after that. Inside it the real (GOTV) state is put back, so the camera and the hands are
// the demo's own; outside it (where the HUD reads its numbers) the watched player is the local player.
// Found at run time: the stage notified last before the view hook runs, and the first one after it. It is learned
// all the time, not once: a CS2 or HLAE update (HLAE 2.193 added stream previews, which render extra views) can
// move it, and a window learned once would then put the swap where the hands are drawn.
static int g_lastStage = -1, g_renderStart = -1, g_renderEnd = -1, g_candStart = -1, g_stable = 0;
static int g_viewsThisFrame = 0, g_viewsPerFrame = 0;
static ULONGLONG g_lastView = 0;

static void Restore(const char* why);
void OnViewSetup() {
    if (g_viewsThisFrame++ == 0) g_candStart = g_lastStage;
    g_lastView = GetTickCount64();
    // Every view is set up with the demo's own state, also a second view in the same frame (HLAE streams and
    // stream preview render one view per stream): put back here, swapped again once this view is done.
    if (g_saved.active && g_mode.load() == 1) Restore(nullptr);
}

static void Detect(int stage) {
    if (!g_viewsThisFrame) return;
    static int prevStart = -2, prevEnd = -2;
    g_viewsPerFrame = g_viewsThisFrame;
    g_viewsThisFrame = 0;
    g_stable = (g_candStart == prevStart && stage == prevEnd) ? g_stable + 1 : 0;
    prevStart = g_candStart;
    prevEnd = stage;
    if (g_stable < 8 || g_candStart < 0 || g_candStart == stage) return;
    if (g_candStart == g_renderStart && stage == g_renderEnd) return;
    const bool moved = g_renderStart >= 0;
    Restore(nullptr);
    g_renderStart = g_candStart;
    g_renderEnd = stage;
    Log("POV: render window %sis between frame stages %d and %d (%d view(s) per frame)", moved ? "MOVED - now " : "",
        g_renderStart, g_renderEnd, g_viewsPerFrame);
}

// ------------------------------------------------------------------ follow the chosen player
// POV is tied to one player (the one watched when it was switched on). While that player is alive, the demo watches
// them with their own HUD. When they die: the death is shown for a moment, then the spectator HUD comes back and the
// demo spectates a living teammate (anyone alive if no teammate is). When they are alive again (next round), the
// demo goes back to them and their HUD returns.
static uint64_t g_lockXuid = 0;
static std::string g_lockName;
static bool g_lockAlive = false, g_canSwap = false;
static ULONGLONG g_deadSince = 0, g_lastSpec = 0;
static int g_specTries = 0;
static bool g_keepLock = false;   // LockTo was used: keep the player even while POV is switched off

struct PlayerOffs { ptrdiff_t health = 0, life = 0; };
static const PlayerOffs& PO() {
    static PlayerOffs p;
    static bool done = false;
    if (!done) {
        done = true;
        p.health = Field("C_BaseEntity", "m_iHealth");
        p.life = Field("C_BaseEntity", "m_lifeState");
    }
    return p;
}
struct Player { void* ctrl = nullptr; void* pawn = nullptr; int index = 0; uint64_t xuid = 0; int team = 0; bool alive = false; std::string name; };
static std::vector<Player> AllPlayers() {
    std::vector<Player> out;
    for (int i = 1; i <= 64; ++i) {
        void* c = Game::EntityAt(i);
        if (!c) continue;
        const char* dn = Game::DesignerName(c);
        if (!dn || std::strcmp(dn, "cs_player_controller") != 0) continue;
        Player p;
        p.ctrl = c;
        p.index = i;
        p.xuid = Rd<uint64_t>(c, ::O.ctrl_steamID);
        p.team = Rd<uint8_t>(c, ::O.ent_iTeamNum);
        char nm[129] = {};
        if (Readable((uint8_t*)c + ::O.ctrl_iszPlayerName, 128)) std::memcpy(nm, (uint8_t*)c + ::O.ctrl_iszPlayerName, 128);
        for (char& ch : nm) if (ch == '"' || ch == ';') ch = ' ';
        p.name = nm;
        p.pawn = Game::FromHandle(Rd<uint32_t>(c, ::O.ctrl_hPlayerPawn));
        if (p.pawn && PO().health) p.alive = Rd<int32_t>(p.pawn, PO().health) > 0 && (!PO().life || Rd<uint8_t>(p.pawn, PO().life) == 0);
        if (p.team == 2 || p.team == 3) out.push_back(p);
    }
    return out;
}
static void SpecPlayer(const Player& p) {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastSpec < 700) return;
    g_lastSpec = now;
    char cmd[200];
    // by name first; if the demo doesn't move after a few tries, by the player's slot number
    if (g_specTries++ % 4 < 2) std::snprintf(cmd, sizeof cmd, "spec_player \"%s\"", p.name.c_str());
    else std::snprintf(cmd, sizeof cmd, "spec_player %d", p.index - 1);
    Game::ExecuteCommand(cmd);
}

// Once per frame, at its start (real GOTV state in place). Decides whether this frame gets the swap.
static void Follow() {
    g_canSwap = false;
    const Game::LocalSlots& L = Game::Locals();
    if (!L.controllers || !Game::IsPlayingDemo()) return;
    void* gotv = L.controllers[0];
    void *wctrl = nullptr, *wpawn = nullptr;
    const bool watching = gotv && Watched(gotv, wctrl, wpawn);
    const auto players = AllPlayers();
    if (!g_lockXuid) {   // lock onto whoever is watched now
        if (!watching) { SetStatus("on - watch a player in first person to start"); return; }
        for (const Player& p : players)
            if (p.ctrl == wctrl) { g_lockXuid = p.xuid; g_lockName = p.name; Log("POV: following %s", p.name.c_str()); }
        if (!g_lockXuid) return;
    }
    const Player* me = nullptr;
    for (const Player& p : players) if (p.xuid == g_lockXuid) me = &p;
    if (!me) { SetStatus("on - " + g_lockName + " isn't in the demo right now"); return; }
    const ULONGLONG now = GetTickCount64();
    if (me->alive) {
        if (!g_lockAlive) { g_lockAlive = true; g_specTries = 0; }
        if (watching && wpawn == me->pawn) {
            g_canSwap = true;
            g_specTries = 0;
            SetStatus("on - " + me->name + "'s own HUD");
        } else {
            SetStatus("on - going back to " + me->name);
            SpecPlayer(*me);
        }
        return;
    }
    // dead
    if (g_lockAlive) { g_lockAlive = false; g_deadSince = now; g_specTries = 0; }
    if (now - g_deadSince < 2500) { SetStatus("on - " + me->name + " died"); return; }   // let the death be seen
    const Player* next = nullptr;
    for (const Player& p : players) if (p.alive && p.team == me->team && (!next || p.pawn == wpawn)) next = &p;
    if (!next) for (const Player& p : players) if (p.alive && (!next || p.pawn == wpawn)) next = &p;
    if (!next) { SetStatus("on - " + me->name + " is dead, nobody alive to watch"); return; }
    if (watching && wpawn == next->pawn) { g_specTries = 0; SetStatus("on - " + me->name + " is dead: spectating " + next->name); return; }
    SetStatus("on - " + me->name + " is dead: switching to " + next->name);
    SpecPlayer(*next);
}

static void Apply(int mode) {
    if (mode != 3 && !g_canSwap) { Restore(nullptr); return; }
    if (!Game::IsPlayingDemo()) { Restore("waiting for a demo"); return; }
    const Game::LocalSlots& L = Game::Locals();
    const Flags& fl = F();
    if (!fl.ok) { SetStatus("not available in this CS2 build: " + fl.why); return; }
    Step step("POV HUD");

    // Who is the GOTV viewer? Our saved one, unless the game put a new one in slot 0 (demo jump, reconnect).
    void* slot0 = L.controllers[0];
    void* gotv = g_saved.active ? Game::FromHandleExact(g_saved.gotvCtrl) : slot0;
    if (g_saved.active && g_saved.targetCtrl && slot0 != Game::FromHandleExact(g_saved.targetCtrl)) {
        // the game replaced slot 0 itself: start over from what it put there
        Restore(nullptr);
        gotv = L.controllers[0];
    }
    if (!gotv || !SafeHandle(gotv)) { SetStatus("no local controller yet"); return; }
    if (!g_saved.active) {
        // Before changing anything, check that what was found really is what it should be: the controller in slot 0
        // is marked local, and the GOTV byte is a bool. If not, this CS2 build moved something: don't write at all.
        const uint8_t local = Rd<uint8_t>(gotv, fl.isLocal), hl = *L.hltv;
        if (local != 1 || hl > 1) {
            static bool logged = false;
            if (!logged) { logged = true; Log("POV: check failed (local flag %u, GOTV byte %u) - POV stays off in this CS2 build", local, hl); }
            SetStatus("not available in this CS2 build (its local-player data moved) - see DemoTools2.log");
            return;
        }
        if (!Rd<uint8_t>(gotv, fl.isHltv) && !hl) { SetStatus("this isn't a GOTV demo (already a POV demo)"); return; }
    }

    void *ctrl = nullptr, *pawn = nullptr;
    if (mode != 3 && (!Watched(gotv, ctrl, pawn) || !SafeHandle(ctrl))) {
        Restore(nullptr);
        return;
    }
    // Never make a dead player the local one. The check at the start of the frame can be a moment too early: the
    // death arrives with this frame's network update, and a dead local player is what makes CS2 start its own death
    // screen (the grey, blurred look) - which then stays.
    if (mode != 3 && pawn && PO().health &&
        (Rd<int32_t>(pawn, PO().health) <= 0 || (PO().life && Rd<uint8_t>(pawn, PO().life) != 0))) {
        g_canSwap = false;
        Restore(nullptr);
        return;
    }

    // Target changed: put the previous one back first, so only one controller is ever marked local.
    if (g_saved.active && ctrl && SafeHandle(ctrl) != g_saved.targetCtrl) Restore(nullptr);
    if (!g_saved.active && ctrl && Rd<uint8_t>(ctrl, fl.isLocal) != 0) {   // another player marked local: not ours to change
        SetStatus("not available in this CS2 build (the watched player is already marked local) - see DemoTools2.log");
        return;
    }
    if (!g_saved.active) {
        g_saved.active = true;
        g_saved.gotvCtrl = SafeHandle(gotv);
        g_saved.hltvByte = *L.hltv;
        g_saved.targetCtrl = ctrl ? SafeHandle(ctrl) : 0;
        g_saved.targetPawn = pawn ? SafeHandle(pawn) : 0;
        if (ctrl) {
            Wr<uint8_t>(gotv, fl.isLocal, 0);
            Wr<uint8_t>(ctrl, fl.isLocal, 1);
            L.controllers[0] = ctrl;
            g_swapped = true;
        }
        if (!ctrl) SetStatus("on (test 3: only the GOTV flag cleared)");
    }
    // keep it so every frame (the game may write some of it again)
    *L.hltv = 0;
    if (ctrl) {
        L.controllers[0] = ctrl;
        Wr<uint8_t>(ctrl, fl.isLocal, 1);
    }
}

// When the swap is in place. Tested: CS2 works out the GOTV camera and the first-person hands inside the frame
// stages (swapped there: camera frozen, no hands), while the HUD reads its numbers after the last stage of a frame
// and before the first stage of the next (the "render window", found at run time). So modes 1 and 2 swap only
// there: from right after the last stage until the next frame's first stage.
// The radar: while it thinks we spectate, CS2 shows every player on it (cl_radar_show_all_players_when_spectating,
// default 1). A player's own POV only shows enemies their team has spotted, so it's 0 while POV is on, 1 after.
static void Radar(bool pov) {
    static int set = -1;
    if (set == (pov ? 1 : 0)) return;
    if (set == -1 && !pov) return;   // never changed: leave the player's own setting alone
    set = pov ? 1 : 0;
    Game::ExecuteCommand(pov ? "cl_radar_show_all_players_when_spectating 0" : "cl_radar_show_all_players_when_spectating 1");
}

void OnFrameStage(int stage) {
    Detect(stage);
    Radar(g_mode.load() == 1 || g_mode.load() == 2);
    struct Last { int s; ~Last() { g_lastStage = s; } } last{stage};
    const int mode = g_mode.load();
    if (mode == 0) {
        Restore(nullptr);
        if (!g_keepLock) g_lockXuid = 0;   // next time it starts with whoever is watched then
        g_lockAlive = false;
        return;
    }
    if (mode == 3) { Apply(3); return; }
    if (g_renderStart < 0) { SetStatus("on - waiting a moment to learn the frame timing (play the demo)"); return; }
    if (stage == g_renderEnd) {   // a new frame starts: the demo's real state for the game's own work
        Restore(nullptr);
        if (GetTickCount64() - g_lastView > 2000) {   // no view this long: the swap would have no "after the camera"
            g_canSwap = false;
            SetStatus("on - waiting for the demo's camera (CS2 isn't running DemoTools2's view hook right now)");
            return;
        }
        Follow();
    }
}
// Mode 1: swapped only once CS2 has worked out this frame's camera (end of the view hook) until the next frame
// starts - the camera (frozen when swapped earlier) and the hands stay the demo's, the HUD reads after that.
// Mode 2 (test): swapped from the end of the frame's last stage, and DemoTools2 sets the camera from the player's eyes.
void AfterFrameStage(int stage) {
    const int mode = g_mode.load();
    if (mode == 2 && g_renderStart >= 0 && stage == g_renderStart) Apply(mode);
}
void OnViewDone() {
    if (g_mode.load() == 1 && g_renderStart >= 0) Apply(1);
}

// ------------------------------------------------------------------ for the recorder (game thread)
std::vector<PlayerSlot> Players() {
    std::vector<PlayerSlot> out;
    for (const Player& p : AllPlayers()) out.push_back({p.index - 1, p.xuid, p.team, p.alive, p.name});
    return out;
}
uint64_t WatchedXuid() {
    const Game::LocalSlots& L = Game::Locals();
    if (!L.controllers) return 0;
    void* gotv = g_saved.active ? Game::FromHandleExact(g_saved.gotvCtrl) : L.controllers[0];
    void *c = nullptr, *pw = nullptr;
    if (!gotv || !Watched(gotv, c, pw)) return 0;
    return Rd<uint64_t>(c, ::O.ctrl_steamID);
}
bool SpecXuid(uint64_t xuid) {
    if (WatchedXuid() == xuid) { g_specTries = 0; return true; }
    for (const Player& p : AllPlayers())
        if (p.xuid == xuid) { SpecPlayer(p); break; }
    return false;
}
void LockTo(uint64_t xuid) {
    g_keepLock = xuid != 0;
    g_lockXuid = xuid;
    g_lockName.clear();
    for (const Player& p : AllPlayers()) if (p.xuid == xuid) g_lockName = p.name;
    g_lockAlive = false;
    g_deadSince = 0;
}

bool SwapActive() { return g_saved.active; }

bool CameraPawn(uint32_t& handle) {
    if (g_mode.load() != 2 || !g_saved.active || !g_saved.targetPawn) return false;
    handle = g_saved.targetPawn;
    return true;
}

void Stop() { Restore(g_mode.load() ? "demo stopped - put back" : nullptr); }

// Switched off after a swap: CS2 may still hold first-person models or screen effects it made while the watched
// player was the local one. Jumping the demo to where it is makes CS2 build them again from the demo itself.
static void TidyUp() {
    if (!g_swapped) return;
    g_swapped = false;
    Log("POV: switched off - reloading the view at the current tick");
    Skins::RequestSeek();
}
void ReloadView() {
    Log("POV: reload view asked for");
    Skins::RequestSeek();
}

int Mode() { return g_mode.load(); }
std::string Status() {
    std::lock_guard<std::mutex> g(g_statusLock);
    return g_status;
}

static void Cmd(const Console::Args& a) {
    if (a.size() < 2) {
        Console::Print("dt2_pov %d - %s", g_mode.load(), Status().c_str());
        Console::Print("  dt2_pov 1  on: their own HUD; the camera and hands stay the demo's (swapped only after the camera is set)");
        Console::Print("  dt2_pov 2  test: like 1, and DemoTools2 puts the camera in the player's eyes itself");
        Console::Print("  dt2_pov 3  only clear 'watching as GOTV' (test)");
        Console::Print("  dt2_pov 0  off");
        return;
    }
    const int m = std::atoi(a[1].c_str());
    if (m < 0 || m > 3) { Console::Print("dt2_pov: 0, 1, 2 or 3"); return; }
    if (m && CrashedLastTime("POV HUD")) Console::Print("dt2_pov: note - CS2 crashed during the POV HUD last time; trying again");
    (void)O();
    if (m && !F().ok) { Console::Print("dt2_pov: not available in this CS2 build: %s", F().why.c_str()); return; }
    SetMode(m, true);
    Console::Print("dt2_pov %d", m);
}

void SetMode(int m, bool tidy) {
    if (m < 0 || m > 3) return;
    const int was = g_mode.exchange(m);
    if (!m) {
        SetStatus("off");
        if (was && tidy) TidyUp();
        if (!tidy) g_swapped = false;
    }
}

void Init() {
    Console::Add("dt2_pov", "DemoTools2's own POV HUD (test): dt2_pov 1|2|3|0", &Cmd);
}

}  // namespace Pov
