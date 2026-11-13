// Copyright (c) 2026 janglijoshi. All rights reserved.
// Part of DemoTools2. Use is governed by the DemoTools2 Source-Available License - see LICENSE.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Calcs: named values computed from the demo every frame - which entity (handle), a position + angles (vecang), a
// field of view (fov) - built on each other, like mirv_calc in HLAE for CS:GO. The DemoTools2 camera (dt2_cam) takes
// one vecang calc (and optionally a fov calc) and becomes the game's view, so HLAE's mirv_streams records it.
namespace Calc {

struct Vec3 { float x = 0, y = 0, z = 0; };
struct View { Vec3 pos, ang; };   // ang: pitch, yaw, roll (degrees, Source convention)

enum class Kind { Handle, VecAng, Fov };

// What the window shows (copied once per frame under the lock).
struct Preview {
    std::string name, kind, def;
    bool ok = false;
    std::string value;
};
struct CamState {
    bool enabled = false;
    std::string source, fov;    // calc names ("" = none); fov may also be a number or "game"
    float blend = 0.5f;          // seconds to blend when the source changes
    bool hideViewmodel = false;
    bool thirdPerson = true;     // switch the demo to chase cam (spec_mode 3) while the camera is on: the player's own
                                 // body and gun are drawn, and no first-person gun
    // live
    bool active = false;         // overriding the view this frame
    View out;
    float outFov = 0;
    std::string status;
};

extern std::mutex lock;          // guards everything here (game thread, window thread)
extern CamState cam;

void Init();                     // reads offsets, prepares the commands (start-up thread)
void OnGameThread();             // registers the commands and installs the view hook (first frame, game thread)
std::vector<Preview> Snapshot(); // for the window
std::vector<std::string> Names(Kind kind);
std::string Help();
uint64_t ObservedXuid();
void SetQuiet(bool quiet);       // no "added/replaced" console lines (live sliders in the window)         // the player whose eyes the demo shows right now (0 if none)

// Commands (also used by presets and the window). Each prints its result to the console.
void CmdCalc(const std::vector<std::string>& args);
void CmdCam(const std::vector<std::string>& args);
void CmdPreset(const std::vector<std::string>& args);

// Presets: DemoTools2-presets\<name>.cfg next to the DLL (plain dt2_ commands).
std::vector<std::string> PresetNames();
bool SavePreset(const std::string& name);
bool LoadPreset(const std::string& name);
bool DeletePreset(const std::string& name);
}
