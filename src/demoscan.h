// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Reads a CS2 .dem file (no game needed) and lists its kills: who killed whom, when (demo tick), with what.
// Written for DemoTools2 from the public CS2 demo format (Valve's demo.proto / netmessages.proto / gameevents.proto).
namespace DemoScan {

struct Kill {
    int tick = 0;            // demo tick of the kill
    int round = 0;           // 1-based round (0: warmup / before the first round)
    int attacker = -1;       // player slot (controller entity index - 1), -1 none
    int victim = -1;
    int assister = -1;
    std::string weapon;      // "ak47", "awp", ...
    bool headshot = false, wallbang = false, noscope = false, thruSmoke = false, attackerBlind = false;
    bool teamKill = false;   // filled at run time by the caller if it knows the teams
};

struct Result {
    bool ok = false;
    std::string error;
    std::string map;
    int ticks = 0;                      // demo length in ticks
    std::vector<int> roundStarts;       // tick each round starts
    int matchStart = -1;                // tick of the match start (end of warmup), -1 if the demo has none
    int eventRounds = 0;                // round_start events seen (all, warmup included)
    std::vector<Kill> kills;
    // players seen in the demo's events (name and Steam ID; CS2 demos carry them when players leave, usually at the
    // end), may be incomplete
    struct Player { int slot = -1; std::string name; uint64_t xuid = 0; };
    std::vector<Player> players;
};

// Whole file in memory (a demo is usually 50-300 MB; read with ReadFile by the caller, or use ScanFile).
Result Scan(const uint8_t* data, size_t size);
Result ScanFile(const std::string& path);
// Only the start and the end of the file: the map (header) and the length in ticks (file info, 0 if the demo was
// cut short and has none). Fast - for telling demos apart.
bool ReadHeader(const std::string& path, std::string& map, int& ticks);

// Snappy (raw block format) - used by CS2 demo frames
bool SnappyDecompress(const uint8_t* in, size_t n, std::vector<uint8_t>& out);
}
