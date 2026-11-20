// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Campaths: a smooth camera move through keyframes, timed to the demo (CS2's game time, the same clock HLAE's
// mirv_campath uses, so paths can go both ways: files are HLAE campath XML). While a path is on and the demo is
// inside its time range, it becomes the game's view (HLAE's mirv_streams records it).
namespace Campath {

struct Key {
    double t = 0;            // game time (seconds)
    int tick = -1;           // demo tick when it was added (for "go to"), -1 if unknown
    float pos[3] = {0, 0, 0};
    float ang[3] = {0, 0, 0};   // pitch, yaw, roll (degrees)
    float fov = 90.f;
    bool cut = false;        // hold this view until the next key, then cut to it (a new shot starts there)
};

// A sequence: camera points made with the demo paused, each with how many demo ticks the camera takes to the next
// one (0 = worked out from the distance and the turn). Generate turns it into a real, timed path that starts at the
// sequence's start tick.
struct Point {
    float pos[3] = {0, 0, 0};
    float ang[3] = {0, 0, 0};
    float fov = 90.f;
    int ticks = 0;           // to the next point; 0 = auto
    bool cut = false;        // hold here for `ticks` (auto: Hold), then cut to the next point
};
struct Sequence {
    std::vector<Point> points;
    int startTick = -1;      // the demo tick the first point was added at (the path starts there)
    double startTime = 0;    // the game time at that tick
    float speed = 300.f;     // auto timing: camera speed, units per second
    float turn = 90.f;       // auto timing: turning speed, degrees per second
    int minTicks = 32;       // auto timing: never quicker than this between two points
    int holdTicks = 128;     // auto timing for a cut: how long the shot holds before it
    int selected = -1;
};

enum Interp { Smooth = 0, Linear = 1 };

struct Settings {
    bool enabled = false;    // use the path when the demo is inside it
    bool hold = false;       // before / after the path: stay on its first / last view
    bool easeEnds = true;    // start and stop gently (no sudden start)
    int posInterp = Smooth, angInterp = Smooth, fovInterp = Smooth;
    int aim = 0;             // 0: keyframe angles, 1: look at a player
    uint64_t aimXuid = 0;    // the player to look at
    int aimPart = 0;         // 0 eyes, 1 chest, 2 feet
    bool hideHud = true;     // cl_drawhud 0 while the path plays
    bool hideViewmodel = true;
};

struct Pov {                 // HLAE's mirv_pov (POV HUD on a GOTV demo), when the HLAE build has it
    bool on = false, scoreboard = false, buymenu = false;
};

extern std::mutex lock;      // guards everything below (window thread and game thread)
extern std::vector<Key> keys;   // sorted by time
extern Settings set;
extern Pov pov;
extern int selected;         // selected key in the window (-1: none)
extern Sequence seq;

// Status for the window (copied under the lock).
struct Live {
    bool playing = false, previewing = false;
    double now = 0;          // game time
    double pathTime = 0;     // where on the path the camera is
    float camPos[3] = {0, 0, 0};
    bool camKnown = false;
    std::string note;
};
Live Status();

void Init();                 // console command (start-up thread)

// Game thread, once per frame from the view hook: the path's view at this moment. aimAt: where the chosen player is
// (nullptr if not found). Returns false when the path doesn't drive the camera now.
bool Evaluate(double gameTime, const float* aimAt, float pos[3], float ang[3], float& fov);
void AfterFrame(bool playing);   // hides / shows the HUD as set (game thread)

// Editing (window or console). Each takes the lock itself and keeps an undo step.
bool AddKeyHere(std::string* why = nullptr);   // the current view at the current demo time
bool SetKeyToView(int index);
void RemoveKey(int index);
void Clear();
bool Undo();
void EvenSpeed();                     // re-time the keys so the camera moves at an even speed
void SetDuration(double seconds);     // stretch / squeeze the whole path (first key stays)
void StartNow();                      // move the whole path so it starts at the current demo time
void Orbit(const float center[3], float radius, float height, float degrees, float seconds, int count);
void Dolly(float distance, float seconds);   // from the current view, straight ahead
void Preview();                       // play the path once in real time, from its start (demo may be paused)
void StopPreview();
void UpdateKey(int index, const Key& k);    // edited in the window (re-sorts; no undo step - call SaveUndoStep first)
void SaveUndoStep();
double Duration();
// Points along the path for drawing (window thread)
void Sample(int count, std::vector<float>& xyz);

// Files: <DemoTools2>\DemoTools2-campaths\<name>.xml (HLAE campath format, also loads HLAE's own files)
std::vector<std::string> SavedNames();
std::string Folder();
bool Save(const std::string& name, std::string* why = nullptr);
bool Load(const std::string& name, std::string* why = nullptr);

// Sequence (each takes the lock itself)
bool SeqAdd(std::string* why = nullptr);      // the current view as the next point (the first one also sets the start)
bool SeqSetToView(int index);
void SeqRemove(int index);
void SeqMove(int index, int dir);             // up (-1) / down (+1)
void SeqClear();
bool SeqUseCurrentStart(std::string* why = nullptr);   // the path starts at the demo tick you are on now
int SeqTicks(int index);                       // ticks from point index to the next (auto worked out), lock held
int SeqTotalTicks();                           // lock held
bool SeqGenerate(std::string* why = nullptr);  // -> keys (timed from the start tick), path switched on
void SeqFromPath();                            // the current path back into a sequence, to edit it
void SampleSequence(int count, std::vector<float>& xyz);

void QueueCommand(const std::string& cmd);   // run a game command on the game thread (next frame)
int TickFor(double gameTime);         // the demo tick for a time on the path (from the keys' ticks), -1 if unknown

// mirv_pov
bool PovAvailable();                  // the loaded HLAE has mirv_pov
void ApplyPov();                      // sends the mirv_pov commands for `pov` (window thread is fine: runs on the game thread)
}
