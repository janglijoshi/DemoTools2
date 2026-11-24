// Copyright (c) 2026 janglijoshi
// Part of DemoTools2, released under the MIT License - see LICENSE.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "demoscan.h"

// Kill recorder: scans the demo file for a player's kills, then goes to each one and records it, one MP4 per clip
// and/or all clips joined. Two recorders, chosen in the Record tab: HLAE (mirv_streams: ffmpeg, motion blur through
// HLAE's sampler, game sound as WAV) or DTRecord, DemoTools2's own (capture.h: frames from DirectX 11, its own motion
// blur, game sound recorded with it) - for when HLAE's recording isn't updated for the newest CS2.
namespace Record {

struct Clip {
    int start = 0, end = 0;       // demo ticks recorded
    int round = 0;
    std::vector<int> kills;       // indexes into the scan's kills
    std::string label;            // file name (no extension): "03_round07_2k_ak47"
    std::string summary;          // "2 kills - ak47 (HS), ak47"
    bool on = true;
};

struct Settings {
    std::string demoPath;
    int slot = -1;                // the player: their slot in the demo (what kill events use)
    uint64_t xuid = 0;            // ... and Steam ID when known
    float before = 3.f, after = 2.f;   // seconds around each kill
    bool merge = true;            // kills close together become one clip
    int view = 0;                 // 0: POV with HUD (DemoTools2's POV HUD), 1: clean first person (no HUD)
    bool hands = true;            // clean view: show the gun and hands
    bool blur = true;
    int fps = 60;                 // output frame rate
    int samples = 8;              // frames blended into one (motion blur); the game runs at fps * samples
    float blurAmount = 0.5f;      // shutter: 0.1 (light) .. 1.0 (strong)
    int crf = 18;                 // quality (lower = better, bigger)
    bool stretch = true;          // game in 4:3 (or 5:4): stretch the video to 16:9 (1440x1080 -> 1920x1080)
    bool clips = true, combined = true;
    std::string outDir;           // "" = <DemoTools2>\recordings\<demo name>
    std::string ffmpeg;           // "" = HLAE's own ffmpeg (found next to HLAE), DemoTools2's folder or PATH
    int recorder = 0;             // Rec_Hlae or Rec_DTRecord (remembered in DemoTools2-record.cfg)
    bool sound = true;            // DemoTools2's recorder: record the game sound
    bool samePass = true;         // ... together with the picture, in real time (else in a second, real-time pass)
};

struct Scan {
    bool busy = false;
    DemoScan::Result result;      // result.ok false + error when it failed
    std::string path;
};

struct Progress {
    bool running = false;
    int clip = 0, clips = 0;      // 1-based clip being recorded
    std::string state;            // what it is doing now
    std::string post;             // post-processing (joining audio, combining)
    std::vector<std::string> done;   // finished files
};

extern std::mutex lock;           // guards set and the scan
extern Settings set;

// window thread
void StartScan(const std::string& path, bool chosen = true);   // background thread; chosen: by the user (kept for this demo)
Scan ScanState();
std::vector<Clip> BuildClips();           // for set.xuid from the scan (needs the demo playing to match the player)
struct Who { int slot; uint64_t xuid; std::string name; int team; int kills; };
std::vector<Who> PlayersWithKills();      // demo players and their kill counts (game snapshot + scan)
void Start(const std::vector<Clip>& clips);
// The Cinematics tab: record demo ticks [startTick, endTick] with the camera as it is (the campath), same video
// settings as the kill clips. name: the file name (no extension).
void StartCinematic(int startTick, int endTick, const std::string& name);
// Record from the current moment until Stop() (the view as it is: DemoTools2 camera, campath, free camera...).
// DemoTools2's recorder records the sound with the picture (or, with "second pass", afterwards over the same ticks).
void StartManual(const std::string& name);
// Freeze path record: the demo stays paused at the current moment and the whole campath is flown and recorded,
// until its last key (or Stop).
void StartFreeze(const std::string& name);
bool ManualRunning();
void Stop();
Progress Status();
std::string OutDir();
std::string FfmpegPath();                 // "" if not found
std::string FfmpegSearched();             // where it looked (for the window)
bool HlaeLoaded();
enum { Rec_Hlae = 0, Rec_DTRecord = 1 };
// The recorder a recording uses: "HLAE" or "DTRecord" when it can record now, "" if not (why: what's missing).
std::string RecorderFor(int recorder, std::string* why);
// "" or why HLAE's recording looks broken in this CS2: it recorded nothing this session, or CS2 crashed last session
// while it started recording (for the window: suggests DTRecord).
std::string HlaeWarning();
void SaveSettings();                       // the recorder choice
bool AutoFindFailed();                    // looked for the playing demo's file and didn't find it
bool GameSize(int& w, int& h);           // CS2's window (what HLAE records), false if not found
bool StretchSize(int w, int h, int& outW, int& outH);   // the 16:9 size for a 4:3 / 5:4 game size (false: not needed)

// game thread, once per frame while a demo plays
void OnFrame();
void Init();                              // console command
}
