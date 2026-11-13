// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct GunSkin {
    int paint = 0;          // paint kit index
    float wear = 0.01f;
    int seed = 0;
    int stattrak = -1;      // -1 = off
    std::string nametag;
    int stickers[5] = {};   // sticker kit id per slot, 0 = empty (guns only)
    int charm = 0;          // keychain (charm) id, 0 = none (guns only)
    int charmSeed = 0;
};

struct ItemRule {           // knife or gloves: which model, and its finish
    uint16_t def = 0;       // 0 = not set
    GunSkin skin;
};

// What a player gets on one side (or on both).
struct Loadout {
    std::map<uint16_t, GunSkin> guns;   // weapon definition index -> skin
    ItemRule knife;
    ItemRule gloves;
    std::string agent;                  // agent model path ("agents/models/....vmdl"), empty = keep
    bool Empty() const { return guns.empty() && !knife.def && !gloves.def && agent.empty(); }
};

enum Side { Side_Both = 0, Side_T = 1, Side_CT = 2 };

// The "both sides" loadout is the PlayerRules itself; t / ct override it item by item while the player is on that team.
struct PlayerRules : Loadout {
    std::string name;                   // last name seen, for the menu only
    Loadout t, ct;
    Loadout& For(int side) { return side == Side_T ? t : side == Side_CT ? ct : *this; }
    const Loadout* Team(int team) const { return team == 2 ? &t : team == 3 ? &ct : nullptr; }
    bool Empty() const { return Loadout::Empty() && t.Empty() && ct.Empty(); }
};

struct PlayerInfo {         // a player in the demo right now (snapshot for the menu)
    uint64_t xuid = 0;
    std::string name;
    int team = 0;
    std::vector<uint16_t> guns;   // definition indexes of the weapons they hold
    uint16_t knife = 0;           // their knife's definition index
    uint16_t gloves = 0;          // their gloves' definition index (0 = default gloves)
    std::string agent;            // their player model
};

enum RefreshMode { Refresh_InPlace = 1, Refresh_Seek = 2, Refresh_Both = 3 };

struct Settings {
    int menuKey = 0x2D;     // VK_INSERT
    int refresh = Refresh_InPlace;
    bool enabled = true;
    bool knifeModels = true;   // change the knife model (not just the finish)
    bool gloves = true;
    bool agents = true;        // change player models (agents)
    bool anyModel = true;      // allow knife/glove models the demo never loaded (switches CS2 to local animation data)
    bool anyModelOldDemos = false;   // ...also in demos from ANY older CS2 version (CS2 crashes on most of those)
    int anyModelVersionsBack = 5;    // ...in demos at most this many CS2 versions older (each version is tried once, see README)
};

namespace Skins {
extern std::mutex lock;                      // guards everything below
extern std::map<uint64_t, PlayerRules> rules;
extern std::vector<PlayerInfo> players;
extern Settings settings;
extern bool demoPlaying;
extern std::string lastStatus;
extern std::string knifeNote;                // why a knife rule is waiting, if it is
extern std::vector<uint16_t> knivesInDemo;   // knife models some player in this demo really has
extern std::vector<uint16_t> glovesInDemo;   // glove types some player in this demo really wears
extern std::vector<std::string> agentsInDemo; // player models some player in this demo really has
extern int demoCurrent;                      // 1: demo from this CS2 version, 0: older version, -1: unknown
extern bool anyModelBlocked;                 // this demo can't use knife/glove models it doesn't have

bool IsKnife(uint16_t def);
bool IsGloves(uint16_t def);

void Load();
void Save();
void RequestSeek();

void OnFrameStage(int stage);                // game thread, every FrameStageNotify
}
